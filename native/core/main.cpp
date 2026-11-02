// Atrium.Core: the resident process. Owns the glass bar, the tray icon and the
// settings pipe. Coexistence mode only: Explorer's taskbar is never hidden here
// (takeover stays behind the recovery gates, docs/DECISIONS.md D09/D10).

#include "bar.h"
#include "log.h"
#include "resource.h"
#include "settings.h"
#include "settings_server.h"
#include "shell/app_catalog.h"
#include "shell/task_model.h"
#include "shell/notification_listener.h"
#include "shell/system_status.h"
#include "shell/tray_model.h"
#include "shell/feishu_source.h"
#include "shell/wechat_source.h"
#include "ui/calendar_panel.h"
#include "ui/control_center.h"
#include "ui/desktop_widgets.h"
#include "ui/lock_screen.h"
#include "ui/notifications.h"
#include "ui/start_menu.h"
#include "ui/tray_panel.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <UIAutomation.h>
#include <windowsx.h>

#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace atrium;

constexpr wchar_t kHostClass[] = L"AtriumCoreHost";
constexpr wchar_t kInstanceMutex[] = L"Local\\Atrium.Core";
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT WM_OPEN_SETTINGS = WM_APP + 3; // sent by a second Atrium.Core.exe
constexpr UINT WM_CATALOG = WM_APP + 4;       // the app catalog has news
constexpr UINT WM_NOTICE = WM_APP + 5;        // lParam: Notice* from the listener thread
constexpr UINT WM_TRAY_ICONS = WM_APP + 6;    // the tray model has read the icons
constexpr UINT WM_VOLUME = WM_APP + 7;        // the default playback device's volume changed
constexpr UINT_PTR kTrayTimer = 0x51;
constexpr UINT_PTR kStatusTimer = 0x52;       // the bar's status icon
constexpr UINT_PTR kTrayFocusTimer = 0x53;    // after a forwarded tray click
constexpr UINT kStatusMs = 3000;
constexpr UINT kTrayId = 1;

enum Command : UINT {
    kCmdSettings = 100, kCmdToggle, kCmdExit, kCmdNewWindow, kCmdPin, kCmdUnpin, kCmdClose, kCmdOpenLocation, kCmdDesktop, kCmdTaskManager,
    kCmdWidget = 200 // + 0..4: add a desktop widget
};

struct App {
    HINSTANCE instance = nullptr;
    HWND host = nullptr;
    UINT taskbarCreated = 0;
    std::unique_ptr<Bar> bar;
    std::unique_ptr<SettingsServer> server;
    std::unique_ptr<TaskModel> tasks;
    std::unique_ptr<AppCatalog> catalog;
    std::unique_ptr<StartMenu> start;
    std::unique_ptr<Notifications> notes;
    std::unique_ptr<NotificationListener> listener;
    std::unique_ptr<WeChatSource> wechat;
    std::unique_ptr<FeishuSource> feishu;
    std::unique_ptr<TrayModel> tray;
    std::unique_ptr<TrayPanel> trayPanel;
    std::unique_ptr<ControlCenter> control;
    std::unique_ptr<CalendarPanel> calendar;
    std::unique_ptr<LockScreen> lock;
    std::unique_ptr<DesktopWidgets> widgets;
    bool widgetsOn = false;
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

// Win+<key> (or Win+Ctrl+<key>), a public system shortcut (D11).
void SendWinShortcut(WORD key, bool ctrl = false)
{
    std::vector<INPUT> in;
    auto add = [&](WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        in.push_back(i);
    };
    add(VK_LWIN, false);
    if (ctrl)
        add(VK_CONTROL, false);
    add(key, false);
    add(key, true);
    if (ctrl)
        add(VK_CONTROL, true);
    add(VK_LWIN, true);
    SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
}

// The system tray's hidden icons. Win+B (public shortcut) puts the keyboard
// focus on the system taskbar's notification area; if that focus is the
// "show hidden icons" button, it is invoked through UI Automation. Otherwise
// (no hidden icons, other shell) the tray simply keeps the focus.
void OpenTray()
{
    SendWinShortcut('B');
    SetTimer(g_app.host, kTrayTimer, 350, nullptr);
}

void OpenTrayStep2()
{
    KillTimer(g_app.host, kTrayTimer);
    winrt::com_ptr<IUIAutomation> uia;
    winrt::com_ptr<IUIAutomationElement> focus;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()))) ||
        FAILED(uia->GetFocusedElement(focus.put())) || !focus) {
        Log(L"tray: no focused element after Win+B");
        return;
    }
    BSTR id = nullptr;
    focus->get_CurrentAutomationId(&id);
    std::wstring automationId = id ? id : L"";
    SysFreeString(id);
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    if (automationId == L"SystemTrayIcon" &&
        SUCCEEDED(focus->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke && SUCCEEDED(invoke->Invoke())) {
        Log(L"tray: opened the hidden icons");
        return;
    }
    Log(L"tray: focus is on '%s', left there (no hidden-icons button)", automationId.c_str());
}

