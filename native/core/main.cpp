// Atrium.Core: the resident process. Owns the glass bar, the tray icon and the
// settings pipe. Coexistence mode only: Explorer's taskbar is never hidden here
// (takeover stays behind the recovery gates, docs/DECISIONS.md D09/D10).

#include "bar.h"
#include "log.h"
#include "resource.h"
#include "settings.h"
#include "settings_server.h"
#include "shell/task_model.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

using namespace atrium;

constexpr wchar_t kHostClass[] = L"AtriumCoreHost";
constexpr wchar_t kInstanceMutex[] = L"Local\\Atrium.Core";
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT WM_OPEN_SETTINGS = WM_APP + 3; // sent by a second Atrium.Core.exe
constexpr UINT kTrayId = 1;

enum Command : UINT {
    kCmdSettings = 100, kCmdToggle, kCmdExit, kCmdNewWindow, kCmdPin, kCmdUnpin, kCmdClose, kCmdOpenLocation, kCmdDesktop, kCmdTaskManager
};

struct App {
    HINSTANCE instance = nullptr;
    HWND host = nullptr;
    UINT taskbarCreated = 0;
    std::unique_ptr<Bar> bar;
    std::unique_ptr<SettingsServer> server;
    std::unique_ptr<TaskModel> tasks;
};

App g_app;

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\'));
}

void OpenSettings()
{
    std::wstring dir = ExeDir();
    for (const wchar_t* rel : {L"\\Settings\\Atrium.Settings.exe", L"\\Atrium.Settings.exe"}) {
        std::wstring exe = dir + rel;
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        // The settings app keeps a single instance itself and activates it.
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32)
            return;
    }
    Log(L"settings: Atrium.Settings.exe not found next to %s", dir.c_str());
    MessageBoxW(nullptr, L"没有找到 Atrium.Settings.exe。请确认它与 Atrium.Core.exe 位于同一安装目录。", L"Atrium", MB_ICONWARNING);
}

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = static_cast<HICON>(LoadImageW(g_app.instance, MAKEINTRESOURCEW(IDI_ATRIUM), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(nid.szTip, L"Atrium");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Win+<key>, a public system shortcut (D11).
void SendWinShortcut(WORD key)
{
    INPUT in[4] = {};
    for (auto& i : in)
        i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].ki.wVk = key;
    in[2].ki.wVk = key;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

void ShowMenu(POINT pt)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"外观设置…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdDesktop, L"显示桌面");
    AppendMenuW(menu, MF_STRING, kCmdTaskManager, L"任务管理器");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToggle, g_app.bar->Visible() ? L"隐藏玻璃栏" : L"显示玻璃栏");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出 Atrium");
    SetMenuDefaultItem(menu, kCmdSettings, FALSE);
    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(g_app.host);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_app.host, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_app.host, WM_NULL, 0, 0);
    switch (cmd) {
    case kCmdSettings:
        OpenSettings();
        break;
    case kCmdToggle:
        g_app.bar->SetVisible(!g_app.bar->Visible());
        break;
    case kCmdDesktop:
        SendWinShortcut('D');
        break;
    case kCmdTaskManager:
        ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kCmdExit:
        DestroyWindow(g_app.host);
        break;
    }
}

// Applies settings to the bar and the task model.
void ApplySettings(const Settings& s)
{
    g_app.tasks->SetPins(s.pins.value_or(std::vector<PinnedApp>{}));
    g_app.tasks->SetShowRunning(s.showRunning);
    g_app.bar->Apply(s);
}

