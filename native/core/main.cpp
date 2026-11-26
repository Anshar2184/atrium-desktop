// Atrium: the resident process. Owns the glass bar, its popups, the desktop
// widgets, the tray icon and the settings pipe. Explorer's taskbar stays
// unless the user hides it (setting hideTaskbar, shell/system_chrome.h; D37).

#include "app_menus.h"
#include "bar.h"
#include "bar_edge.h"
#include "render/capture.h"
#include "render/shutdown_wait.h"
#include "clock.h"
#include "crash_dump.h"
#include "desktop_helpers.h"
#include "extras_host.h"
#include "guard_process.h"
#include "host_messages.h"
#include "launch_args.h"
#include "localization.h"
#include "log.h"
#include "notice_actions.h"
#include "resource.h"
#include "settings.h"
#include "settings_server.h"
#include "shell/app_catalog.h"
#include "shell/shell_taskbar.h"
#include "shell/app_registration.h"
#include "shell/displays.h"
#include "shell/logon_task.h"
#include "shell/shell_icon.h"
#include "shell/update_checker.h"
#include "shell/task_model.h"
#include "shell/notification_listener.h"
#include "shell/system_chrome.h"
#include "shell/ime_status.h"
#include "shell/system_status.h"
#include "shell/tray_model.h"
#include "shell/usb_volume.h"
#include "shell/jump_list.h"
#include "shell/games.h"
#include "shell/win_key.h"
#include "shell/feishu_source.h"
#include "shell/wechat_source.h"
#include "shell/messengers.h"
#include "ui/calendar_panel.h"
#include "ui/control_center.h"
#include "ui/desktop_widgets.h"
#include "ui/glass_menu.h"
#include "ui/ime_bubble.h"
#include "ui/window_preview.h"
#include "ui/notifications.h"
#include "ui/genie_minimize.h"
#include "ui/stage_manager.h"
#include "ui/start_menu.h"
#include "ui/tray_panel.h"
#include "posted.h"
#include "win32_path.h"

#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <UIAutomation.h>
#include <windowsx.h>

#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace atrium;

constexpr wchar_t kHostClass[] = L"AtriumCoreHost";
constexpr wchar_t kInstanceMutex[] = L"Local\\Atrium.Core";
constexpr UINT_PTR kStatusTimer = 0x52;       // the bar's status icon
constexpr UINT_PTR kTrayFocusTimer = 0x53;    // after a forwarded tray click
constexpr UINT_PTR kDisplaySettleTimer = 0x56; // once more after a display change, the topology settled
constexpr UINT_PTR kMessengerTimer = 0x57;     // a chat app's flash: did a card with its text follow?
constexpr UINT_PTR kReadyTimer = 0x54;       // only during an update startup handshake
constexpr UINT kStatusMs = 3000;
constexpr UINT kTrayId = 1;

struct App {
    HINSTANCE instance = nullptr;
    HWND host = nullptr;
    UINT taskbarCreated = 0;
    bool trayAdded = false;
    std::unique_ptr<Bar> bar;
    std::vector<std::unique_ptr<Bar>> secondaryBars, drainingBars;
    Bar* activeBar = nullptr; // the last taskbar the pointer interacted with
    SystemStatus lastStatus;
    bool capturable = false;
    bool displaysReady = false;
    ULONGLONG switchedBarAt = 0; // switching displays must not suppress the first popup click
    std::unique_ptr<SystemStatusReader> status;
    std::unique_ptr<SettingsServer> server;
    std::unique_ptr<TaskModel> tasks;
    std::unique_ptr<AppCatalog> catalog;
    std::unique_ptr<StartMenu> start;
    std::unique_ptr<Notifications> notes;
    std::unique_ptr<NotificationListener> listener;
    std::unique_ptr<UsbVolumeMonitor> usb;
    std::unique_ptr<UpdateChecker> updates;
    std::unique_ptr<WeChatSource> wechat;
    std::unique_ptr<FeishuSource> feishu;
    MessengerAlerts alerts;
    std::array<std::wstring, size_t(Messenger::Count)> messengerProgram; // the program that flashed last
    std::unique_ptr<TrayModel> tray;
    std::unique_ptr<TrayPanel> trayPanel;
    std::unique_ptr<ExtrasHost> extras; // Launchpad, the now-playing entry, the folder stack
    std::unique_ptr<ControlCenter> control;
    std::unique_ptr<CalendarPanel> calendar;
    std::unique_ptr<DesktopWidgets> widgets;
    std::unique_ptr<SystemChrome> chrome;
    std::unique_ptr<GlassMenu> menu;
    std::unique_ptr<WindowPreview> preview;
    std::unique_ptr<StageManager> stage;
    std::unique_ptr<GenieMinimize> genie;
    std::unique_ptr<WinKey> winKey;
    std::unique_ptr<ImeWatcher> imeWatcher; // the events that may change the input method (issue #56)
    ImeWatch imeWatch;                     // what announces (a foreground change only reseeds it)
    std::unique_ptr<ImeBubble> ime;        // the bubble that announces while the bar is hidden
    bool imeOn = true;                     // the "show the input language" setting, as last applied
    bool imeBubble = true;                 // imeBubble, as last applied
    HANDLE guard = nullptr; // the guard process (EnsureGuard)
    bool winKeyStart = false, winNumbers = false; // what it watches
    bool widgetsOn = false;
    std::wstring desktopDisplay; // effective setting, also available during display broadcasts
    Settings displaySettings;
    std::optional<bool> barOn; // the bar module, as last applied
    int brightness = -1; // the built-in screen's, from GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS
    HPOWERNOTIFY brightnessNotify = nullptr;
    HDEVNOTIFY usbNotify = nullptr; // volume interface events, for the USB card
};

App g_app;
std::wstring g_readyEvent;

Bar& ActiveBar() { return *(g_app.activeBar ? g_app.activeBar : g_app.bar.get()); }
Bar* WinKeyBar() { return g_app.barOn.value_or(true) ? (g_app.activeBar ? g_app.activeBar : g_app.bar.get()) : nullptr; }

template<class Action> void ForBars(const Action& action)
{
    action(*g_app.bar);
    for (const auto& bar : g_app.secondaryBars)
        action(*bar);
}

// A settled change also shows in the bottom bubble, but only while the bar on
// that display is hidden — its input method entry already shows the glyph
// otherwise. A fullscreen window in front (a game, a video: it covers the
// screen, not just its work area) keeps the bubble quiet.
void ShowImeBubble(HWND foreground, const ImeStatus& status)
{
    if (!g_app.barOn.value_or(true) || !g_app.ime || !g_app.imeBubble)
        return; // the bar is off with Windows' taskbar back: it shows its own
    const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    Bar* bar = g_app.bar && g_app.bar->Monitor() == monitor ? g_app.bar.get() : nullptr;
    if (!bar)
        for (const auto& secondary : g_app.secondaryBars)
            if (secondary->Monitor() == monitor) {
                bar = secondary.get();
                break;
            }
    if (bar && bar->ShownOnScreen())
        return;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return;
    if (FullScreenInFront(monitor))
        return; // nothing is typed over a fullscreen window
    g_app.ime->Show(status, info.rcWork, int(info.rcMonitor.bottom));
    Log(L"ime: %s (layout %08lX) as the bubble", ImeGlyph(status).c_str(), status.layout);
}
void OnImeSample(ImeWatcher::Sample& sample)
{
    if (!g_app.imeOn || !g_app.imeWatcher)
        return;
    ImeStatus status = sample.status;
    status.caps = status.upper = g_app.imeWatcher->CapsLock();
    const bool changed = g_app.imeWatch.Update(sample.foreground, status);
    const ImeStatus& shown = g_app.imeWatch.State();
    if (shown.Empty())
        return;
    const std::wstring glyph = ImeGlyph(shown);
    ForBars([&](Bar& bar) { bar.SetIme(glyph); });
    if (changed) {
        Log(L"ime: %s (layout %08lX)", glyph.c_str(), shown.layout);
        ShowImeBubble(sample.foreground, shown);
    }
}