void OpenSystem(const std::wstring& what)
{
    if (what == L"network" || what == L"volume" || what == L"battery") {
        g_app.control->Open(g_app.bar->EntryRect(L"#status"), g_app.bar->BarRect());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry %s (control centre)", what.c_str());
        return;
    }
    if (what == L"lock") {
        g_app.lock->Open(g_app.notes->Recent(3));
        return;
    }
    if (what == L"tray") {
        g_app.trayPanel->Open(g_app.bar->EntryRect(L"#tray"), g_app.bar->BarRect(), g_app.bar->PreviousForeground());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry tray (tray panel)");
        return;
    }
    if (what == L"network")
        ShellExecuteW(nullptr, L"open", L"ms-availablenetworks:", nullptr, nullptr, SW_SHOWNORMAL);
    else if (what == L"volume")
        SendWinShortcut('V', true); // Win+Ctrl+V: sound output in quick settings
    else if (what == L"battery")
        ShellExecuteW(nullptr, L"open", L"ms-settings:batterysaver", nullptr, nullptr, SW_SHOWNORMAL);
    else if (what == L"tray")
        OpenTray();
    else if (what == L"account")
        ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"start: system entry %s", what.c_str());
}

void ToggleTray(RECT);
void ToggleControl(RECT);
void ToggleCalendar();
void UpdateStatus();
void FakeStatus(const std::wstring& arg);