void ShowItemMenu(const std::wstring& key, POINT pt)
{
    std::vector<TaskItem> items = g_app.tasks->Snapshot();
    auto it = std::find_if(items.begin(), items.end(), [&](const TaskItem& t) { return t.key == key; });
    if (it == items.end())
        return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, it->name.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdNewWindow, it->windows.empty() ? L"打开" : L"打开新窗口");
    std::wstring location = g_app.tasks->FileLocation(key);
    if (!location.empty())
        AppendMenuW(menu, MF_STRING, kCmdOpenLocation, L"打开文件所在的位置");
    AppendMenuW(menu, MF_STRING, it->pinned ? kCmdUnpin : kCmdPin, it->pinned ? L"从 Atrium 取消固定" : L"固定到 Atrium");
    if (!it->windows.empty())
        AppendMenuW(menu, MF_STRING, kCmdClose, it->windows.size() > 1 ? L"关闭所有窗口" : L"关闭窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"外观设置…");
    SetForegroundWindow(g_app.host);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_CENTERALIGN, pt.x, pt.y, g_app.host, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_app.host, WM_NULL, 0, 0);
    switch (cmd) {
    case kCmdNewWindow:
        g_app.tasks->Launch(key);
        break;
    case kCmdPin: {
        PinnedApp pin = g_app.tasks->PinFor(key);
        g_app.server->Modify([&](Settings& s) {
            auto pins = s.pins.value_or(std::vector<PinnedApp>{});
            pins.push_back(pin);
            s.pins = std::move(pins);
        });
        break;
    }
    case kCmdUnpin:
        g_app.server->Modify([&](Settings& s) {
            auto pins = s.pins.value_or(std::vector<PinnedApp>{});
            if (it->pinIndex >= 0 && it->pinIndex < int(pins.size()))
                pins.erase(pins.begin() + it->pinIndex);
            s.pins = std::move(pins);
        });
        break;
    case kCmdClose:
        g_app.tasks->CloseAll(key);
        break;
    case kCmdOpenLocation:
        if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(location.c_str())) {
            SHOpenFolderAndSelectItems(id, 0, nullptr, 0);
            ILFree(id);
        }
        break;
    case kCmdSettings:
        OpenSettings();
        break;
    }
}

// An app dropped at visible position `to` (in the list without it). Pinned
// apps reorder the pins; a running app dropped among the pins gets pinned
// there; a pinned app dropped among the running ones goes to the end of the
// pins (dragging never unpins, as on the Windows taskbar).
void Reorder(const std::wstring& key, int to)
{
    std::vector<TaskItem> items = g_app.tasks->Snapshot();
    auto it = std::find_if(items.begin(), items.end(), [&](const TaskItem& t) { return t.key == key; });
    if (it == items.end())
        return;
    TaskItem dragged = *it;
    items.erase(it);
    to = std::clamp(to, 0, int(items.size()));
    int pinnedLeft = int(std::count_if(items.begin(), items.end(), [](const TaskItem& t) { return t.pinned; }));
    if (!dragged.pinned && to >= pinnedLeft) {
        g_app.tasks->MoveRunning(key, to - pinnedLeft);
        return;
    }
    // Pin index (in the current list) the item goes in front of.
    int at = to < pinnedLeft ? items[size_t(to)].pinIndex : pinnedLeft ? items[size_t(pinnedLeft - 1)].pinIndex + 1 : 0;
    PinnedApp fresh = dragged.pinned ? PinnedApp{} : g_app.tasks->PinFor(key);
    g_app.server->Modify([&](Settings& s) {
        auto pins = s.pins.value_or(std::vector<PinnedApp>{});
        PinnedApp pin = fresh;
        int dest = at;
        if (dragged.pinned) {
            if (dragged.pinIndex < 0 || dragged.pinIndex >= int(pins.size()))
                return;
            pin = pins[size_t(dragged.pinIndex)];
            pins.erase(pins.begin() + dragged.pinIndex);
            if (dest > dragged.pinIndex)
                --dest;
        }
        pins.insert(pins.begin() + std::clamp(dest, 0, int(pins.size())), pin);
        s.pins = std::move(pins);
    });
    Log(L"bar: %s moved to %d%s", key.c_str(), to, dragged.pinned ? L"" : L" and pinned");
}