void ConfigureBar(Bar& bar);
void CloseBarPopups();
void SyncDisplayBars(const Settings& settings, bool refresh = false, bool workAreaOnly = false);

// GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS: Windows reports every change of the
// built-in screen's brightness (keys, quick settings, our slider), and the
// current level once on registration.
constexpr GUID kBrightnessSetting = {0x8ffee2c6, 0x2d01, 0x46be, {0xad, 0xb9, 0x39, 0x8a, 0xdd, 0xc5, 0xb4, 0xff}};
constexpr GUID kVolumeInterface = {0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}}; // GUID_DEVINTERFACE_VOLUME

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\'));
}

bool SameCoreExecutable(HWND window) { return atrium::SameExecutableWindow(window); }

// page: the settings app opens on it ("widgets"); an open window just comes to the front.
void OpenSettings(const wchar_t* page = nullptr)
{
    std::wstring dir = ExeDir();
    std::vector<std::wstring> candidates = {dir + L"\\Settings\\Atrium.Settings.exe", dir + L"\\Atrium.Settings.exe"};
    // A development build (native\build\win-x64\Release): the packaged settings app of the same repository.
    for (std::wstring up = dir; IsDevelopmentBuild(); candidates.push_back(up + L"\\dist\\Development\\Atrium\\Settings\\Atrium.Settings.exe")) {
        const size_t pos = up.find_last_of(L'\\');
        if (pos == std::wstring::npos || pos <= 2)
            break;
        up.resize(pos);
    }
    const std::wstring args = FormatSettingsLaunchArgs(page, DataDirectoryArgs());
    for (const std::wstring& exe : candidates) {
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        // The settings app keeps a single instance itself and activates it.
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
            return;
    }
    Log(L"settings: Atrium.Settings.exe not found next to %s", dir.c_str());
    MessageBoxW(nullptr, Tr(L"Atrium.Settings.exe was not found. Make sure the Settings folder in the installation directory is complete."), L"Atrium",
                MB_ICONWARNING);
}

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = static_cast<HICON>(
        LoadImageW(g_app.instance, MAKEINTRESOURCEW(IDI_ATRIUM), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(nid.szTip, L"Atrium");
    g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    const DWORD err = g_app.trayAdded ? 0 : GetLastError();
    DestroyIcon(nid.hIcon);
    static bool failLogged = false;
    static DWORD lastErr = 0;
    static HWND lastFirst = nullptr;
    if (!g_app.trayAdded) {
        HWND first = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!failLogged || err != lastErr || first != lastFirst) {
            failLogged = true;
            lastErr = err;
            lastFirst = first;
            const bool hosting = g_app.tray && g_app.tray->Hosting();
            const bool yielded = g_app.tray && g_app.tray->HostYielded();
            const auto reason = g_app.tray ? g_app.tray->HostYieldReason() : TrayHostYieldReason::None;
            Log(L"tray: could not add Atrium's notification icon (err=%lu first=0x%p host=%d yielded=%d reason=%s); will retry", err, static_cast<void*>(first),
                hosting ? 1 : 0, yielded ? 1 : 0, YieldReasonName(reason));
            LogShellTrayWindows();
        }
        return;
    }
    failLogged = false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid))
        Log(L"tray: could not set notification icon version");
    Log(L"tray: Atrium notification icon added");
}

void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.trayAdded = false;
}

// Win+<key> (or Win+Ctrl+<key>), a public system shortcut (D11).
// UIA and popup discovery run on the tray worker, including explicit fallback.
void OpenTray()
{
    if (g_app.tray)
        g_app.tray->OpenSystemTray();
}

void OpenSystem(const std::wstring& what)
{
    if (what == L"network" || what == L"volume" || what == L"battery") {
        g_app.control->Open(ActiveBar().EntryRect(L"#status"), ActiveBar().BarRect());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry %s (control centre)", what.c_str());
        return;
    }
    if (what == L"lock") {
        LockWorkStation();
        return;
    }
    if (what == L"tray") {
        g_app.trayPanel->Open(ActiveBar().EntryRect(L"#tray"), ActiveBar().BarRect(), ActiveBar().PreviousForeground());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry tray (tray panel)");
        return;
    }
    if (what == L"account")
        ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"start: system entry %s", what.c_str());
}

void UpdateStatus()
{
    if (g_app.status)
        g_app.status->Refresh();
}

// Atrium's tray panel and control centre open above their entries on the
// bar; a click on the entry while one is open closes it (and must not reopen
// it: the press that deactivated it is the same click).
template<class Panel, class OpenFn> void TogglePopup(Panel& panel, OpenFn open)
{
    if (panel.Visible()) {
        panel.Close();
        return;
    }
    if (GetTickCount64() - panel.LastClosed() < 400 && panel.LastClosed() > g_app.switchedBarAt)
        return;
    open();
    g_app.notes->PanelsChanged();
}

void ToggleTray(RECT)
{
    TogglePopup(*g_app.trayPanel, [] { g_app.trayPanel->Open(ActiveBar().EntryRect(L"#tray"), ActiveBar().BarRect(), ActiveBar().PreviousForeground()); });
}

void ToggleControl(RECT)
{
    TogglePopup(*g_app.control, [] { g_app.control->Open(ActiveBar().EntryRect(L"#status"), ActiveBar().BarRect()); });
}

void ToggleCalendar()
{
    TogglePopup(*g_app.calendar, [] { g_app.calendar->Open(ActiveBar().EntryRect(L"#clock"), ActiveBar().BarRect()); });
}

void ToggleStart(RECT)
{
    if (g_app.start->Visible()) {
        g_app.start->Close(false);
        return;
    }
    // A click on the logo while the menu is open first deactivates (closes) it;
    // that click must not open it again.
    if (GetTickCount64() - g_app.start->LastClosed() < 400 && g_app.start->LastClosed() > g_app.switchedBarAt)
        return;
    g_app.start->Open(ActiveBar().BarRect(), ActiveBar().EntryRect(L"#start"), ActiveBar().PreviousForeground());
    g_app.notes->PanelsChanged();
}

// The bar module on or off (its menu, the tray icon's). Off, its popups close and Windows' own
// taskbar and banners take over again (ApplySettings).
void SetBarModule(bool on)
{
    g_app.server->Modify([on](Settings& s) { s.bar = on; });
}

void SetWidgetsModule(bool on)
{
    g_app.server->Modify([on](Settings& s) { s.widgets = on; });
}

// Stage Manager from the bar (its menu, the control centre); the widgets go to the other side.
void SetStageManager(bool on)
{
    g_app.server->Modify([on](Settings& s) {
        s.stageManager = on;
        if (on)
            s.widgetsOnRight = !s.stageOnRight;
    });
}

AppMenuEnv MenuEnv()
{
    return {
        g_app.host,
        g_app.menu.get(),
        g_app.tasks.get(),
        g_app.server.get(),
        g_app.stage.get(),
        []() -> Bar& { return ActiveBar(); },
        [](const wchar_t* page) { OpenSettings(page); },
        [] { return g_app.barOn.value_or(true); },
        [] { return g_app.widgetsOn; },
        SetBarModule,
        SetWidgetsModule,
        SetStageManager,
    };
}

void ShowMenu(POINT pt) { ShowBarContextMenu(MenuEnv(), pt); }

void ShowStartMenu(POINT pt) { ShowStartContextMenu(MenuEnv(), pt); }

NoticeActionEnv NoticeEnv()
{
    return {
        g_app.tray.get(),
        g_app.tasks.get(),
        g_app.wechat.get(),
        [](const wchar_t* page) { OpenSettings(page); },
    };
}

// The chat apps' own switches (Settings ▸ Notifications ▸ Sources); other apps' Windows
// notifications follow notifyListener.
bool MessengerOn(Messenger m)
{
    const Settings& s = g_app.displaySettings;
    switch (m) {
    case Messenger::WeChat: return s.notifyWeChat;
    case Messenger::QQ: return s.notifyQQ;
    case Messenger::Feishu: return s.notifyFeishu;
    case Messenger::WeCom: return s.notifyWeCom;
    case Messenger::DingTalk: return s.notifyDingTalk;
    default: return s.notifyListener;
    }
}