// Test and automation commands from a second process: Atrium.Core.exe --command <verb> [argument]
void RunCommand(const std::wstring& line)
{
    size_t sp = line.find(L' ');
    std::wstring verb = line.substr(0, sp), arg = sp == std::wstring::npos ? L"" : line.substr(sp + 1);
    while (!arg.empty() && iswspace(arg.back()))
        arg.pop_back();
    while (!verb.empty() && iswspace(verb.back()))
        verb.pop_back();
    Log(L"command: %s", line.c_str());
    if (verb == L"start") {
        g_app.start->Visible() ? g_app.start->Close(false)
                               : g_app.start->Open(g_app.bar->BarRect(), g_app.bar->EntryRect(L"#start"), GetForegroundWindow());
        g_app.notes->PanelsChanged();
    }
    else if (verb == L"start-type")
        g_app.start->Type(arg);
    else if (verb == L"start-key" && !arg.empty())
        SendMessageW(g_app.start->Hwnd(), WM_CHAR, arg == L"enter" ? 13 : arg == L"esc" ? 27 : arg == L"back" ? 8 : arg[0], 0);
    else if (verb == L"start-state")
        g_app.start->LogState();
    else if (verb == L"start-click")
        g_app.start->TestClick(arg);
    else if (verb == L"bar-state")
        g_app.bar->LogState();
    else if (verb == L"notify") {
        // notify <app>|<title>|<body>|<action>
        std::vector<std::wstring> f;
        size_t from = 0;
        for (;;) {
            size_t bar = arg.find(L'|', from);
            f.push_back(arg.substr(from, bar == std::wstring::npos ? std::wstring::npos : bar - from));
            if (bar == std::wstring::npos)
                break;
            from = bar + 1;
        }
        f.resize(4);
        Notice n;
        n.source = L"test";
        n.app = f[0];
        n.appId = f[0];
        n.title = f[1];
        n.body = f[2];
        n.action = f[3];
        g_app.notes->Post(std::move(n));
    } else if (verb == L"tray-open")
        ToggleTray(RECT{});
    else if (verb == L"tray-state")
        g_app.trayPanel->LogState();
    else if (verb == L"tray-click" || verb == L"tray-rclick")
        g_app.trayPanel->TestClick(arg, verb == L"tray-rclick");
    else if (verb == L"control-open")
        ToggleControl(RECT{});
    else if (verb == L"control-state")
        g_app.control->LogState();
    else if (verb == L"control-click")
        g_app.control->TestClick(arg);
    else if (verb == L"lock-open")
        OpenSystem(L"lock");
    else if (verb == L"lock-state")
        g_app.lock->LogState();
    else if (verb == L"lock-dismiss")
        g_app.lock->Dismiss(arg == L"lock");
    else if (verb == L"calendar-open")
        ToggleCalendar();
    else if (verb == L"widgets-state")
        g_app.widgets->LogState();
    else if (verb == L"widgets-do")
        g_app.widgets->TestAction(arg);
    else if (verb == L"calendar-state")
        g_app.calendar->LogState();
    else if (verb == L"calendar-click")
        g_app.calendar->TestClick(arg);
    else if (verb == L"tray-find")
        g_app.trayPanel->LogFind(arg);
    else if (verb == L"status-fake") { // screenshots: "battery=15,charging=0,volume=40,muted=0,signal=80" or "off"
        FakeStatus(arg);
        UpdateStatus();
    }
    else if (verb == L"control-volume") { // tests: put the volume back
        SetVolume(_wtoi(arg.c_str()));
        UpdateStatus();
    }
    else if (verb == L"notify-state")
        g_app.notes->LogState();
    else if (verb == L"notify-open")
        g_app.notes->ToggleCenter();
    else if (verb == L"notify-click")
        g_app.notes->TestClick(arg);
    else if (verb == L"notify-clear")
        g_app.notes->Clear();
    else if (verb == L"notify-quiet")
        g_app.notes->SetQuietOverride(_wtoi(arg.c_str()));
    else if (verb == L"notify-probe") {
        // Blocking WinRT calls are not allowed on this STA thread.
        std::wstring result;
        std::thread([&] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            result = NotificationListener::Probe();
            winrt::uninit_apartment();
        }).join();
        Log(L"listener: probe %s", result.c_str());
    }
}

// Test override of the status shown on the bar (`status-fake`): key=value
// pairs replace what the system reports; "off" ends it.
std::vector<std::pair<std::wstring, int>> g_fakeStatus;

void FakeStatus(const std::wstring& arg)
{
    g_fakeStatus.clear();
    if (arg == L"off")
        return;
    size_t at = 0;
    while (at < arg.size()) {
        size_t end = arg.find(L',', at);
        std::wstring pair = arg.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        size_t eq = pair.find(L'=');
        if (eq != std::wstring::npos)
            g_fakeStatus.emplace_back(pair.substr(0, eq), _wtoi(pair.c_str() + eq + 1));
        at = end == std::wstring::npos ? arg.size() : end + 1;
    }
}

void UpdateStatus()
{
    SystemStatus st = QuerySystemStatus();
    for (const auto& [key, value] : g_fakeStatus) {
        if (key == L"battery") {
            st.battery = value >= 0;
            st.batteryPercent = value;
        } else if (key == L"charging") {
            st.charging = value != 0;
        } else if (key == L"volume") {
            st.audio = value >= 0;
            st.volume = std::max(value, 0);
        } else if (key == L"muted") {
            st.muted = value != 0;
        } else if (key == L"signal") {
            st.connected = st.wifi = value >= 0;
            st.signal = std::max(value, 0);
        }
    }
    // Volume notifications follow the default playback device.
    static std::wstring watched;
    static bool watching = false;
    if (!watching || st.audioDevice != watched) {
        WatchVolume(g_app.host, WM_VOLUME);
        watched = st.audioDevice;
        watching = true;
    }
    g_app.bar->SetStatus(st);
}

