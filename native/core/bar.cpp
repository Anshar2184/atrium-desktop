#include "bar.h"

#include "log.h"
#include "render/capture.h"
#include "render/glass_renderer.h"

#include <shellscalingapi.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace atrium {

namespace {

constexpr wchar_t kVisualClass[] = L"AtriumBarVisual";
constexpr wchar_t kInputClass[] = L"AtriumBarInput";
constexpr double kInsetDip = 14; // room for the drop shadow around the shape
constexpr DWORD kStaleMs = 2000;  // present at least this often (echo filter cap)

int Px(double dip, float scale) { return int(std::lround(dip * scale)); }

GlassParams ParamsFor(const Settings& s, float scale)
{
    GlassParams p{};
    p.radius = float(s.cornerRadius * scale);
    p.edge = float(s.edge * scale);
    p.refract = float(s.refraction * scale);
    p.dispersion = float(s.dispersion);
    p.blurMix = float(s.blur);
    p.blurLod = float(1.0 + 3.5 * s.blur);
    p.tintMix = float(1.0 - s.transparency);
    p.tintR = float((s.tint >> 16) & 0xFF) / 255.f;
    p.tintG = float((s.tint >> 8) & 0xFF) / 255.f;
    p.tintB = float(s.tint & 0xFF) / 255.f;
    p.tintA = 1;
    p.highlight = float(s.highlight);
    p.shadow = float(s.shadow);
    p.lightX = -0.6f;
    p.lightY = -0.8f;
    p.inset = float(kInsetDip * scale);
    return p;
}

TextOverlay ClockText(const Settings& s, float scale)
{
    TextOverlay t;
    if (!s.showClock)
        return t;
    wchar_t time[64] = {}, date[64] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, time, 64);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, date, 64, nullptr);
    t.primary = time;
    t.secondary = date;
    float h = float(s.height * scale);
    t.primaryPx = h * 0.27f;
    t.secondaryPx = h * 0.21f;
    return t;
}

void RegisterClasses(HINSTANCE instance)
{
    static bool done = false;
    if (done)
        return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = DefWindowProcW; // replaced per window once `this` is attached
    wc.lpszClassName = kVisualClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kInputClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

Bar::Bar(HINSTANCE instance, bool debugCapturable) : debugCapturable_(debugCapturable)
{
    RegisterClasses(instance);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    frame_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Visual: never hit-tested (layered + transparent), no redirection bitmap.
    visual_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                              kVisualClass, L"Atrium Bar", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(visual_, 0, 255, LWA_ALPHA);
    // Input: draws nothing, so it is invisible; its window region is the shape.
    input_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kInputClass,
                             L"Atrium Bar Input", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(input_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::InputProc));
    SetWindowLongPtrW(visual_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(visual_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::VisualProc));
    if (debugCapturable_) {
        Log(L"bar: --debug-capturable, backdrop frozen after the first frame");
    } else {
        if (!SetWindowDisplayAffinity(visual_, WDA_EXCLUDEFROMCAPTURE))
            Log(L"bar: WDA_EXCLUDEFROMCAPTURE failed (%lu); the glass would capture itself", GetLastError());
        SetWindowDisplayAffinity(input_, WDA_EXCLUDEFROMCAPTURE);
    }

    render_ = std::thread([this] { RenderThread(); });
}

Bar::~Bar()
{
    SetEvent(quit_);
    if (render_.joinable())
        render_.join();
    DestroyWindow(input_);
    DestroyWindow(visual_);
    CloseHandle(wake_);
    CloseHandle(quit_);
    CloseHandle(frame_);
}