// A chat app flashed for a message (its window, or its tray icon began to): a plain card follows
// unless one with the message's text does (MessengerAlerts).
void MessengerFlashed(const std::wstring& program)
{
    const Messenger m = MessengerOfProgram(program);
    if (m == Messenger::None || !g_app.barOn.value_or(true) || !MessengerOn(m))
        return;
    g_app.messengerProgram[size_t(m)] = FoldCase(program);
    if (m == Messenger::WeChat && g_app.wechat)
        g_app.wechat->Wake();
    if (g_app.alerts.Flash(m, GetTickCount64()))
        SetTimer(g_app.host, kMessengerTimer, UINT(MessengerAlerts::kWaitMs + 100), nullptr);
}

void PostMessengerCard(Messenger m)
{
    if (!g_app.notes || !g_app.barOn.value_or(true) || !MessengerOn(m))
        return;
    if (HWND fg = GetForegroundWindow(); fg && MessengerOfProgram(ProgramOf(fg)) == m)
        return; // the app is in front: the user sees the message there
    if (m == Messenger::WeChat && g_app.wechat && g_app.wechat->Reading())
        return; // its chat list brings the card, with the text (a flash comes first, and again)
    const MessengerInfo& info = InfoOf(m);
    Log(L"notify: %s flashed with no card: a plain one", info.key);
    const std::wstring& program = g_app.messengerProgram[size_t(m)];
    Notice n;
    n.source = info.key;
    n.app = Tr(info.name);
    n.appId = program;
    n.title = Tr(L"New message");
    if (!program.empty())
        n.action = L"app:" + program;
    g_app.notes->Post(std::move(n));
}

std::vector<TaskItem> BarItems();

void ApplyImeSettings(const Settings& s)
{
    g_app.imeOn = s.imeStatus;
    g_app.imeBubble = s.imeBubble;
    if (!s.imeBubble && g_app.ime)
        g_app.ime->Cancel();
    if (g_app.imeOn && !g_app.imeWatcher) {
        g_app.imeWatcher = std::make_unique<ImeWatcher>(g_app.host, WM_IME_STATE);
        g_app.imeWatch.Reset();
    } else if (!g_app.imeOn && g_app.imeWatcher) {
        g_app.imeWatcher.reset();
        g_app.imeWatch.Reset();
        ForBars([](Bar& bar) { bar.SetIme(L""); });
        if (g_app.ime)
            g_app.ime->Cancel(); // a bubble already on screen goes with the setting, not when its life ends
    }
}

void ApplyNotificationSources(const Settings& s, bool badgesChanged)
{
    const bool toasts = s.notifyListener || s.notifyQQ || s.notifyWeChat || s.notifyFeishu || s.notifyWeCom || s.notifyDingTalk;
    if (s.bar && toasts && !g_app.listener)
        g_app.listener = std::make_unique<NotificationListener>(g_app.host, WM_NOTICE);
    else if (!(s.bar && toasts) && g_app.listener)
        g_app.listener.reset();
    if (g_app.listener)
        g_app.listener->SetAlways(s.notifyAlways);
    const bool needWeChat = s.bar && (s.notifyWeChat || s.barBadges);
    if (needWeChat && !g_app.wechat)
        g_app.wechat = std::make_unique<WeChatSource>(g_app.host, WM_NOTICE, WM_BADGES);
    else if (!needWeChat && g_app.wechat)
        g_app.wechat.reset();
    if (g_app.wechat) {
        g_app.wechat->SetBlockOfficial(s.wechatBlockOfficial);
        g_app.wechat->SetPostCards(s.notifyWeChat);
    }
    if (badgesChanged && g_app.tasks && g_app.bar) {
        const std::vector<TaskItem> items = BarItems();
        ForBars([&](Bar& bar) { bar.SetItems(items); });
    }
    if (s.bar && s.notifyFeishu && !g_app.feishu)
        g_app.feishu = std::make_unique<FeishuSource>(g_app.host, WM_NOTICE);
    else if (!(s.bar && s.notifyFeishu) && g_app.feishu)
        g_app.feishu.reset();
}

void ApplyDesktopAndSystemSettings(const Settings& s)
{
    const HMONITOR desktop = ResolveDisplay(ConnectedDisplays(), s.desktopDisplay);
    g_app.widgets->SetMonitor(desktop);
    g_app.widgets->SetOnRight(s.stageManager ? !s.stageOnRight : s.widgetsOnRight);
    g_app.stage->SetMonitor(desktop), g_app.stage->SetOnRight(s.stageOnRight), g_app.stage->SetAutoHide(s.stageAutoHide);
    g_app.stage->SetMinimizeToStrip(s.stageMinimize), g_app.stage->SetDragMerge(s.stageDragMerge);
    g_app.stage->SetExcluded(s.stageExcluded), SetUserGames(s.stageExcluded);
    g_app.stage->SetCloseButton(s.stageCloseButton), g_app.stage->SetScrollWrap(s.stageScrollWrap);
    g_app.stage->SetAvoid(s.stageAvoid), g_app.stage->SetEnabled(s.stageManager);
    g_app.genie->SetEnabled(s.genieMinimize && s.bar);
    g_app.preview->SetPeek(s.windowPeek);
    g_app.control->SetStage(s.stageManager, s.stageOnRight);
    g_app.control->SetModules(s.controlModules);
    g_app.widgets->SetMaterial(s.widgetMaterial == L"dusk"  ? WidgetMaterial::Dusk :
                               s.widgetMaterial == L"color" ? WidgetMaterial::Color :
                                                              WidgetMaterial::Glass);
    g_app.widgets->SetEnabled(s.widgets);
    g_app.widgetsOn = s.widgets;

    const bool keyStart = s.bar && s.winKeyStart && !LowIntegrity(), keyNumbers = s.bar && s.winNumbers && !LowIntegrity();
    if (keyStart != g_app.winKeyStart || keyNumbers != g_app.winNumbers || bool(g_app.winKey) != (keyStart || keyNumbers)) {
        g_app.winKey.reset();
        if (keyStart || keyNumbers)
            g_app.winKey = std::make_unique<WinKey>(g_app.host, WM_WIN_KEY, keyStart, keyNumbers);
        g_app.winKeyStart = keyStart;
        g_app.winNumbers = keyNumbers;
    }
    if (g_app.tray && s.trayHost != g_app.tray->Hosting()) {
        g_app.tray->SetHosting(s.trayHost);
        Log(L"core: tray host %s", g_app.tray->Hosting() ? L"on" : s.trayHost ? L"failed to start" : L"off");
    }
    if (s.remoteVisible != RemoteVisible()) {
        SetRemoteVisible(s.remoteVisible);
        RefreshCaptureExclusion();
        Log(L"core: remote-control mode %s", s.remoteVisible ? L"on: Atrium shows in every capture, the glass shows the wallpaper" : L"off");
    }
    if (g_app.chrome) {
        g_app.chrome->SetTaskbarHidden(s.bar && s.hideTaskbar);
        g_app.chrome->SetBannersHidden(s.bar && s.hideBanners);
        if ((s.bar && (s.hideTaskbar || s.hideBanners)) || s.trayHost)
            EnsureGuard(g_app.guard);
    }
}