// Atrium's tray panel and control centre open above their entries on the
// bar; a click on the entry while one is open closes it (and must not reopen
// it: the press that deactivated it is the same click).
void ToggleTray(RECT)
{
    if (g_app.trayPanel->Visible()) {
        g_app.trayPanel->Close();
        return;
    }
    if (GetTickCount64() - g_app.trayPanel->LastClosed() < 400)
        return;
    g_app.trayPanel->Open(g_app.bar->EntryRect(L"#tray"), g_app.bar->BarRect(), g_app.bar->PreviousForeground());
    g_app.notes->PanelsChanged();
}

void ToggleControl(RECT)
{
    if (g_app.control->Visible()) {
        g_app.control->Close();
        return;
    }
    if (GetTickCount64() - g_app.control->LastClosed() < 400)
        return;
    g_app.control->Open(g_app.bar->EntryRect(L"#status"), g_app.bar->BarRect());
    g_app.notes->PanelsChanged();
}

void ToggleCalendar()
{
    if (g_app.calendar->Visible()) {
        g_app.calendar->Close();
        return;
    }
    if (GetTickCount64() - g_app.calendar->LastClosed() < 400)
        return;
    g_app.calendar->Open(g_app.bar->EntryRect(L"#clock"), g_app.bar->BarRect());
    g_app.notes->PanelsChanged();
}

void ToggleStart(RECT)
{
    if (g_app.start->Visible()) {
        g_app.start->Close(false);
        return;
    }
    // A click on the logo while the menu is open first deactivates (closes) it;
    // that click must not open it again.
    if (GetTickCount64() - g_app.start->LastClosed() < 400)
        return;
    g_app.start->Open(g_app.bar->BarRect(), g_app.bar->EntryRect(L"#start"), g_app.bar->PreviousForeground());
    g_app.notes->PanelsChanged();
}

void ShowMenu(POINT pt)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"外观设置…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdDesktop, L"显示桌面");
    AppendMenuW(menu, MF_STRING, kCmdTaskManager, L"任务管理器");
    HMENU widgets = nullptr;
    if (g_app.widgetsOn) {
        widgets = CreatePopupMenu();
        const wchar_t* names[] = {L"时钟", L"世界时钟", L"日历", L"日历（月视图）", L"电池"};
        for (int i = 0; i < 5; ++i)
            AppendMenuW(widgets, MF_STRING, kCmdWidget + i, names[i]);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widgets), L"添加桌面小组件");
    }
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
    default:
        if (cmd >= kCmdWidget && cmd < kCmdWidget + 5) {
            const std::pair<WidgetKind, WidgetSize> kinds[] = {{WidgetKind::Clock, WidgetSize::Small},
                                                               {WidgetKind::Clock, WidgetSize::Medium},
                                                               {WidgetKind::Calendar, WidgetSize::Small},
                                                               {WidgetKind::Calendar, WidgetSize::Medium},
                                                               {WidgetKind::Battery, WidgetSize::Small}};
            g_app.widgets->Add(kinds[cmd - kCmdWidget].first, kinds[cmd - kCmdWidget].second);
        }
        break;
    }
}