LRESULT CALLBACK Bar::VisualProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_DPICHANGED && self) {
        self->Relayout();
        return 0;
    }
    if (m == WM_CLOSE && self) {
        if (self->onCloseRequest)
            self->onCloseRequest(); // the app decides; never destroy just this window
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Bar::InputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // never take focus from the user's app
    case WM_CLOSE:
        if (self && self->onCloseRequest)
            self->onCloseRequest();
        return 0;
    case WM_RBUTTONUP:
        if (self && self->onContextMenu) {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            self->onContextMenu(pt);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Bar::Apply(const Settings& s)
{
    settings_ = s;
    Relayout();
}

void Bar::SetVisible(bool visible)
{
    visible_ = visible;
    Relayout();
}

void Bar::Relayout()
{
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    float scale = float(dpiX) / 96.f;

    const RECT& wa = mi.rcWork;
    int inset = Px(kInsetDip, scale);
    int barW = std::min(Px(settings_.width, scale), int(wa.right - wa.left) - 2 * inset);
    int barH = Px(settings_.height, scale);
    int winW = barW + 2 * inset, winH = barH + 2 * inset;
    int x = wa.left + (wa.right - wa.left - winW) / 2;
    int y = wa.bottom - Px(settings_.margin, scale) - barH - inset;

    SetWindowPos(visual_, HWND_TOPMOST, x, y, winW, winH, SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    // Input window right below the visual one, region = the rounded shape.
    SetWindowPos(input_, visual_, x, y, winW, winH, SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    int r = std::min(Px(settings_.cornerRadius, scale), std::min(barW, barH) / 2);
    SetWindowRgn(input_, CreateRoundRectRgn(inset, inset, inset + barW + 1, inset + barH + 1, 2 * r, 2 * r), TRUE);

    {
        std::lock_guard g(lock_);
        snapshot_.settings = settings_;
        snapshot_.window = RECT{x, y, x + winW, y + winH};
        snapshot_.monitor = mi.rcMonitor;
        snapshot_.hmonitor = mon;
        snapshot_.scale = scale;
        snapshot_.visible = visible_;
        ++snapshot_.generation;
    }
    SetEvent(wake_);
}

void Bar::RenderThread()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        bool rebuild = false;
        try {
            rebuild = RenderSession();
        } catch (const winrt::hresult_error& e) {
            Log(L"render: 0x%08X %s; rebuilding in 1 s", static_cast<unsigned>(e.code()), e.message().c_str());
            rebuild = true;
        }
        if (!rebuild || WaitForSingleObject(quit_, 1000) != WAIT_TIMEOUT)
            break;
    }
    winrt::uninit_apartment();
}

bool Bar::RenderSession()
{
    // Wait for the first layout.
    HANDLE first[] = {quit_, wake_};
    Snapshot snap;
    for (;;) {
        {
            std::lock_guard g(lock_);
            snap = snapshot_;
        }
        if (snap.hmonitor)
            break;
        if (WaitForMultipleObjects(2, first, FALSE, INFINITE) == WAIT_OBJECT_0)
            return false;
    }

    Gpu gpu = Gpu::ForMonitor(snap.hmonitor);
    MonitorCapture capture(gpu, snap.hmonitor, frame_);
    GlassRenderer renderer(gpu, visual_, snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);

    unsigned drawnGeneration = 0;
    std::wstring drawnText;
    ULONGLONG lastPresent = 0;
    // Per-minute counters: captured frames, presents, frames skipped as echoes.
    unsigned frames = 0, presents = 0, skipped = 0;
    std::vector<double> workMs; // capture copy + comparison (+ draw and present), per wake-up
    winrt::com_ptr<ID3D11Texture2D> frozen; // --debug-capturable only
    ULONGLONG statsStart = GetTickCount64();
    Log(L"render: started on %s, window %ldx%ld", gpu.adapterName.c_str(), snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
    HANDLE handles[] = {quit_, wake_, frame_};
    for (;;) {
        DWORD w = WaitForMultipleObjects(3, handles, FALSE, 1000);
        if (w == WAIT_OBJECT_0)
            return false;
        if (GetTickCount64() - statsStart >= 60000) {
            std::sort(workMs.begin(), workMs.end());
            auto pct = [&](double q) { return workMs.empty() ? 0.0 : workMs[std::min(workMs.size() - 1, size_t(q * double(workMs.size())))]; };
            Log(L"render: last minute %u frames, %u presents, %u echoes skipped; work p50 %.2f ms, p95 %.2f ms, max %.2f ms", frames,
                presents, skipped, pct(0.5), pct(0.95), workMs.empty() ? 0.0 : workMs.back());
            frames = presents = skipped = 0;
            workMs.clear();
            statsStart = GetTickCount64();
        }
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        auto elapsedMs = [&] {
            LARGE_INTEGER t1, f;
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        };
        {
            std::lock_guard g(lock_);
            snap = snapshot_;
        }
        auto frame = capture.TryLatest();
        if (!snap.visible) {
            if (frame)
                frame.Close();
            continue;
        }
        if (snap.hmonitor != MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY))
            return true; // primary monitor changed: new device and capture
        renderer.Resize(snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);

        // Backdrop source: the newest capture frame, or in --debug-capturable
        // mode a private copy of the first one (later frames contain the glass).
        winrt::com_ptr<ID3D11Texture2D> source;
        if (frame) {
            source = MonitorCapture::TextureOf(frame);
            ++frames;
            if (debugCapturable_) {
                if (!frozen) {
                    D3D11_TEXTURE2D_DESC d{};
                    source->GetDesc(&d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    d.MiscFlags = 0;
                    winrt::check_hresult(gpu.device->CreateTexture2D(&d, nullptr, frozen.put()));
                    gpu.context->CopyResource(frozen.get(), source.get());
                }
                source = frozen;
            }
        } else if (frozen && snap.generation != drawnGeneration) {
            source = frozen; // size-dependent textures were recreated
        }
        bool changed = false;
        bool hadBackdrop = source != nullptr;
        if (source) {
            renderer.UpdateBackdrop(source.get(), snap.window.left - snap.monitor.left, snap.window.top - snap.monitor.top);
            changed = renderer.BackdropChanged();
        }
        source = nullptr;
        if (frame)
            frame.Close();
        TextOverlay text = ClockText(snap.settings, snap.scale);
        std::wstring textKey = text.primary + L"|" + text.secondary;
        ULONGLONG now = GetTickCount64();
        bool force = snap.generation != drawnGeneration || textKey != drawnText || now - lastPresent > kStaleMs;
        if (!changed && !force) {
            ++skipped; // echo of our own present, or nothing new under the glass
            if (hadBackdrop)
                workMs.push_back(elapsedMs());
            continue;
        }
        if (!renderer.Present(ParamsFor(snap.settings, snap.scale), text)) {
            Log(L"render: device lost");
            return true;
        }
        ++presents;
        workMs.push_back(elapsedMs());
        drawnGeneration = snap.generation;
        drawnText = textKey;
        lastPresent = now;
    }
}

} // namespace atrium