// Applies settings to the bar and the task model.
void ApplySettings(const Settings& s)
{
    if (SetLanguage(s.language) && g_app.widgets)
        g_app.widgets->ReloadLanguage();
    ApplyImeSettings(s);
    const bool badgesChanged = g_app.displaySettings.barBadges != s.barBadges;
    g_app.desktopDisplay = s.desktopDisplay;
    g_app.displaySettings = s;
    Theme::SetGlass(s.tint, s.transparency);
    const float radius = float(std::clamp(s.cornerRadius, 8.0, std::max(8.0, s.height / 2)));
    g_app.start->SetRadius(radius);
    g_app.trayPanel->SetRadius(radius);
    g_app.extras->Apply(s, radius);
    g_app.control->SetRadius(radius);
    g_app.control->SetBarHeight(float(s.height));
    g_app.calendar->SetRadius(radius);
    g_app.calendar->SetBarHeight(float(s.height));
    g_app.start->SetBarHeight(float(s.height));
    g_app.start->SetShowTray(s.startTray);
    g_app.start->SetShowStatus(s.startStatus);
    g_app.start->SetCentered(s.startCentered);
    g_app.start->SetPreferredSize(s.startSize);
    g_app.start->SetPreferredIcon(s.startIcon);
    g_app.notes->SetRadius(radius);
    g_app.notes->SetOptions(s.notifyTimeout, s.notifyHideBody, s.notifyPlacement, s.notifyFullscreen, s.notifyAlways);
    ApplyNotificationSources(s, badgesChanged);
    g_app.tasks->SetPins(s.pins.value_or(std::vector<PinnedApp>{}));
    g_app.tasks->SetShowRunning(s.showRunning);
    SyncDisplayBars(s);
    if (g_app.barOn != s.bar) {
        g_app.barOn = s.bar;
        if (!s.bar)
            CloseBarPopups();
        ForBars([&](Bar& bar) { bar.SetVisible(s.bar); });
        Log(L"modules: bar %s", s.bar ? L"on" : L"off");
    }
    ApplyDesktopAndSystemSettings(s);
}

void ShowItemMenu(const std::wstring& key, POINT pt) { ShowTaskItemMenu(MenuEnv(), key, pt); }

void ShowCatalogMenu(const CatalogApp& app, POINT pt, std::function<void(bool)> done) { ShowCatalogAppMenu(MenuEnv(), app, pt, std::move(done)); }

void ShowOverflow(const std::vector<std::wstring>& keys, RECT anchor) { ShowOverflowAppsMenu(MenuEnv(), keys, anchor); }

void Reorder(const std::wstring& key, int to) { ReorderBarTask(MenuEnv(), key, to); }

// A popup of the bar's is open: the notification cards keep clear of it.
bool BarPopupOpen()
{
    return g_app.start->Visible() || g_app.trayPanel->Visible() || g_app.control->Visible() || g_app.calendar->Visible() || g_app.extras->AnyPopupVisible();
}

void CloseBarPopups()
{
    g_app.extras->ClosePopups();
    g_app.start->Close(false);
    g_app.trayPanel->Close();
    g_app.control->Close();
    g_app.calendar->Close();
    g_app.menu->Close();
    g_app.preview->Close();
    if (g_app.notes->CenterVisible())
        g_app.notes->ToggleCenter();
}

void ConfigureBar(Bar& bar)
{
    bar.onInteract = [&bar] {
        if (g_app.activeBar == &bar)
            return;
        CloseBarPopups();
        g_app.activeBar = &bar;
        g_app.switchedBarAt = GetTickCount64();
        g_app.notes->PanelsChanged();
    };
    bar.iconProvider = [](const std::wstring& key, int px) { return g_app.tasks->Icon(key, px); };
    bar.onActivate = [&bar](const std::wstring& key) { g_app.tasks->Activate(key, bar.TaskMonitor()); };
    bar.onLaunch = [](const std::wstring& key) { g_app.tasks->Launch(key); };
    bar.onItemMenu = ShowItemMenu;
    bar.onClockClick = ToggleCalendar;
    bar.onReorder = Reorder;
    bar.onOverflow = ShowOverflow;
    bar.onStartClick = ToggleStart;
    bar.onStartMenu = ShowStartMenu;
    bar.onNotifyClick = [](RECT) { g_app.notes->ToggleCenter(); };
    bar.onTrayClick = ToggleTray;
    bar.onDesktopClick = [] { SendWinShortcut('D'); }; // the system's Show Desktop: a second click brings the windows back
    bar.onStatusClick = ToggleControl;
    g_app.extras->ConfigureBar(bar);
    bar.onImeClick = [] {
        if (g_app.imeWatcher)
            g_app.imeWatcher->Toggle();
    };
    if (!g_app.imeWatch.State().Empty())
        bar.SetIme(ImeGlyph(g_app.imeWatch.State()));
    bar.keepShown = [&bar] { return g_app.activeBar == &bar && (BarPopupOpen() || g_app.notes->CenterVisible() || g_app.menu->Visible()); };
    bar.onStatusWheel = [](int notches) {
        AdjustVolume(2 * notches);
        UpdateStatus();
    };
    bar.onFileDragHover = [](const std::wstring& key) { g_app.tasks->Show(key); };
    bar.canOpenFiles = [](const std::wstring& key) { return g_app.tasks->CanOpenFiles(key); };
    bar.onDropFiles = [](const std::wstring& key, const std::vector<std::wstring>& files) { g_app.tasks->OpenFiles(key, files); };
    bar.onContextMenu = ShowMenu;
    bar.onPreview = [&bar](const std::wstring& key, const std::vector<HWND>& windows, POINT anchor) {
        g_app.preview->Show(key, windows, anchor, bar.BarRect(), g_app.tasks->Icon(key, 64));
    };
    bar.onPreviewLeave = [&bar] {
        if (g_app.activeBar == &bar)
            g_app.preview->Leave();
    };
    bar.onPreviewClose = [&bar] {
        if (g_app.activeBar == &bar)
            g_app.preview->Close();
    };
    bar.previewOpen = [&bar] { return g_app.activeBar == &bar && g_app.preview->Visible(); };
    bar.onMonitorChanged = [&bar] {
        if (g_app.activeBar == &bar) {
            CloseBarPopups();
            g_app.notes->PanelsChanged();
        }
    };
    bar.onItemsChanged = [&bar] {
        if (g_app.activeBar == &bar)
            g_app.preview->Refresh(bar.Items());
    };
    bar.onCloseRequest = [] { PostMessageW(g_app.host, WM_CLOSE, 0, 0); };
}

Messenger ItemMessenger(const TaskItem& t, const PinnedApp& pin, std::wstring& exeLower)
{
    exeLower = FoldCase(pin.path);
    if (!exeLower.empty()) {
        if (const Messenger m = MessengerOfProgram(exeLower); m != Messenger::None)
            return m;
    }
    const std::wstring aumid = FoldCase(!pin.aumid.empty() ? pin.aumid : t.key);
    if (!aumid.empty()) {
        if (const Messenger m = MessengerOfNotice(L"windows", t.name, aumid); m != Messenger::None)
            return m;
        if (aumid.find(L"tencentqq") != std::wstring::npos || aumid.find(L".qq") != std::wstring::npos || aumid.find(L"qqdesktop") != std::wstring::npos ||
            aumid.find(L"qqnt") != std::wstring::npos)
            return Messenger::QQ;
    }
    return Messenger::None;
}

// The bar's items, lit for attention also where an app's tray icon flashes (QQ and WeChat flash
// theirs for a new message while they sit in the tray, with no window to flash).
std::vector<TaskItem> BarItems()
{
    std::vector<TaskItem> items = g_app.tasks->Snapshot();
    const bool badgesOn = g_app.displaySettings.barBadges;
    if (!badgesOn) {
        for (TaskItem& t : items)
            t.badge = 0;
    }
    const std::vector<std::wstring> flashing = g_app.tray ? g_app.tray->Flashing() : std::vector<std::wstring>{};
    const int wechatUnread = (badgesOn && g_app.wechat) ? g_app.wechat->Unread() : 0;
    if (flashing.empty() && wechatUnread <= 0)
        return items;
    for (TaskItem& t : items) {
        const bool needFlash = !t.attention && !flashing.empty();
        const bool needWeChat = wechatUnread > 0;
        if (!needFlash && !needWeChat)
            continue;
        const PinnedApp pin = g_app.tasks->PinFor(t.key);
        std::wstring exe;
        const Messenger messenger = ItemMessenger(t, pin, exe);
        if (needFlash) {
            if (!exe.empty())
                t.attention = std::ranges::find(flashing, exe) != flashing.end();
            else if (messenger != Messenger::None)
                t.attention = std::ranges::any_of(flashing, [messenger](const std::wstring& f) { return MessengerOfProgram(f) == messenger; });
        }
        if (needWeChat && messenger == Messenger::WeChat)
            t.badge = std::max(t.badge, wechatUnread);
    }
    return items;
}