// The tray icon of a notice's sender: its program's file name or the start of
// its tooltip is the sender's name or id ("QQ" -> qq.exe, "QQ: 12345").
const TrayIcon* TrayIconOf(const Notice& n)
{
    static std::vector<TrayIcon> icons;
    icons = g_app.tray->Icons();
    auto lower = [](std::wstring s) {
        for (auto& c : s)
            c = wchar_t(towlower(c));
        return s;
    };
    std::vector<std::wstring> names;
    for (const std::wstring& s : {n.app, n.appId}) {
        std::wstring v = lower(s);
        if (size_t bang = v.find(L'!'); bang != std::wstring::npos) // packaged: "publisher.app_hash!App"
            v = v.substr(0, bang);
        if (size_t slash = v.find_last_of(L"\\/"); slash != std::wstring::npos) // a path
            v = v.substr(slash + 1);
        if (v.size() > 4 && v.ends_with(L".exe"))
            v.resize(v.size() - 4);
        if (v.size() >= 2)
            names.push_back(v);
    }
    for (const TrayIcon& icon : icons) {
        std::wstring stem = lower(icon.exe.substr(icon.exe.find_last_of(L'\\') + 1));
        if (stem.size() > 4 && stem.ends_with(L".exe"))
            stem.resize(stem.size() - 4);
        std::wstring tip = lower(TrayModel::Title(icon.tip));
        for (const std::wstring& name : names) {
            bool tipNames = tip.rfind(name, 0) == 0 && (tip.size() == name.size() || !iswalnum(tip[name.size()]));
            if ((!icon.exe.empty() && stem == name) || tipNames)
                return &icon;
        }
    }
    return nullptr;
}

