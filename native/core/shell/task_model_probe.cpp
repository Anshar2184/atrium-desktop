// Console probe for atrium::TaskModel (not part of Atrium.Core).
//
//   atrium_task_probe.exe <seconds> <icon-dir> [--selftest]
//
// Phase 1 watches the user's real windows read-only for <seconds>, printing every
// snapshot change, and writes a 32 px PNG of each item's icon to <icon-dir>.
// Phase 2 (--selftest) spawns its own test window (this exe with --testwindow) and
// exercises Activate / CloseAll against that window only.

#include "shell/task_model.h"

#include "log.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace atrium;

namespace {

TaskModel* g_model = nullptr;
bool g_changed = false;

void Out(const wchar_t* format, ...)
{
    wchar_t text[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    char utf8[6200];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n > 0)
        fputs(utf8, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

double NowMs()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_model && g_model->HandleMessage(m, w, l))
        return 0;
    return DefWindowProcW(h, m, w, l);
}

// Dispatches messages for `ms`, or until `done` returns true.
template <class Pred> bool Pump(DWORD ms, Pred done)
{
    ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done())
            return true;
        ULONGLONG now = GetTickCount64();
        if (now >= end)
            return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(std::min<ULONGLONG>(end - now, 20)), QS_ALLINPUT);
    }
}

void PrintSnapshot(double t0)
{
    auto items = g_model->Snapshot();
    Out(L"--- snapshot at +%.0f ms: %zu items", NowMs() - t0, items.size());
    for (const TaskItem& i : items) {
        std::wstring hw;
        for (HWND h : i.windows) {
            wchar_t b[24];
            swprintf_s(b, L" %llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
            hw += b;
        }
        Out(L"  [%s] pin=%d win=%zu active=%d attn=%d iconVer=%llu name=\"%s\" key=%s hwnds:%s", i.pinned ? L"pin" : L"run", i.pinIndex, i.windows.size(),
            i.active, i.attention, i.iconVersion, i.name.c_str(), i.key.c_str(), hw.c_str());
    }
}

// ---- PNG output (WIC) --------------------------------------------------------

bool WritePng(const std::wstring& path, const IconImage& img)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))))
        return false;
    winrt::com_ptr<IWICBitmap> bmp;
    if (FAILED(factory->CreateBitmapFromMemory(UINT(img.width), UINT(img.height), GUID_WICPixelFormat32bppPBGRA, UINT(img.width * 4),
                                               UINT(img.bgra.size()), const_cast<BYTE*>(img.bgra.data()), bmp.put())))
        return false;
    winrt::com_ptr<IWICFormatConverter> conv; // PNG stores straight alpha
    if (FAILED(factory->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(bmp.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> enc;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) || FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(UINT(img.width), UINT(img.height))))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)) || FAILED(frame->WriteSource(conv.get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return false;
    return true;
}

std::wstring Sanitize(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s)
        out += (iswalnum(c) || c == L'.' || c == L'-') ? c : L'_';
    return out.substr(0, 40);
}

void AlphaStats(const IconImage& img, wchar_t* text, size_t cap)
{
    size_t zero = 0, full = 0, part = 0, bad = 0, n = img.bgra.size() / 4;
    for (size_t i = 0; i < img.bgra.size(); i += 4) {
        uint8_t a = img.bgra[i + 3];
        (a == 0 ? zero : a == 255 ? full : part)++;
        bad += img.bgra[i] > a || img.bgra[i + 1] > a || img.bgra[i + 2] > a;
    }
    swprintf_s(text, cap, L"%dx%d alpha: %zu transparent, %zu partial, %zu opaque of %zu; non-premultiplied px: %zu", img.width, img.height, zero, part, full, n,
               bad);
}

// ---- test window process -----------------------------------------------------

int RunTestWindow()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AtriumProbeTestWindow";
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_DESTROY || (m == WM_TIMER && w == 1)) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    };
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"Atrium Probe Test Window", WS_OVERLAPPEDWINDOW, 120, 120, 420, 300, nullptr, nullptr, inst, nullptr);
    if (!w)
        return 1;
    ShowWindow(w, SW_SHOWNOACTIVATE);
    SetTimer(w, 1, 90000, nullptr); // never outlive a crashed parent for long
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