void SyncDisplayBars(const Settings& settings, bool refresh, bool workAreaOnly)
{
    const auto displays = ConnectedDisplays();
    // Keep existing renderers when changing appearance; only retire disconnected or unused bars.
    const bool all = settings.bar && settings.dockDisplayMode == L"all";
    std::vector<std::unique_ptr<Bar>> retired;
    bool closeActive = false;
    std::erase_if(g_app.secondaryBars, [&](auto& bar) {
        if (all && std::ranges::any_of(displays, [&](const DisplayInfo& d) { return !d.primary && d.id == bar->DisplayOverride(); }))
            return false;
        if (g_app.activeBar == bar.get())
            g_app.activeBar = g_app.bar.get(), closeActive = true;
        retired.push_back(std::move(bar));
        return true;
    });
    if (closeActive)
        CloseBarPopups();
    for (const auto& bar : retired)
        g_app.extras->OnBarRetiring(bar.get());
    std::ranges::for_each(retired, [](const auto& bar) { bar->SetVisible(false), bar->DetachInput(), bar->SignalStopRendering(); });
    const size_t beforeSlow = g_app.drainingBars.size();
    CollectSlowWithinDeadline(retired, g_app.drainingBars, 200, [](const auto& bar, DWORD ms) { return bar->WaitStopRendering(ms); });
    if (g_app.drainingBars.size() > beforeSlow)
        Log(L"core: %zu secondary display renderer(s) slow during removal; draining", g_app.drainingBars.size() - beforeSlow);
    if (all)
        for (const DisplayInfo& display : displays) {
            if (display.primary || std::ranges::any_of(g_app.secondaryBars, [&](const auto& bar) { return bar->DisplayOverride() == display.id; }))
                continue;
            auto bar = std::make_unique<Bar>(g_app.instance, g_app.capturable, display.id);
            ConfigureBar(*bar);
            bar->Apply(settings);
            bar->SetStatus(g_app.lastStatus);
            bar->SetUnread(g_app.notes->Unread());
            g_app.secondaryBars.push_back(std::move(bar));
        }
    const auto items = BarItems();
    ForBars([&](Bar& bar) {
        if (refresh)
            bar.RefreshDisplays(workAreaOnly);
        bar.Apply(settings);
        bar.SetItems(items);
        bar.SetVisible(settings.bar);
    });
    const BarEdge edge = ParseBarEdge(settings.barEdge);
    g_app.widgets->SetBarEdge(edge);
    g_app.stage->SetBarEdge(edge);
    g_app.notes->PanelsChanged();
}

void UpdateDesktopMonitor(bool notify)
{
    HMONITOR desktop = ResolveDisplay(ConnectedDisplays(), g_app.desktopDisplay);
    g_app.widgets->SetMonitor(desktop, true);
    g_app.stage->SetMonitor(desktop, true);
    if (notify)
        g_app.notes->PanelsChanged();
}

void HandleTrayCallback(WPARAM w, LPARAM l)
{
    switch (LOWORD(l)) {
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP: {
        POINT at{GET_X_LPARAM(w), GET_Y_LPARAM(w)}, cursor;
        if (GetCursorPos(&cursor) && (std::abs(at.x - cursor.x) > 48 || std::abs(at.y - cursor.y) > 48))
            at = cursor;
        ShowMenu(at);
        return;
    }
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK: OpenSettings(); return;
    default: break;
    }
}

void HandleWinKeyMessage(WPARAM w)
{
    Bar* bar = WinKeyBar();
    if (!bar)
        return;
    if (w) {
        HWND fg = GetForegroundWindow();
        ForegroundAttach attach(fg);
        AllowSetForegroundWindow(ASFW_ANY);
        bar->ActivateIndex(int(w) - 1);
    } else if (g_app.start->Visible()) {
        g_app.start->Close(false);
    } else if (const RECT r = bar->BarRect(); GameInFront(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST)) && bar->RevealOverGame()) {
        Log(L"winkey: the bar over a game");
    } else {
        g_app.activeBar = bar;
        HWND fg = GetForegroundWindow();
        {
            ForegroundAttach attach(fg);
            if (g_app.displaySettings.startWithBar)
                bar->Reveal();
            g_app.start->Open(bar->BarRect(), bar->EntryRect(L"#start"), fg);
        }
        g_app.notes->PanelsChanged();
        if (GetForegroundWindow() != g_app.start->Hwnd())
            Log(L"winkey: the start menu did not get the focus");
    }
}

void HandleNoticeMessage(LPARAM l)
{
    if (auto n = TakePosted<Notice>(l)) {
        if (!n->unread) {
            if (g_app.notes)
                g_app.notes->Post(*n);
            return;
        }
        const Messenger messenger = MessengerOfNotice(n->source, n->app, n->appId);
        bool on = n->source == L"atrium" ? g_app.barOn.value_or(true) :
                  n->source == L"wechat" ? bool(g_app.wechat) :
                  n->source == L"feishu" ? bool(g_app.feishu) :
                                           bool(g_app.listener);
        on = on && (messenger != Messenger::None ? MessengerOn(messenger) : n->source != L"windows" || g_app.displaySettings.notifyListener);
        if (g_app.notes && on) {
            if (n->source == L"wechat" && n->unread && (n->title != Tr(L"New message") || !n->body.empty()))
                g_app.notes->Post(Notice{.source = L"wechat", .title = Tr(L"New message"), .unread = false});
            g_app.notes->Post(*n);
            if (n->unread)
                g_app.alerts.Noticed(messenger, GetTickCount64());
        }
    }
}

bool HandleDeviceBroadcast(WPARAM w, LPARAM l)
{
    if (!g_app.usb || (w != DBT_DEVICEARRIVAL && w != DBT_DEVICEREMOVEPENDING && w != DBT_DEVICEQUERYREMOVEFAILED && w != DBT_DEVICEREMOVECOMPLETE))
        return false;
    UsbVolumes::Event event;
    event.kind = UINT(w);
    if (const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(l); header && header->dbch_devicetype == DBT_DEVTYP_VOLUME) {
        event.unitmask = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(l)->dbcv_unitmask;
    } else if (!header || header->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE ||
               reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(l)->dbcc_classguid != kVolumeInterface) {
        return false;
    }
    g_app.usb->Changed(event);
    return true;
}

void HandleUsbMessage()
{
    if (!g_app.usb)
        return;
    const std::vector<RemovedVolume> removed = g_app.usb->TakeRemoved();
    if (removed.empty() || !g_app.notes || !g_app.barOn.value_or(true))
        return;
    std::vector<std::wstring> names;
    bool ejected = true;
    for (const auto& volume : removed) {
        names.push_back(VolumeName(volume.label, volume.letter));
        if (!volume.ejected)
            ejected = false;
    }
    Notice notice;
    notice.source = L"usb";
    notice.app = L"Atrium";
    const std::wstring list = JoinNames(names);
    if (ejected) {
        notice.title = Tr(L"Safe to remove hardware");
        notice.body = TrFormat(L"\"%s\" can now be safely unplugged.", list.c_str());
    } else {
        notice.title = Tr(L"USB device removed");
        notice.body = TrFormat(L"\"%s\" has been removed from the computer.", list.c_str());
    }
    g_app.notes->Post(std::move(notice));
}