// The clock opens the Windows calendar and notifications (Win+N), like the
// system clock does (D11: public shortcuts only).
void OpenCalendar()
{
    SendWinShortcut('N');
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_app.tasks && g_app.tasks->HandleMessage(m, w, l))
        return 0;
    if (m == g_app.taskbarCreated && m != 0) {
        AddTrayIcon(); // Explorer restarted
        return 0;
    }
    switch (m) {
    case WM_TRAY:
        switch (LOWORD(l)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            ShowMenu(POINT{GET_X_LPARAM(w), GET_Y_LPARAM(w)});
            return 0;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            OpenSettings();
            return 0;
        }
        return 0;
    case WM_OPEN_SETTINGS:
        OpenSettings();
        return 0;
    case WM_ATRIUM_SETTINGS:
        ApplySettings(g_app.server->Current());
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        g_app.bar->Relayout();
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETWORKAREA)
            g_app.bar->Relayout(); // e.g. the system taskbar changed size or auto-hide
        return 0;
    case WM_ENDSESSION:
        if (w)
            Log(L"core: session ending");
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int)
{
    // `Atrium.Core.exe --exit` asks a running instance to quit (installer, updater).
    if (wcsstr(cmdLine, L"--exit")) {
        HWND other = FindWindowW(kHostClass, nullptr);
        if (!other)
            return 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(other, WM_CLOSE, 0, 0);
        DWORD wait = process ? WaitForSingleObject(process, 10000) : WAIT_FAILED;
        if (process)
            CloseHandle(process);
        return wait == WAIT_OBJECT_0 ? 0 : 1;
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: launching it again opens the settings instead.
        if (HWND other = FindWindowW(kHostClass, nullptr))
            PostMessageW(other, WM_OPEN_SETTINGS, 0, 0);
        return 0;
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    g_app.instance = instance;
    Log(L"core: starting");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = kHostClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ATRIUM));
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so TaskbarCreated arrives.
    g_app.host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Atrium", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g_app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_app.host, g_app.taskbarCreated, MSGFLT_ALLOW, nullptr);

    Settings settings = LoadSettings();
    if (!settings.pins) {
        // First run: start from the user's Windows taskbar pins (saved on the next save).
        settings.pins = TaskModel::ImportWindowsPins();
        Log(L"core: imported %zu Windows taskbar pins", settings.pins->size());
    }
    g_app.bar = std::make_unique<Bar>(instance, wcsstr(cmdLine, L"--debug-capturable") != nullptr, wcsstr(cmdLine, L"--debug-layout") != nullptr);
    g_app.tasks = std::make_unique<TaskModel>(g_app.host, [] { g_app.bar->SetItems(g_app.tasks->Snapshot()); });
    g_app.bar->iconProvider = [](const std::wstring& key, int px) { return g_app.tasks->Icon(key, px); };
    g_app.bar->onActivate = [](const std::wstring& key) { g_app.tasks->Activate(key); };
    g_app.bar->onLaunch = [](const std::wstring& key) { g_app.tasks->Launch(key); };
    g_app.bar->onItemMenu = ShowItemMenu;
    g_app.bar->onClockClick = OpenCalendar;
    g_app.bar->onReorder = Reorder;
    g_app.bar->onFileDragHover = [](const std::wstring& key) { g_app.tasks->Show(key); };
    g_app.bar->canOpenFiles = [](const std::wstring& key) { return g_app.tasks->CanOpenFiles(key); };
    g_app.bar->onDropFiles = [](const std::wstring& key, const std::vector<std::wstring>& files) { g_app.tasks->OpenFiles(key, files); };
    g_app.bar->onContextMenu = ShowMenu;
    g_app.bar->onCloseRequest = [] { PostMessageW(g_app.host, WM_CLOSE, 0, 0); };
    ApplySettings(settings);
    g_app.bar->SetItems(g_app.tasks->Snapshot());
    g_app.server = std::make_unique<SettingsServer>(g_app.host, settings);
    AddTrayIcon();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_app.server.reset();
    g_app.tasks.reset();
    g_app.bar.reset();
    Log(L"core: exited");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