// What clicking a notification does: open its link, or bring its app to the
// front (start it if it is not running).
bool RunNoticeAction(const Notice& n)
{
    if (n.action.rfind(L"uri:", 0) == 0) {
        std::wstring target = n.action.substr(4);
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
    if (n.action.rfind(L"app:", 0) == 0) {
        std::wstring aumid = n.action.substr(4), key = aumid;
        for (auto& c : key)
            c = wchar_t(towlower(c));
        // An app living in the tray (QQ, WeChat, Feishu): its own tray click
        // opens what is new, the chat of the unread message. Starting it again
        // would open a second instance's login window instead.
        if (const TrayIcon* icon = TrayIconOf(n)) {
            AllowSetForegroundWindow(ASFW_ANY);
            g_app.tray->Click(*icon, false);
            Log(L"notify: action through the tray icon of %s", n.app.c_str());
            return true;
        }
        for (const TaskItem& t : g_app.tasks->Snapshot())
            if (t.key == key && !t.windows.empty()) {
                g_app.tasks->Show(key);
                return true;
            }
        // A program path (WeChat): starting it again shows its running instance.
        bool exe = aumid.size() > 2 && aumid[1] == L':';
        std::wstring path = exe ? aumid : L"shell:AppsFolder\\" + aumid;
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
    return true; // nothing to run: the click just dismisses it
}

// Applies settings to the bar and the task model.
void ApplySettings(const Settings& s)
{
    // Popups share the bar's material and corner radius (concentric inside).
    Theme::SetGlass(s.tint, s.transparency);
    float radius = float(std::clamp(s.cornerRadius, 8.0, std::max(8.0, s.height / 2)));
    g_app.start->SetRadius(radius);
    g_app.trayPanel->SetRadius(radius);
    g_app.control->SetRadius(radius);
    g_app.control->SetBarHeight(float(s.height));
    g_app.calendar->SetRadius(radius);
    g_app.calendar->SetBarHeight(float(s.height));
    g_app.start->SetBarHeight(float(s.height)); // its status capsule is shaped like the bar
    g_app.notes->SetRadius(radius);
    g_app.notes->SetOptions(s.notifyTimeout, s.notifyHideBody);
    if (s.notifyListener && !g_app.listener)
        g_app.listener = std::make_unique<NotificationListener>(g_app.host, WM_NOTICE);
    else if (!s.notifyListener && g_app.listener)
        g_app.listener.reset();
    if (s.notifyWeChat && !g_app.wechat)
        g_app.wechat = std::make_unique<WeChatSource>(g_app.host, WM_NOTICE);
    else if (!s.notifyWeChat && g_app.wechat)
        g_app.wechat.reset();
    if (s.notifyFeishu && !g_app.feishu)
        g_app.feishu = std::make_unique<FeishuSource>(g_app.host, WM_NOTICE);
    else if (!s.notifyFeishu && g_app.feishu)
        g_app.feishu.reset();
    g_app.tasks->SetPins(s.pins.value_or(std::vector<PinnedApp>{}));
    g_app.tasks->SetShowRunning(s.showRunning);
    g_app.bar->Apply(s);
    // Widgets live on the bar's monitor, where its capture gives them glass.
    RECT bar = g_app.bar->BarRect();
    g_app.widgets->SetMonitor(MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY));
    g_app.widgets->SetEnabled(s.widgets);
    g_app.widgetsOn = s.widgets;
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

// 32-bit premultiplied DIB for a menu item image.
HBITMAP MenuBitmap(const IconImage& img)
{
    if (img.empty())
        return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = img.width;
    bi.bmiHeader.biHeight = -img.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits)
        memcpy(bits, img.bgra.data(), img.bgra.size());
    return bmp;
}

// The overflow entry: the apps that did not fit, as a menu above the entry.
void ShowOverflow(const std::vector<std::wstring>& keys, RECT anchor)
{
    std::vector<TaskItem> items = g_app.tasks->Snapshot();
    HMENU menu = CreatePopupMenu();
    std::vector<HBITMAP> bitmaps;
    int px = GetSystemMetrics(SM_CXSMICON);
    for (size_t i = 0; i < keys.size(); ++i) {
        auto it = std::find_if(items.begin(), items.end(), [&](const TaskItem& t) { return t.key == keys[i]; });
        std::wstring name = it != items.end() ? it->name : keys[i];
        if (it != items.end() && !it->windows.empty())
            name += it->windows.size() > 1 ? L"  \u00B7 " + std::to_wstring(it->windows.size()) : L"  \u00B7";
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP;
        mi.wID = UINT(1000 + i);
        mi.dwTypeData = name.data();
        mi.hbmpItem = MenuBitmap(g_app.tasks->Icon(keys[i], px));
        if (mi.hbmpItem)
            bitmaps.push_back(mi.hbmpItem);
        InsertMenuItemW(menu, UINT(i), TRUE, &mi);
    }
    SetForegroundWindow(g_app.host);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_CENTERALIGN, (anchor.left + anchor.right) / 2,
                                anchor.top - 8, g_app.host, nullptr);
    DestroyMenu(menu);
    for (HBITMAP b : bitmaps)
        DeleteObject(b);
    PostMessageW(g_app.host, WM_NULL, 0, 0);
    if (cmd >= 1000 && cmd < 1000 + keys.size())
        g_app.tasks->Activate(keys[cmd - 1000]);
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
    case WM_COPYDATA: {
        auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (cd && cd->dwData == 0xA7C0 && cd->cbData % sizeof(wchar_t) == 0 && cd->cbData < 8192)
            RunCommand(std::wstring(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t)));
        return TRUE;
    }
    case WM_NOTICE:
        if (auto* n = reinterpret_cast<Notice*>(l)) {
            // Only from sources still turned on (a notice may arrive just after).
            bool on = n->source == L"wechat" ? bool(g_app.wechat) : n->source == L"feishu" ? bool(g_app.feishu) : bool(g_app.listener);
            if (g_app.notes && on)
                g_app.notes->Post(*n);
            delete n;
        }
        return 0;
    case WM_CATALOG:
        if (g_app.start)
            g_app.start->Refresh();
        return 0;
    case WM_VOLUME:
        UpdateStatus();
        return 0;
    case WM_TRAY_ICONS:
        if (w == 1) // a click was forwarded: see where the focus ends up
            SetTimer(h, kTrayFocusTimer, 600, nullptr);
        else if (g_app.trayPanel)
            g_app.trayPanel->Refresh();
        return 0;
    case WM_TIMER:
        if (w == kTrayTimer) {
            OpenTrayStep2();
            return 0;
        }
        if (w == kStatusTimer) {
            UpdateStatus();
            return 0;
        }
        if (w == kTrayFocusTimer) {
            // Explorer gives the focus to its hidden overflow window when a
            // tray button is invoked. The app usually takes it for its window
            // or menu; if not, it goes back to where it was before the panel.
            // Left there, the hidden window holds the system taskbar up and,
            // as a light-dismiss popup, swallows the next click elsewhere.
            KillTimer(h, kTrayFocusTimer);
            HWND fg = GetForegroundWindow();
            wchar_t cls[64] = {};
            GetClassNameW(fg, cls, 64);
            if (wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 && !IsWindowVisible(fg)) {
                HWND back = g_app.trayPanel->Previous();
                if (!back || !IsWindow(back))
                    back = FindWindowW(L"Progman", nullptr);
                // The focus belongs to Explorer's thread: share its input state
                // for the moment of the switch (the usual way to hand it on).
                DWORD fgThread = GetWindowThreadProcessId(fg, nullptr), me = GetCurrentThreadId();
                bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
                bool ok = SetForegroundWindow(back) != FALSE;
                DWORD error = ok ? 0 : GetLastError();
                if (attached)
                    AttachThreadInput(me, fgThread, FALSE);
                const wchar_t* how = L"attached";
                if (!ok) {
                    // A key press of our own lifts the foreground lock (Alt alone
                    // does nothing in the hidden window that has the keyboard).
                    INPUT alt[2] = {};
                    alt[0].type = alt[1].type = INPUT_KEYBOARD;
                    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
                    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, alt, sizeof(INPUT));
                    ok = SetForegroundWindow(back) != FALSE;
                    how = L"after a key press";
                }
                Log(L"tray: the hidden overflow window had the focus; given back %s (%s, error %lu)", how, ok && GetForegroundWindow() == back ? L"ok" : L"failed",
                    error);
            }
            return 0;
        }
        break;
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
    // `Atrium.Core.exe --command <verb> [argument]` hands a command to the running instance.
    if (const wchar_t* c = wcsstr(cmdLine, L"--command ")) {
        HWND other = FindWindowW(kHostClass, nullptr);
        if (!other)
            return 2;
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        std::wstring text(c + 10);
        COPYDATASTRUCT cd{0xA7C0, DWORD(text.size() * sizeof(wchar_t)), text.data()};
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(other, WM_COPYDATA, 0, LPARAM(&cd), SMTO_ABORTIFHUNG, 5000, &result) ? 0 : 1;
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
    const bool capturable = wcsstr(cmdLine, L"--debug-capturable") != nullptr;
    g_app.bar = std::make_unique<Bar>(instance, capturable, wcsstr(cmdLine, L"--debug-layout") != nullptr);
    // The bar's render thread draws the popups' glass from its capture.
    Surface::SetGlassHost(g_app.bar.get());
    Surface::SetCapturable(capturable);
    g_app.tasks = std::make_unique<TaskModel>(g_app.host, [] {
        g_app.bar->SetItems(g_app.tasks->Snapshot());
        if (g_app.start)
            g_app.start->Refresh();
    });
    g_app.catalog = std::make_unique<AppCatalog>(g_app.host, WM_CATALOG);
    g_app.start = std::make_unique<StartMenu>(instance, *g_app.tasks, *g_app.catalog);
    g_app.start->onSystem = OpenSystem;
    g_app.notes = std::make_unique<Notifications>(instance);
    g_app.tray = std::make_unique<TrayModel>(g_app.host, WM_TRAY_ICONS);
    g_app.trayPanel = std::make_unique<TrayPanel>(instance, *g_app.tray);
    g_app.trayPanel->onSystemTray = OpenTray;
    g_app.control = std::make_unique<ControlCenter>(instance);
    g_app.control->onChanged = UpdateStatus;
    g_app.calendar = std::make_unique<CalendarPanel>(instance);
    g_app.lock = std::make_unique<LockScreen>(instance);
    g_app.widgets = std::make_unique<DesktopWidgets>(instance);
    g_app.widgets->onCalendar = [] {
        if (!g_app.calendar->Visible())
            ToggleCalendar();
    };
    // The bar steps aside while the lock screen shows (and comes back as it was).
    g_app.lock->onBackdrop = [] { g_app.bar->Refreeze(); };
    g_app.lock->onShown = [](bool shown) {
        static bool was = true;
        if (shown) {
            was = g_app.bar->Visible();
            g_app.bar->SetVisible(false);
        } else {
            g_app.bar->SetVisible(was);
        }
    };
    g_app.notes->bellRect = [] { return g_app.bar->EntryRect(L"#notify"); };
    g_app.notes->barRect = [] { return g_app.bar->BarRect(); };
    g_app.notes->onUnread = [](int n) { g_app.bar->SetUnread(n); };
    g_app.notes->onActivate = RunNoticeAction;
    // New cards wait while one of Atrium's panels is open (they would overlap it).
    g_app.notes->held = [] {
        return g_app.start->Visible() || g_app.trayPanel->Visible() || g_app.control->Visible() || g_app.calendar->Visible();
    };
    g_app.bar->iconProvider = [](const std::wstring& key, int px) { return g_app.tasks->Icon(key, px); };
    g_app.bar->onActivate = [](const std::wstring& key) { g_app.tasks->Activate(key); };
    g_app.bar->onLaunch = [](const std::wstring& key) { g_app.tasks->Launch(key); };
    g_app.bar->onItemMenu = ShowItemMenu;
    g_app.bar->onClockClick = ToggleCalendar;
    g_app.bar->onReorder = Reorder;
    g_app.bar->onOverflow = ShowOverflow;
    g_app.bar->onStartClick = ToggleStart;
    g_app.bar->onNotifyClick = [](RECT) { g_app.notes->ToggleCenter(); };
    g_app.bar->onTrayClick = ToggleTray;
    g_app.bar->onStatusClick = ToggleControl;
    g_app.bar->keepShown = [] {
        return g_app.start->Visible() || g_app.trayPanel->Visible() || g_app.control->Visible() || g_app.calendar->Visible() ||
               g_app.notes->CenterVisible();
    };
    g_app.bar->onStatusWheel = [](int notches) {
        AdjustVolume(2 * notches);
        UpdateStatus();
    };
    g_app.bar->onFileDragHover = [](const std::wstring& key) { g_app.tasks->Show(key); };
    g_app.bar->canOpenFiles = [](const std::wstring& key) { return g_app.tasks->CanOpenFiles(key); };
    g_app.bar->onDropFiles = [](const std::wstring& key, const std::vector<std::wstring>& files) { g_app.tasks->OpenFiles(key, files); };
    g_app.bar->onContextMenu = ShowMenu;
    g_app.bar->onCloseRequest = [] { PostMessageW(g_app.host, WM_CLOSE, 0, 0); };
    ApplySettings(settings);
    g_app.bar->SetItems(g_app.tasks->Snapshot());
    g_app.server = std::make_unique<SettingsServer>(g_app.host, settings);
    // The start menu shows the Windows start menu's pins; from there an app
    // can be pinned to the bar (the bar's own pins are separate).
    g_app.start->onPin = [](const PinnedApp& pin) {
        g_app.server->Modify([&](Settings& s) {
            auto pins = s.pins.value_or(std::vector<PinnedApp>{});
            if (std::find(pins.begin(), pins.end(), pin) == pins.end() && pins.size() < 64)
                pins.push_back(pin);
            s.pins = std::move(pins);
        });
    };
    AddTrayIcon();
    UpdateStatus();
    SetTimer(g_app.host, kStatusTimer, kStatusMs, nullptr);
    g_app.tray->Refresh(); // the panel opens with the icons already read

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_app.server.reset();
    g_app.listener.reset();
    g_app.wechat.reset();
    g_app.feishu.reset();
    g_app.notes.reset();
    g_app.widgets.reset();
    g_app.lock.reset();
    g_app.calendar.reset();
    g_app.control.reset();
    g_app.trayPanel.reset();
    g_app.tray.reset();
    g_app.start.reset();
    Surface::SetGlassHost(nullptr);
    g_app.catalog.reset();
    g_app.tasks.reset();
    g_app.bar.reset();
    Log(L"core: exited");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