void HandleTrayIconsMessage(HWND h, WPARAM w)
{
    if (w == 4 || w == 1) {
        SetTimer(h, kTrayFocusTimer, 600, nullptr);
        return;
    }
    if (w == 5) {
        if (g_app.trayPanel)
            g_app.trayPanel->Close();
        return;
    }
    if (w == 2 || w == 3) {
        KillTimer(h, kTrayFocusTimer);
        if (g_app.trayPanel)
            g_app.trayPanel->Close();
        static ULONGLONG lastTrayNotice = 0;
        const auto now = GetTickCount64();
        if (g_app.notes && (!lastTrayNotice || now - lastTrayNotice >= 30000)) {
            lastTrayNotice = now;
            Notice notice;
            notice.source = L"atrium";
            notice.app = L"Atrium";
            notice.title = Tr(w == 2 ? L"Use the Windows notification area" : L"Windows notification area is unavailable");
            notice.body = Tr(w == 2 ? L"Select the icon in the Windows hidden icons panel." :
                                      L"Temporarily turn off Hide system taskbar, open the hidden icons panel, and try again.");
            g_app.notes->Post(std::move(notice));
        }
        return;
    }
    if (g_app.trayPanel)
        g_app.trayPanel->Refresh();
    static std::vector<std::wstring> lastFlashing;
    std::vector<std::wstring> flashing = g_app.tray ? g_app.tray->Flashing() : std::vector<std::wstring>{};
    std::ranges::sort(flashing);
    if (flashing != lastFlashing && g_app.tasks) {
        for (const std::wstring& exe : flashing)
            if (!std::ranges::binary_search(lastFlashing, exe))
                MessengerFlashed(exe);
        lastFlashing = std::move(flashing);
        const auto items = BarItems();
        ForBars([&](Bar& bar) { bar.SetItems(items); });
    }
}

void TrySignalReadyEvent(HWND h)
{
    if (g_readyEvent.empty()) {
        KillTimer(h, kReadyTimer);
        return;
    }
    if (!g_app.server || !g_app.server->Ready())
        return;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, g_readyEvent.c_str());
    if (event) {
        SetEvent(event);
        CloseHandle(event);
        Log(L"core: ready event signalled");
    } else {
        Log(L"core: OpenEventW failed for ready event (%lu)", GetLastError());
    }
    g_readyEvent.clear();
    KillTimer(h, kReadyTimer);
}

bool HandleHostTimer(HWND h, WPARAM w)
{
    if (w == kReadyTimer)
        return TrySignalReadyEvent(h), true;
    if (w == kStatusTimer) {
        std::erase_if(g_app.drainingBars, [](const auto& bar) { return bar->WaitStopRendering(0); });
        if (!g_app.trayAdded)
            AddTrayIcon();
        if (g_app.guard && WaitForSingleObject(g_app.guard, 0) == WAIT_OBJECT_0) {
            Log(L"guard: it went away; started again");
            EnsureGuard(g_app.guard);
        }
        UpdateStatus();
        if (g_app.chrome)
            g_app.chrome->Enforce();
        return true;
    }
    if (w == kMessengerTimer) {
        KillTimer(h, kMessengerTimer);
        bool waiting = false;
        for (Messenger due : g_app.alerts.Due(GetTickCount64(), &waiting))
            PostMessengerCard(due);
        if (waiting)
            SetTimer(h, kMessengerTimer, 500, nullptr);
        return true;
    }
    if (w == kDisplaySettleTimer) {
        KillTimer(h, kDisplaySettleTimer);
        if (g_app.displaysReady) {
            Log(L"core: displays settled: renderers set up again");
            SyncDisplayBars(g_app.displaySettings, true);
            UpdateDesktopMonitor(true);
        }
        return true;
    }
    if (w == kTrayFocusTimer)
        return KillTimer(h, kTrayFocusTimer), RestoreHiddenOverflowFocus(g_app.trayPanel->Previous()), true;
    return false;
}

void HandleTaskbarCreated()
{
    g_app.trayAdded = false;
    AddTrayIcon();
    if (g_app.tray)
        g_app.tray->Refresh();
    if (g_app.chrome)
        g_app.chrome->Enforce();
    if (g_app.widgets)
        g_app.widgets->OnShellRestart();
}
void HandleCatalogMessage(bool listChanged)
{
    if (g_app.start)
        g_app.start->Refresh(listChanged);
    if (g_app.extras)
        g_app.extras->OnCatalogChanged(listChanged);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_app.tasks && g_app.tasks->HandleMessage(m, w, l))
        return 0;
    if (m == g_app.taskbarCreated && m != 0)
        return HandleTaskbarCreated(), 0;
    switch (m) {
    case WM_UPDATE_READY:
        if (g_app.updates && g_app.notes)
            g_app.updates->NotifyPrepared([](std::optional<Notice> card, bool silent) {
                if (card)
                    g_app.notes->Post(std::move(*card), silent);
                else
                    g_app.notes->ClearPreparedUpdate();
            });
        return 0;
    case WM_TRAY: HandleTrayCallback(w, l); return 0;
    case WM_OPEN_SETTINGS: OpenSettings(); return 0;
    case WM_IME_STATE:
        if (auto sample = TakePosted<ImeWatcher::Sample>(l))
            OnImeSample(*sample);
        return 0;
    case WM_WIN_KEY: HandleWinKeyMessage(w); return 0;
    case WM_NOTICE: HandleNoticeMessage(l); return 0;
    case WM_BADGES:
        if (g_app.displaySettings.barBadges && g_app.tasks && g_app.bar) {
            const std::vector<TaskItem> items = BarItems();
            ForBars([&](Bar& bar) { bar.SetItems(items); });
        }
        return 0;
    case WM_CATALOG: HandleCatalogMessage(w == 0); return 0;
    case WM_VOLUME: UpdateStatus(); return 0;
    case WM_STATUS: {
        auto status = TakePosted<SystemStatus>(l);
        status->brightness = g_app.brightness;
        g_app.lastStatus = *status;
        ForBars([&](Bar& bar) { bar.SetStatus(*status); });
        g_app.control->SetStatus(*status);
        g_app.start->SetStatus(*status);
        g_app.widgets->SetStatus(*status);
        return 0;
    }
    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            const auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(l);
            if (s && s->PowerSetting == kBrightnessSetting && s->DataLength == sizeof(DWORD)) {
                g_app.brightness = int(std::min<DWORD>(*reinterpret_cast<const DWORD*>(s->Data), 100));
                g_app.control->BrightnessChanged(g_app.brightness);
                UpdateStatus();
            }
        }
        return TRUE;
    case WM_DEVICECHANGE:
        if (HandleDeviceBroadcast(w, l))
            return TRUE;
        break;
    case WM_USB: HandleUsbMessage(); return 0;
    case WM_TRAY_ICONS: HandleTrayIconsMessage(h, w); return 0;
    case WM_TIMER:
        if (HandleHostTimer(h, w))
            return 0;
        break;
    case WM_ATRIUM_ADD_WIDGET:
        return w <= WPARAM(WidgetKind::Network) && l >= 0 && l <= LPARAM(WidgetSize::Large) && g_app.widgetsOn &&
                       g_app.widgets->Add(WidgetKind(w), WidgetSize(l)) ?
                   1 :
                   0;
    case WM_ATRIUM_SETTINGS: ApplySettings(g_app.server->Current()); return 0;
    case WM_ATRIUM_SERVER_READY: TrySignalReadyEvent(h); return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        if (!g_app.displaysReady)
            return 0;
        if (g_app.chrome)
            g_app.chrome->Enforce();
        SyncDisplayBars(g_app.displaySettings, true);
        SetTimer(h, kDisplaySettleTimer, 2000, nullptr);
        UpdateDesktopMonitor(true);
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETWORKAREA && g_app.displaysReady) {
            SyncDisplayBars(g_app.displaySettings, true, true);
            UpdateDesktopMonitor(false);
        }
        return 0;
    case WM_CLOSE:
        if (g_app.chrome)
            g_app.chrome->RestoreForExit();
        DestroyWindow(h);
        return 0;
    case WM_ENDSESSION:
        if (w) {
            Log(L"core: session ending");
            if (g_app.chrome)
                g_app.chrome->RestoreForExit();
        }
        return 0;
    case WM_DESTROY:
        if (g_app.brightnessNotify)
            UnregisterPowerSettingNotification(g_app.brightnessNotify);
        if (g_app.usbNotify)
            UnregisterDeviceNotification(g_app.usbNotify);
        RemoveTrayIcon(), PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