std::wstring LowerPath(std::wstring s)
{
    CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

const TaskItem* FindItem(const std::vector<TaskItem>& items, const std::wstring& key)
{
    for (const TaskItem& i : items)
        if (i.key == key)
            return &i;
    return nullptr;
}

void SelfTest(const std::wstring& exe)
{
    Out(L"=== selftest: Activate / CloseAll against our own test window");
    HWND prevFg = GetForegroundWindow();
    {
        wchar_t cls[128] = {}, title[128] = {};
        DWORD pid = 0;
        GetClassNameW(prevFg, cls, 128);
        GetWindowTextW(prevFg, title, 128);
        GetWindowThreadProcessId(prevFg, &pid);
        Out(L"selftest: foreground before: class=%s title=\"%s\" pid=%lu", cls, title, pid);
    }
    std::wstring key = LowerPath(exe);

    std::wstring cmd = L"\"" + exe + L"\" --testwindow";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Out(L"selftest: CreateProcess failed (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    HWND win = nullptr;
    bool appeared = Pump(8000, [&] {
        auto items = g_model->Snapshot();
        const TaskItem* t = FindItem(items, key);
        if (t && t->windows.size() == 1) {
            win = t->windows[0];
            return true;
        }
        return false;
    });
    Out(L"selftest: test window %s in the model", appeared ? L"appeared" : L"DID NOT appear");
    if (appeared) {
        auto state = [&](const wchar_t* what) {
            auto items = g_model->Snapshot();
            const TaskItem* t = FindItem(items, key);
            Out(L"selftest: %-22s iconic=%d foreground=%d item.active=%d windows=%zu", what, IsIconic(win), GetForegroundWindow() == win, t && t->active,
                t ? t->windows.size() : size_t(0));
        };
        state(L"before");
        double t0 = NowMs();
        g_model->Activate(key);
        Out(L"selftest: Activate took %.1f ms", NowMs() - t0);
        Pump(600, [] { return false; });
        state(L"after Activate #1");
        {
            // Diagnostic: is a plain SetForegroundWindow from this process refused in this session?
            BOOL ok = SetForegroundWindow(win);
            Pump(300, [] { return false; });
            Out(L"selftest: plain SetForegroundWindow -> %d, foreground now == test window: %d (lock timeout may apply)", ok, GetForegroundWindow() == win);
        }
        ShowWindowAsync(win, SW_MINIMIZE); // our own test window: set up the restore case
        Pump(500, [] { return false; });
        state(L"test window minimized");
        g_model->Activate(key); // minimized -> must be restored
        Pump(800, [] { return false; });
        state(L"after Activate #2 (restore)");
        g_model->Activate(key); // minimized -> restore + activate
        Pump(800, [] { return false; });
        state(L"after Activate #3");
        PinnedApp pin = g_model->PinFor(key);
        Out(L"selftest: PinFor -> name=\"%s\" path=%s aumid=\"%s\"", pin.name.c_str(), pin.path.c_str(), pin.aumid.c_str());
        g_model->CloseAll(key);
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    Out(L"selftest: test process %s after CloseAll", wait == WAIT_OBJECT_0 ? L"exited" : L"STILL RUNNING");
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1); // our own child, clean up
        WaitForSingleObject(pi.hProcess, 2000);
    }
    CloseHandle(pi.hProcess);
    bool gone = Pump(3000, [&] { return !FindItem(g_model->Snapshot(), key); });
    Out(L"selftest: item %s from the snapshot", gone ? L"removed" : L"STILL PRESENT");
    if (prevFg && IsWindow(prevFg))
        SetForegroundWindow(prevFg);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && wcscmp(argv[1], L"--testwindow") == 0)
        return RunTestWindow();
    if (argc < 3) {
        Out(L"usage: atrium_task_probe <seconds> <icon-dir> [--selftest]");
        return 2;
    }
    int seconds = _wtoi(argv[1]);
    std::wstring iconDir = argv[2];
    bool selftest = argc > 3 && wcscmp(argv[3], L"--selftest") == 0;
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SHCreateDirectoryExW(nullptr, iconDir.c_str(), nullptr);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = L"AtriumTaskProbeHost";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(0, wc.lpszClassName, L"Atrium task probe", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr); // never shown

    double t0 = NowMs();
    {
        TaskModel model(host, [] { g_changed = true; });
        g_model = &model;
        auto pins = TaskModel::ImportWindowsPins();
        Out(L"imported %zu Windows pins", pins.size());
        for (const PinnedApp& p : pins)
            Out(L"  pin \"%s\" path=%s aumid=\"%s\"", p.name.c_str(), p.path.c_str(), p.aumid.c_str());
        model.SetPins(pins);

        std::map<std::wstring, double> requested; // key -> request time
        std::map<std::wstring, bool> done;
        double loadMsTotal = 0, loadMsMax = 0;
        int loaded = 0, failed = 0, index = 0;
        ULONGLONG end = GetTickCount64() + ULONGLONG(seconds) * 1000;
        PrintSnapshot(t0);
        g_changed = false;
        while (GetTickCount64() < end) {
            Pump(50, [] { return false; });
            if (g_changed) {
                g_changed = false;
                PrintSnapshot(t0);
            }
            for (const TaskItem& item : model.Snapshot()) {
                if (done[item.key])
                    continue;
                if (!requested.contains(item.key))
                    requested[item.key] = NowMs();
                IconImage img = model.Icon(item.key, 32);
                if (img.empty())
                    continue;
                double ms = NowMs() - requested[item.key];
                done[item.key] = true;
                ++loaded;
                loadMsTotal += ms;
                loadMsMax = std::max(loadMsMax, ms);
                wchar_t stats[256];
                AlphaStats(img, stats, ARRAYSIZE(stats));
                wchar_t file[64];
                swprintf_s(file, L"%02d_", index++);
                std::wstring path = iconDir + L"\\" + file + Sanitize(item.name) + L".png";
                bool ok = WritePng(path, img);
                Out(L"icon %-32s %6.1f ms  %s  %s", item.name.c_str(), ms, stats, ok ? path.c_str() : L"(PNG write failed)");
            }
        }
        // Items whose icon never arrived.
        for (const TaskItem& item : model.Snapshot())
            if (!done[item.key]) {
                ++failed;
                Out(L"icon %-32s NOT LOADED (key %s)", item.name.c_str(), item.key.c_str());
            }
        auto items = model.Snapshot();
        double s0 = NowMs();
        for (int i = 0; i < 1000; ++i)
            items = model.Snapshot();
        Out(L"Snapshot(): %.4f ms per call (%zu items); icons loaded %d, failed %d, avg load %.1f ms, max %.1f ms", (NowMs() - s0) / 1000.0, items.size(),
            loaded, failed, loaded ? loadMsTotal / loaded : 0.0, loadMsMax);
        PrintSnapshot(t0);
        if (selftest)
            SelfTest(exe);
        g_model = nullptr;
    }
    DestroyWindow(host);
    CoUninitialize();
    return 0;
}