std::thread StartLogonUpdate(const Settings& settings)
{
    if (!ShouldSyncStartupOnLaunch(IsDevelopmentBuild(), IsRegisteredApplication(ExeDir()), settings.startOnLogin, StartupTurnedOff()))
        return {};
    return std::thread([] {
        if (!SetStartOnLogin(true))
            Log(L"startup: could not update the Windows logon entry");
    });
}

std::optional<int> HandleLaunchCommand(const atrium::CoreLaunchOptions& launch)
{
    if (launch.uninstall) {
        if (MessageBoxW(nullptr, Tr(L"Uninstall Atrium? Your personal settings and desktop widget layout will be kept."), L"Atrium",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
            return 0;
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        const std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        const std::wstring arguments = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ExeDir() + L"\\Uninstall.ps1\" -Confirmed";
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", powershell.c_str(), arguments.c_str(), nullptr, SW_HIDE)) > 32 ? 0 : 1;
    }
    if (launch.guard)
        return launch.guardPid ? RunGuard(launch.guardPid, kHostClass) : 1;
    if (!launch.exitRequested)
        return std::nullopt;
    HWND other = atrium::FindWindowForExecutable(kHostClass);
    if (!other)
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(other, WM_CLOSE, 0, 0);
    const DWORD wait = process ? WaitForSingleObject(process, 10000) : WAIT_FAILED;
    if (process)
        CloseHandle(process);
    WaitForGuard();
    return wait == WAIT_OBJECT_0 ? 0 : 1;
}

HANDLE AcquireInstanceMutex(int& exitCode)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!mutex) {
        exitCode = 1;
        return nullptr;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return mutex;
    HWND other = FindWindowW(kHostClass, nullptr);
    if (other && SameCoreExecutable(other)) {
        PostMessageW(other, WM_OPEN_SETTINGS, 0, 0);
        CloseHandle(mutex);
        exitCode = 0;
        return nullptr;
    }
    bool replaced = false;
    if (other && MessageBoxW(nullptr, Tr(L"Atrium is running from another location. Quit it and use the version in this folder?"), L"Atrium",
                             MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            PostMessageW(other, WM_CLOSE, 0, 0);
            replaced = WaitForSingleObject(process, 15000) == WAIT_OBJECT_0;
            CloseHandle(process);
        }
        WaitForGuard();
    }
    CloseHandle(mutex);
    if (!replaced) {
        exitCode = 0;
        return nullptr;
    }
    mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex)
            CloseHandle(mutex);
        exitCode = 0;
        return nullptr;
    }
    Log(L"core: took over from the copy at another location");
    return mutex;
}

void MakeExtras(HINSTANCE instance);

std::optional<RECT> GenieIconTarget(HWND window)
{
    std::optional<RECT> icon;
    ForBars([&](Bar& bar) {
        if (icon || !bar.Visible())
            return;
        for (const TaskItem& item : bar.Items())
            if (std::ranges::find(item.windows, window) != item.windows.end()) {
                if (bar.Seen()) {
                    icon = bar.EntryRect(item.key);
                } else {
                    const RECT r = bar.BarRect();
                    const LONG cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                    icon = RECT{cx - 1, cy - 1, cx + 1, cy + 1};
                }
                return;
            }
    });
    return icon;
}

void OnTasksChanged()
{
    const double t0 = ClockMs();
    WithdrawForegroundMessenger(g_app.alerts, g_app.notes.get());
    const std::vector<TaskItem> all = g_app.tasks->Snapshot(true);
    if (g_app.stage)
        g_app.stage->Update(all);
    if (g_app.trayPanel)
        g_app.trayPanel->SetAttention(all);
    const double t1 = ClockMs();
    const auto items = BarItems();
    const double t2 = ClockMs();
    ForBars([&](Bar& bar) { bar.SetItems(items); });
    if (g_app.preview)
        g_app.preview->Refresh(ActiveBar().Items());
    const double t3 = ClockMs();
    if (g_app.start)
        g_app.start->Refresh();
    const double t4 = ClockMs();
    if (t4 - t0 > 20)
        Log(L"tasks: changes took %.0f ms (stage %.0f, preview %.0f, bar %.0f, start %.0f)", t4 - t0, t1 - t0, t2 - t1, t3 - t2, t4 - t3);
}

void InitializeSubsystems(HINSTANCE instance, Settings& settings)
{
    g_app.bar = std::make_unique<Bar>(instance, g_app.capturable);
    g_app.activeBar = g_app.bar.get();
    Surface::SetGlassHost(g_app.bar.get());
    Surface::SetCapturable(g_app.capturable);
    g_app.menu = std::make_unique<GlassMenu>(instance);
    StartJumpListIndex(settings.windowsContextMenu);
    g_app.tasks = std::make_unique<TaskModel>(g_app.host, OnTasksChanged, [](const std::wstring& exe) { return g_app.tray->OpenExisting(exe); });
    g_app.tasks->SetOnFlash([](HWND window) { MessengerFlashed(ProgramOf(window)); });
    g_app.catalog = std::make_unique<AppCatalog>(g_app.host, WM_CATALOG);
    g_app.start = std::make_unique<StartMenu>(instance, *g_app.tasks, *g_app.catalog);
    MakeExtras(instance);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_app.catalog->PrefetchStartIcons(StartMenu::TileIconPx(PopupScale(mi.rcWork)));
    g_app.start->onSystem = OpenSystem, g_app.start->SetPowerNotify(g_app.host, WM_NOTICE);
    g_app.start->menu = g_app.menu.get();
    g_app.notes = std::make_unique<Notifications>(instance);
    g_app.tray = std::make_unique<TrayModel>(g_app.host, WM_TRAY_ICONS, [] {
        std::vector<PinnedApp> names;
        for (const auto& app : g_app.catalog->Apps())
            names.push_back(AppCatalog::PinFor(app));
        return names;
    });
    g_app.trayPanel = std::make_unique<TrayPanel>(instance, *g_app.tray);
    g_app.trayPanel->onSystemTray = OpenTray;
    g_app.trayPanel->SetAttention(g_app.tasks->Snapshot(true));
    g_app.control = std::make_unique<ControlCenter>(instance);
    g_app.control->onChanged = UpdateStatus;
    g_app.start->onStatusRequest = UpdateStatus;
    g_app.calendar = std::make_unique<CalendarPanel>(instance);
    g_app.widgets = std::make_unique<DesktopWidgets>(instance);
    g_app.widgets->ReloadLanguage();
    g_app.stage = std::make_unique<StageManager>(instance, *g_app.tasks);
    g_app.stage->onCoverSoon = [](HMONITOR monitor) {
        ForBars([&](Bar& bar) {
            if (bar.Monitor() == monitor)
                bar.ExpectCover(1200);
        });
    };
    g_app.genie = std::make_unique<GenieMinimize>(instance, GenieIconTarget, [](HWND window) { return g_app.stage && g_app.stage->OwnsMinimize(window); });
    TaskModel::SetWindowSteps([](HWND w, std::function<void()> step) { return g_app.genie && g_app.genie->Minimize(w, std::move(step)); },
                              [](HWND w, std::function<void()> step) { return g_app.genie && g_app.genie->Restore(w, std::move(step)); });
    g_app.control->onStage = SetStageManager;
    g_app.widgets->onStatusRequest = UpdateStatus;
    g_app.status = std::make_unique<SystemStatusReader>(g_app.host, WM_STATUS, WM_VOLUME);
    g_app.usb = std::make_unique<UsbVolumeMonitor>([host = g_app.host] { PostMessageW(host, WM_USB, 0, 0); });
    g_app.widgets->menu = g_app.menu.get();
    g_app.widgets->onAddWidgets = [] { OpenSettings(L"widgets"); };
    g_app.widgets->onCalendar = [] {
        if (!g_app.calendar->Visible())
            ToggleCalendar();
    };
    g_app.notes->bellRect = [] { return ActiveBar().EntryRect(L"#notify"); };
    g_app.notes->barRect = [] { return ActiveBar().BarRect(); };
    g_app.notes->onUnread = [](int n) { ForBars([&](Bar& bar) { bar.SetUnread(n); }); };
    g_app.notes->onActivate = [](const Notice& n) { return RunNoticeAction(NoticeEnv(), n); };
    g_app.notes->quietIsOurs = [] { return g_app.chrome && g_app.chrome->QuietIsOurs(); };
    g_app.notes->held = BarPopupOpen;
    g_app.preview = std::make_unique<WindowPreview>(instance);
    g_app.ime = std::make_unique<ImeBubble>(instance);
    ConfigureBar(*g_app.bar);
    g_app.displaysReady = true;
    ApplySettings(settings);
    g_app.bar->SetItems(BarItems());
    g_app.server = std::make_unique<SettingsServer>(g_app.host, settings);
    g_app.start->onSize = [](const std::wstring& name) { g_app.server->Modify([&](Settings& s) { s.startSize = name; }); };
    g_app.start->onIcon = [](const std::wstring& name) { g_app.server->Modify([&](Settings& s) { s.startIcon = name; }); };
    if (settings.startSize.empty())
        g_app.start->onSize(g_app.start->SizeName());
    if (settings.startIcon.empty())
        g_app.start->onIcon(g_app.start->IconSizeName());
    g_app.start->onPin = [](const PinnedApp& pin) {
        if (!g_app.server)
            return;
        g_app.server->Modify([&](Settings& s) {
            auto pins = s.pins.value_or(std::vector<PinnedApp>{});
            if (AddUniquePin(pins, pin))
                s.pins = std::move(pins);
        });
    };
}

void ShutdownApp(HANDLE mutex, std::thread& startupThread)
{
    g_app.displaysReady = false;
    if (g_app.chrome)
        g_app.chrome->RestoreForExit();
    g_app.stage->RestoreAll();
    g_app.stage->SignalSnapshotStop();
    g_app.winKey.reset(), g_app.imeWatcher.reset(), g_app.status.reset();
    g_app.preview->Close();
    std::vector<Bar*> stopBars;
    ForBars([&](Bar& bar) { stopBars.push_back(&bar); });
    for (const auto& bar : g_app.drainingBars)
        stopBars.push_back(bar.get());
    const bool renderingStopped =
        StopWithinDeadline(stopBars, 3000, [](Bar* bar) { bar->SignalStopRendering(); }, [](Bar* bar, DWORD ms) { return bar->WaitStopRendering(ms); });
    if (!renderingStopped) {
        g_app.chrome.reset();
        Log(L"core: exited (render thread stuck)");
        ReleaseMutex(mutex);
        ShutdownLog();
        TerminateProcess(GetCurrentProcess(), 0);
    }
    g_app.server.reset(), g_app.updates.reset(), g_app.listener.reset();
    g_app.wechat.reset(), g_app.feishu.reset(), g_app.usb.reset();
    g_app.notes.reset(), g_app.widgets.reset(), g_app.calendar.reset(), g_app.control.reset();
    g_app.trayPanel.reset(), g_app.tray.reset(), g_app.start.reset();
    g_app.menu.reset(), g_app.preview.reset();
    TaskModel::SetWindowSteps({}, {});
    g_app.genie.reset();
    if (g_app.stage && !g_app.stage->WaitSnapshotStop(400)) {
        g_app.chrome.reset();
        Log(L"core: exited (snapshot worker stuck)");
        ReleaseMutex(mutex);
        ShutdownLog();
        TerminateProcess(GetCurrentProcess(), 0);
    }
    g_app.stage.reset(), g_app.ime.reset();
    Surface::SetGlassHost(nullptr);
    g_app.extras.reset(), g_app.catalog.reset();
    g_app.tasks.reset();
    g_app.drainingBars.clear(), g_app.secondaryBars.clear(), g_app.bar.reset();
    g_app.chrome.reset();
    if (startupThread.joinable())
        startupThread.join();
    Log(L"core: exited");
    ShutdownLog();
    ShutdownCrashHandlers();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

// Launchpad, the now-playing entry and the folder stack: theirs the bars' extra entries and popups.
void MakeExtras(HINSTANCE instance)
{
    g_app.extras = std::make_unique<ExtrasHost>(instance, *g_app.catalog, [](const std::function<void(Bar&)>& each) { ForBars(each); });
    g_app.extras->onPanelsChanged = [] { g_app.notes->PanelsChanged(); };
    g_app.extras->onOpenSettings = [](const wchar_t* page) { OpenSettings(page); };
    g_app.extras->SetAppMenu(ShowCatalogMenu);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetLanguage(L"auto");
    if (PWSTR profile = nullptr; SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
        SetCurrentDirectoryW(profile);
        CoTaskMemFree(profile);
    }
    int argumentCount = 0;
    auto* startupArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::vector<std::wstring> args(startupArguments, startupArguments + (startupArguments ? argumentCount : 0));
    LocalFree(static_cast<HLOCAL>(startupArguments));
    const atrium::CoreLaunchOptions launch = atrium::ParseCoreLaunchOptions(args);
    g_readyEvent = launch.readyEvent;
    if (const std::optional<int> code = HandleLaunchCommand(launch))
        return *code;
    int instanceExit = 0;
    HANDLE mutex = AcquireInstanceMutex(instanceExit);
    if (!mutex)
        return instanceExit;

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    SetCurrentProcessExplicitAppUserModelID(kAppId);
    g_app.instance = instance;
    Log(L"core: starting");
    if (LowIntegrity())
        Log(L"core: running at low integrity (Atrium.exe has a Low label?): settings cannot be saved, the Windows key is left alone");
    RegisterApplication(ExeDir());
    InstallCrashHandlers(&SystemChrome::EmergencyRestore);
    g_app.chrome = std::make_unique<SystemChrome>();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = kHostClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ATRIUM));
    RegisterClassExW(&wc);
    g_app.host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Atrium", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g_app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_app.host, g_app.taskbarCreated, MSGFLT_ALLOW, nullptr);

    Settings settings = LoadSettings();
    SetLanguage(settings.language);
    std::thread startupThread = StartLogonUpdate(settings);
    if (!settings.pins) {
        settings.pins = TaskModel::ImportWindowsPins();
        Log(L"core: imported %zu Windows taskbar pins", settings.pins->size());
    }
    g_app.capturable = launch.debugCapturable;
    SetRemoteVisible(settings.remoteVisible);
    if (settings.remoteVisible)
        Log(L"core: remote-control mode on: Atrium shows in every capture, the glass shows the wallpaper");

    InitializeSubsystems(instance, settings);
    AddTrayIcon();
    UpdateStatus();
    g_app.brightnessNotify = RegisterPowerSettingNotification(g_app.host, &kBrightnessSetting, DEVICE_NOTIFY_WINDOW_HANDLE);
    DEV_BROADCAST_DEVICEINTERFACE_W volumeInterface{sizeof(DEV_BROADCAST_DEVICEINTERFACE_W)};
    volumeInterface.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    volumeInterface.dbcc_classguid = kVolumeInterface;
    g_app.usbNotify = RegisterDeviceNotificationW(g_app.host, &volumeInterface, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!g_app.usbNotify)
        Log(L"usb: could not register for volume notifications (%lu)", GetLastError());
    SetTimer(g_app.host, kStatusTimer, kStatusMs, nullptr);
    g_app.tray->Refresh();
    g_app.updates = std::make_unique<UpdateChecker>(g_app.host, WM_UPDATE_READY);
    if (!g_readyEvent.empty()) {
        SetTimer(g_app.host, kReadyTimer, 50, nullptr);
        PostMessageW(g_app.host, WM_ATRIUM_SERVER_READY, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ShutdownApp(mutex, startupThread);
    return 0;
}
