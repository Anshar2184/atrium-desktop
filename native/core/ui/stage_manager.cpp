#include "ui/stage_manager.h"

#include "clock.h"
#include "log.h"
#include "settings.h"
#include "shell/displays.h"
#include "shell/games.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <functional>
#include <climits>
#include <cmath>
#include <ranges>
#include <thread>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

// DIP. Each set of windows stands in a box kBoxW x kBoxH, kEdge from the screen's edge, the boxes one
// above the other in the middle of what the bar leaves of the screen's height. The windows turn
// kTilt degrees about their upright axis, the side towards the screen's middle away from the eye
// (a viewer kDepth in front of the screen); a pointer over them turns them most of the way back.
// The eye is level with a point kKeystone of the way from a box to the strip's middle: the boxes
// above it lean down towards the middle of the screen, those below lean up, the more so the
// further they are.
constexpr float kSlotGap = kStageSlotGap, kVPad = 24, kStripW = kStageStripW;
constexpr float kSlot = kStageBoxH + kSlotGap;
constexpr size_t kStack = 3; // windows shown per set
constexpr float kPictureScale = 1.25f; // pictures are taken this much larger than the box (the pointer enlarges them)
constexpr float kPictureRadius = 8;
constexpr float kBarRoom = 90; // the bar floats over the bottom of the work area
constexpr int kMaxEntries = 6;
constexpr UINT_PTR kWatchTimer = 0x5C01, kDownTimer = 0x5C02;
constexpr UINT kWatchMs = 30, kDownMs = 700;
constexpr ULONGLONG kDesktopMs = 250; // the desktop in front this long: it was clicked
constexpr UINT WM_STAGE_PICTURE = WM_APP + 0x5C; // a window's picture was taken
// The pointer at the screen's edge for kEdgeMs brings the strip out; away from it for kPeekLeaveMs
// sends it back. Whether a window covers the strip's place is looked at every kCoverMs.
constexpr ULONGLONG kEdgeMs = 40, kPeekLeaveMs = 350, kCoverMs = 150;
constexpr double kRevealInMs = 380, kRevealOutMs = 210, kRevealStagger = 32;
constexpr int kLargeMax = 4096;
// A window's picture taken this recently (the pointer went to the strip) is used as it is: the window
// goes at once.
constexpr ULONGLONG kFreshMs = 4000;

std::wstring StatePath()
{
    std::wstring path = SettingsPath();
    return path.substr(0, path.find_last_of(L'\\')) + L"\\stage.json";
}

bool OnMonitor(HWND h, HMONITOR m) { return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == m; }

} // namespace

// Not a window to stage: a layer over everything (a screenshot's selection, an overlay), topmost
// without a title bar, or one covering the whole screen. It neither takes the stage, nor is put
// away, nor is moved beside the strip; covering the screen, it sends the strip away instead.
bool StageManager::Transient(HWND h)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) && (style & WS_CAPTION) != WS_CAPTION)
        return true;
    MONITORINFO mi{sizeof(mi)};
    RECT r{};
    return !IsZoomed(h) && GetWindowRect(h, &r) && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) && r.left <= mi.rcMonitor.left &&
           r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

namespace {

bool Has(const std::vector<std::wstring>& set, const std::wstring& key) { return std::ranges::find(set, key) != set.end(); }

// Each window waits on a card of its own: keyed by its app's key and itself ('|' is in neither an
// app's path nor its AUMID).
std::wstring WindowKey(const std::wstring& app, HWND h)
{
    wchar_t hex[24];
    swprintf_s(hex, L"|%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    return app + hex;
}

std::wstring AppOf(const std::wstring& key)
{
    const size_t bar = key.rfind(L'|');
    return bar == std::wstring::npos ? key : key.substr(0, bar);
}

bool AppsDark()
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                 &light, &size);
    return light == 0;
}

float EaseOut3(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

constexpr motion::Spring kGlide{0.42f, 0.86f};
constexpr motion::Spring kFadeSpring{0.30f, 1.f};

StageManager* g_stage = nullptr; // the one Stage Manager, for its WinEvent hooks

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND h, LONG object, LONG, DWORD, DWORD)
{
    if (object != OBJID_WINDOW || !h || !g_stage)
        return;
    if (event == EVENT_SYSTEM_MINIMIZESTART)
        g_stage->Minimized(h);
    else
        g_stage->MoveSize(h, event == EVENT_SYSTEM_MOVESIZESTART);
}

} // namespace

void StageManager::SetTransitions(HWND h, bool on)
{
    BOOL off = on ? FALSE : TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}

std::wstring StageManager::AppKeyOf(const std::wstring& key) { return AppOf(key); }
bool StageManager::HasKey(const Set& set, const std::wstring& key) { return Has(set, key); }

StageManager::StageManager(HINSTANCE instance, TaskModel& tasks)
    : tasks_(tasks),
      surface_(instance, L"Atrium Stage Manager", false),
      flight_(instance, L"Atrium Stage Flight", false, 0, false, true),
      snapshots_(surface_.Hwnd(), WM_STAGE_PICTURE),
      actionWorker_([this] { ActionWorker(); })
{
    surface_.SetGlassless(true);
    flight_.SetGlassless(true);
    flight_.paint = [this](Canvas& c) { PaintFlights(c); };
    flight_.message = [this](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m != WM_TIMER || w != kFlightTimer)
            return false;
        KillTimer(flight_.Hwnd(), kFlightTimer);
        if (flights_.empty())
            flight_.Hide(false); // idle for a while: its buffers go
        return true;
    };
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(desktops_.put()))))
        desktops_ = nullptr;
    Load(); // windows a previous run put away (it may not have ended well)
}

StageManager::~StageManager()
{
    if (g_stage == this)
        g_stage = nullptr;
    if (minimizeHook_)
        UnhookWinEvent(minimizeHook_);
    if (moveHook_)
        UnhookWinEvent(moveHook_);
    std::unique_lock lk(actionLock_);
    actionStop_ = true;
    lk.unlock();
    actionCv_.notify_all();
    if (actionWorker_.joinable())
        actionWorker_.join();
}

// ---- the stage ---------------------------------------------------------------------------------

void StageManager::SetExcluded(std::vector<std::wstring> apps)
{
    const bool changed = apps != excluded_;
    if (changed)
        excluded_ = std::move(apps);
    // Anything an excluded app already put away comes back and leaves the stage model. The sweep
    // also runs when the list did not change, since saved state may hold entries from before.
    bool touched = false;
    Set release;
    for (const auto& [h, p] : parked_)
        if (StageAppExcluded(excluded_, p.key) && !Has(release, p.key))
            release.push_back(p.key);
    if (!release.empty()) {
        Unpark(release, nullptr, false, nullptr, false);
        touched = true;
    }
    touched |= std::erase_if(stage_, [&](const std::wstring& key) { return StageAppExcluded(excluded_, key); }) > 0;
    for (Set& set : recent_)
        touched |= std::erase_if(set, [&](const std::wstring& key) { return StageAppExcluded(excluded_, key); }) > 0;
    touched |= std::erase_if(recent_, [](const Set& set) { return set.empty(); }) > 0;
    touched |= std::erase_if(pending_, [&](const std::wstring& key) { return StageAppExcluded(excluded_, key); }) > 0;
    // A window that was on the stage gets its own minimize animation back.
    if (!up_.empty()) {
        const std::vector<TaskItem> snapshot = tasks_.Snapshot(true);
        for (auto it = up_.begin(); it != up_.end();) {
            bool excluded = false;
            for (const TaskItem& t : snapshot) {
                if (!StageAppExcluded(excluded_, t.key))
                    continue;
                if (std::ranges::find(t.windows, it->first) != t.windows.end()) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) {
                SetTransitions(it->first, true);
                it = up_.erase(it);
                touched = true;
            } else {
                ++it;
            }
        }
    }
    if (!changed && !touched)
        return;
    openIntent_.Cancel();
    Save();
    Update(tasks_.Snapshot(true));
    Rebuild();
}

void StageManager::SetEnabled(bool on)
{
    if (on == enabled_ && applied_)
        return;
    applied_ = true;
    enabled_ = on;
    openIntent_.Cancel();
    pressed_ = -1;
    SyncMoveHook();
    if (on) {
        stage_.clear();
        pending_.clear();
        SetTimer(surface_.Hwnd(), kWatchTimer, kWatchMs, nullptr);
        Log(L"stage: on (%s, %s), %zu window(s) waiting from before", onRight_ ? L"right" : L"left", autoHide_ ? L"auto-hidden" : L"always shown",
            parked_.size());
        Update(tasks_.Snapshot(true));
    } else {
        KillTimer(surface_.Hwnd(), kWatchTimer);
        size_t n = parked_.size();
        HWND front = GetForegroundWindow();
        RestoreAll();
        // The windows coming back must not cover the one in front.
        if (n && front) {
            raise_ = front;
            SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
        }
        Log(L"stage: off, %zu window(s) back", n);
    }
}

void StageManager::SetOnRight(bool onRight)
{
    if (onRight == onRight_)
        return;
    onRight_ = onRight;
    // It comes out again on the other side.
    shown_ = peek_ = false;
    surface_.Hide(false);
    Rebuild();
}

void StageManager::SetBarEdge(BarEdge edge)
{
    if (edge == barEdge_)
        return;
    barEdge_ = edge;
    coveredAt_ = 0;
    Rebuild();
}

void StageManager::SetAutoHide(bool autoHide)
{
    if (autoHide == autoHide_)
        return;
    autoHide_ = autoHide;
    peek_ = false;
    Log(L"stage: %s", autoHide ? L"auto-hidden" : L"always shown");
    if (enabled_)
        Watch();
}

void StageManager::SetScrollWrap(bool on)
{
    if (on == scrollWrap_)
        return;
    scrollWrap_ = on;
    scroll_.SetWrap(on);
    scroll_.Fit(scrollCount_, size_t(Capacity()));
    Rebuild(false);
}

void StageManager::SetMinimizeToStrip(bool on)
{
    if (on == minimizeToStrip_)
        return;
    minimizeToStrip_ = on;
    ReleaseUp();
    // Out of context, on this thread: the picture leaves the window's place as it goes down.
    if (on && !minimizeHook_) {
        g_stage = this;
        minimizeHook_ =
            SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else if (!on && minimizeHook_) {
        UnhookWinEvent(minimizeHook_);
        minimizeHook_ = nullptr;
    }
    Log(L"stage: minimized windows %s", on ? L"go to the strip" : L"stay minimized");
}

void StageManager::SyncMoveHook()
{
    const bool want = enabled_ && dragMerge_;
    if (want && !moveHook_) {
        g_stage = this;
        moveHook_ =
            SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else if (!want && moveHook_) {
        UnhookWinEvent(moveHook_);
        moveHook_ = nullptr;
    }
    if (!want && (dragWindow_ || dragHover_ >= 0))
        CancelDrag();
}

void StageManager::SetMonitor(HMONITOR monitor, bool refresh)
{
    const bool changed = monitor != monitor_;
    if (!changed && !refresh)
        return;
    // Release windows on the old display before managing another one.
    if (changed && monitor_)
        RestoreAll();
    monitor_ = monitor;
    coveredAt_ = 0;
    if (changed && enabled_)
        Update(tasks_.Snapshot(true));
    Rebuild();
    if (changed && enabled_)
        Watch();
}

void StageManager::ProcessOpenIntent()
{
    Set opened, openedApps;
    for (const TaskItem& t : items_) {
        if (!openIntent_.Matches(AppOf(t.key), GetTickCount64()))
            continue;
        for (HWND h : t.windows) {
            BOOL here = TRUE;
            if (IsWindow(h) && IsWindowVisible(h) && OnMonitor(h, monitor_) && !Transient(h) &&
                (!desktops_ || FAILED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) || here)) {
                opened.push_back(t.key);
                if (!Has(openedApps, AppOf(t.key)))
                    openedApps.push_back(AppOf(t.key));
            }
        }
    }
    if (opened.empty())
        return;
    // Only these launches are done: another app added meanwhile still waits for its window.
    for (const std::wstring& app : openedApps)
        openIntent_.Done(app);
    pending_ = opened;
    pendingUntil_ = GetTickCount64() + kPendingMs;
    Stage(opened, true, nullptr, false);
}

void StageManager::Update(const std::vector<TaskItem>& items)
{
    if (!enabled_)
        return;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    // Window by window: each its own item, as each has its own card. Excluded apps never
    // enter the stage model at all.
    items_.clear();
    excludedWindows_.clear();
    size_t totalWindows = 0;
    for (const TaskItem& t : items)
        totalWindows += t.windows.size();
    items_.reserve(totalWindows);
    for (const TaskItem& t : items) {
        // Games too: their windows stay on the screen, whatever opens.
        if (StageAppExcluded(excluded_, t.key) || std::ranges::any_of(t.windows, IsGameWindow)) {
            // Never put away, but still a window the strip keeps out of the way of.
            excludedWindows_.insert(excludedWindows_.end(), t.windows.begin(), t.windows.end());
            continue;
        }
        const bool rootHere = std::ranges::find(t.windows, root) != t.windows.end();
        const TaskItem base{L"", t.name, t.pinned, t.pinIndex, {}, false, t.attention, t.badge, t.iconVersion, false};
        for (HWND h : t.windows) {
            TaskItem w = base;
            w.key = WindowKey(t.key, h);
            w.windows.push_back(h);
            // In front: the window itself, or the app's most recent one when what is in front is not one of them.
            w.active = t.active && (rootHere ? h == root : h == t.windows.front());
            items_.push_back(std::move(w));
        }
    }
    coveredAt_ = 0; // windows moved: look again
    Prune();
    ProcessOpenIntent();
    AdoptMinimized();
    if (!pending_.empty() && GetTickCount64() > pendingUntil_)
        pending_.clear();
    auto active = std::ranges::find_if(items_, [](const TaskItem& t) { return t.active; });
    // A window in front, on this screen, of an app not on the stage: its set takes the stage. Not
    // while a set picked in the strip is on its way to the front.
    if (active != items_.end() && root && root != dragWindow_ && !IsIconic(root) && OnMonitor(root, monitor_) && !Transient(root)) {
        if (pending_.empty() || Has(pending_, active->key)) {
            pending_.clear();
            if (!Has(stage_, active->key)) {
                // Another window of an app on the stage (a chat opened from the main window) joins
                // it; the app's other windows stay up.
                const std::wstring app = AppOf(active->key);
                const bool sameApp = std::ranges::any_of(stage_, [&](const std::wstring& k) { return AppOf(k) == app; });
                Stage(sameApp ? Set{active->key} : SetOf(active->key), sameApp, root, false);
                MoveBeside(root); // a window taking the stage over the strip moves beside it
            }
        }
    }
    AdoptMinimized(); // the stage's windows as they are now, ready to be minimized
    Rebuild();
}

StageManager::Set StageManager::SetOf(const std::wstring& key) const
{
    for (const Set& set : recent_)
        if (Has(set, key))
            return set;
    return Set{key};
}

// `keys` take the stage (or join it: add). Their windows that were put away come back, `front` in
// front; then every other app's windows on this screen go to the strip: the apps that were on the
// stage at the top, each on a card of its own (only a drag puts windows together in one set).
void StageManager::Stage(const Set& keys, bool add, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly)
{
    Unpark(keys, front, activate, fly);
    if (!add) {
        openIntent_.Cancel(); // a different stage supersedes an app launch still waiting for a window
        std::vector<Set> left, others; // the stage's windows that were up; the rest
        for (const TaskItem& t : items_) {
            if (Has(keys, t.key))
                continue;
            bool any = false;
            // Least recent first: the most recent window gets the latest number and shows in front.
            // A window filling the screen (a game, a video) is left alone: minimized, a full-screen
            // game comes back to its screen mode first, and flashes up before it goes.
            for (auto h = t.windows.rbegin(); h != t.windows.rend(); ++h)
                if (ShouldParkStageWindow(parked_.contains(*h), IsWindowVisible(*h) != FALSE, IsIconic(*h) != FALSE, OnMonitor(*h, monitor_), Transient(*h),
                                          FillsScreen(*h), IsWindowEnabled(*h) != FALSE)) {
                    Park(*h, t.key);
                    any = true;
                }
            if (!any)
                continue;
            (Has(stage_, t.key) ? left : others).push_back(Set{t.key});
        }
        // The most recently used first: the windows nearest the top of the screen's stack.
        std::unordered_map<HWND, int> depth;
        int n = 0;
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
            depth[h] = n++;
        auto nearest = [&](const Set& set) {
            int best = INT_MAX;
            for (const auto& [h, p] : parked_)
                if (Has(set, p.key))
                    if (auto d = depth.find(h); d != depth.end())
                        best = std::min(best, d->second);
            return best;
        };
        std::ranges::stable_sort(left, [&](const Set& a, const Set& b) { return nearest(a) < nearest(b); });
        std::ranges::stable_sort(others, [&](const Set& a, const Set& b) { return nearest(a) < nearest(b); });
        others.insert(others.begin(), left.begin(), left.end());
        // An app is in one set only: the newest.
        for (const Set& set : others)
            for (const std::wstring& k : set)
                for (Set& old : recent_)
                    std::erase(old, k);
        std::erase_if(recent_, [](const Set& set) { return set.empty(); });
        recent_.insert(recent_.begin(), others.begin(), others.end());
        stage_.clear();
        if (!keys.empty() || !others.empty())
            Log(L"stage: %s on the stage, %zu set(s) to the strip", keys.empty() ? L"the desktop" : keys.front().c_str(), others.size());
    } else {
        Log(L"stage: %s joins the stage", keys.front().c_str());
    }
    for (const std::wstring& k : keys) {
        if (!Has(stage_, k))
            stage_.push_back(k);
        for (Set& old : recent_)
            std::erase(old, k);
    }
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    Save();
}

// The window's picture is taken first (a minimized window has none); it goes down once it has, or
// after kDownMs whatever happened.
void StageManager::Park(HWND window, const std::wstring& key)
{
    restoring_.erase(window);
    Parked p;
    p.key = key;
    p.seq = ++seq_;
    p.since = GetTickCount64();
    p.down = false;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &p.frame, sizeof(p.frame))))
        GetWindowRect(window, &p.frame);
    parked_[window] = p;
    SetTransitions(window, false); // it goes at once: its picture flies instead
    if (auto fresh = pictures_.find(window);
        fresh != pictures_.end() && fresh->second.picture && fresh->second.picture->ok && GetTickCount64() - fresh->second.taken < kFreshMs) {
        PutDown(window, true);
        return;
    }
    RequestPicture(window, true);
    SetTimer(surface_.Hwnd(), kDownTimer, kDownMs, nullptr);
}

// The flight layer over the whole screen: kept up between flights (its first frame is slow), and
// letting clicks through while nothing flies.
void StageManager::Launch()
{
    const MONITORINFO mi = MonitorInfo();
    KillTimer(flight_.Hwnd(), kFlightTimer);
    SetWindowRgn(flight_.Hwnd(), nullptr, FALSE);
    if (!flight_.Visible())
        flight_.Show(mi.rcMonitor, false, POINT{mi.rcMonitor.left, mi.rcMonitor.top}, false);
    else
        flight_.Move(mi.rcMonitor);
    flight_.Invalidate();
}

// The stage's windows are pictured while the pointer is on the strip: picked there, they go at once.
void StageManager::Foresee()
{
    const ULONGLONG now = GetTickCount64();
    for (const TaskItem& t : items_) {
        if (!Has(stage_, t.key))
            continue;
        for (HWND h : t.windows) {
            if (parked_.contains(h) || !IsWindowVisible(h) || IsIconic(h) || !OnMonitor(h, monitor_))
                continue;
            if (auto p = pictures_.find(h); p != pictures_.end()) {
                const bool hasPicture = p->second.picture && p->second.picture->ok;
                const ULONGLONG minAge = hasPicture ? kFreshMs / 2 : StagePictureRetryMs(p->second.misses);
                if (now - p->second.taken < minAge)
                    continue;
            }
            pictures_[h].taken = now; // asked for: not again meanwhile
            RequestPicture(h);
        }
    }
}

void StageManager::RequestPicture(HWND window, bool urgent)
{
    const float s = PopupScale(WorkArea());
    snapshots_.Request(window, int(kBoxW * kPictureScale * s), int(kBoxH * kPictureScale * s), kPictureRadius * kPictureScale * s, kLargeMax, urgent);
}

void StageManager::SetLarge(Flight& f)
{
    auto it = pictures_.find(f.window);
    if (it == pictures_.end() || !it->second.large || !it->second.large->ok || !f.key.starts_with(L"stage|w|"))
        return;
    f.large = it->second.large;
    f.largeKey = f.key + L"|L";
}

void StageManager::PutDown(HWND window, bool fly)
{
    auto it = parked_.find(window);
    if (it == parked_.end() || it->second.down)
        return;
    it->second.down = true;
    it->second.since = GetTickCount64();
    std::wstring key;
    auto picture = fly ? PictureOf(window, it->second.key, key) : nullptr;
    if (!picture || !picture->ok || !key.starts_with(L"stage|w|")) {
        SetTransitions(window, true); // no picture to fly: Windows' own animation
        ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
        return;
    }
    ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
    std::erase_if(flights_, [&](const Flight& f) { return f.window == window; });
    Flight f;
    f.window = window;
    f.picture = picture;
    f.key = key;
    f.toStrip = true;
    f.place = PlacePose(it->second.frame);
    SetLarge(f);
    flights_.push_back(std::move(f));
    Launch();
    surface_.Invalidate();
}

void StageManager::Pick(const Set& keys, bool add, HWND front)
{
    pending_ = keys;
    pendingUntil_ = GetTickCount64() + kPendingMs;
    hover_ = pressed_ = -1;
    // Where the card's pictures are now: they fly from there.
    std::unordered_map<HWND, Pose> from;
    for (const Entry& e : entries_)
        if (e.keys == keys)
            for (HWND h : e.windows) {
                Pose pose;
                if (StripPose(h, pose))
                    from[h] = pose;
            }
    pendingUntil_ += ULONGLONG(kFlightMs);
    Stage(keys, add, front, true, &from);
    Rebuild();
}

void StageManager::OpenInCurrentGroup(const std::wstring& app, const std::function<void()>& launch)
{
    if (!enabled_ || app.empty() || StageAppExcluded(excluded_, app))
        return; // excluded apps are left where they are (their own open command still works)
    Update(tasks_.Snapshot(true)); // the menu's snapshot may already be out of date
    Set keys;
    HWND front = nullptr;
    for (const TaskItem& t : items_)
        if (AppOf(t.key) == app)
            for (HWND h : t.windows) {
                BOOL here = TRUE;
                if (!IsWindow(h) || !IsWindowVisible(h) || !OnMonitor(h, monitor_) || Transient(h) ||
                    (desktops_ && SUCCEEDED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) && !here))
                    continue;
                keys.push_back(t.key);
                if (!front)
                    front = h;
            }
    if (!keys.empty())
        Pick(keys, true, front);
    else {
        openIntent_.Arm(app, GetTickCount64());
        launch ? launch() : tasks_.Launch(app);
    }
}

// Minimized on the stage (setting stageMinimize): a window on the stage that is down now (it was up
// at the last look) leaves the stage and waits at the top of the strip, put away as if Atrium had
// (a pick there brings it back, where it was).
void StageManager::Minimized(HWND window)
{
    if (minimizeToStrip_ && enabled_ && (up_.contains(window) || InStage(window)) && AdoptMinimized())
        Rebuild();
}

// The stage's windows get their own minimize animation back (the option off, Stage Manager off, exit):
// also those just back from the strip and not looked at yet.
void StageManager::ReleaseUp()
{
    for (const auto& [h, frame] : up_)
        if (IsWindow(h) && !parked_.contains(h) && !Flying(h))
            SetTransitions(h, true);
    for (const TaskItem& t : items_)
        for (HWND h : t.windows)
            if (!up_.contains(h) && !parked_.contains(h) && !Flying(h) && IsWindow(h))
                SetTransitions(h, true);
    up_.clear();
    restoring_.clear();
}

void StageManager::SyncUpTransitions(const std::unordered_map<HWND, RECT>& up)
{
    for (const auto& [h, frame] : up)
        if ((!up_.contains(h) || Flying(h)) && HasPicture(h))
            SetTransitions(h, false);
    for (const auto& [h, frame] : up_)
        if (!up.contains(h) && !parked_.contains(h) && IsWindow(h))
            SetTransitions(h, true);
}

void StageManager::FlyAdopted(const std::vector<HWND>& flyIn)
{
    for (HWND h : flyIn) {
        std::wstring key;
        auto picture = PictureOf(h, parked_[h].key, key);
        if (!picture || !picture->ok || !key.starts_with(L"stage|w|"))
            continue; // no picture yet: the card just comes
        std::erase_if(flights_, [&](const Flight& f) { return f.window == h; });
        Flight f;
        f.window = h;
        f.picture = picture;
        f.key = key;
        f.toStrip = true;
        f.place = PlacePose(parked_[h].frame);
        SetLarge(f);
        flights_.push_back(std::move(f));
    }
}

bool StageManager::AdoptMinimized()
{
    if (!minimizeToStrip_ || !enabled_) {
        for (const auto& [h, frame] : up_)
            if (IsWindow(h) && !parked_.contains(h))
                SetTransitions(h, true);
        up_.clear();
        restoring_.clear();
        return false;
    }
    std::unordered_map<HWND, RECT> up;
    Set gone;
    std::vector<HWND> flyIn;
    const ULONGLONG now = GetTickCount64();
    PruneRestoring(now);
    for (const TaskItem& t : items_) {
        if (!Has(stage_, t.key))
            continue;
        bool anyUp = false;
        std::vector<HWND> down;
        for (HWND h : t.windows) {
            const bool iconic = IsIconic(h);
            const auto action = DecideStageAdopt(parked_.contains(h), IsWindowVisible(h) != FALSE, iconic, !iconic && OnMonitor(h, monitor_), up_.contains(h),
                                                 IsRestoring(h, now));
            if (action == StageAdoptAction::Up) {
                anyUp = true;
                RECT r{};
                if (auto seen = up_.find(h); seen != up_.end())
                    r = seen->second;
                if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
                    GetWindowRect(h, &r);
                up[h] = r;
            } else if (action == StageAdoptAction::Adopt) {
                down.push_back(h);
            }
        }
        if (anyUp || down.empty())
            continue;
        for (HWND h : down) {
            Parked p{t.key, ++seq_, now, true, {}};
            if (auto seen = up_.find(h); seen != up_.end())
                p.frame = seen->second;
            else if (WINDOWPLACEMENT wp{sizeof(wp)}; GetWindowPlacement(h, &wp))
                p.frame = wp.rcNormalPosition;
            parked_[h] = p;
            SetTransitions(h, !HasPicture(h)); // picked in the strip, its picture flies back: no zoom of its own
            flyIn.push_back(h);
        }
        gone.push_back(t.key);
    }
    SyncUpTransitions(up);
    up_ = std::move(up);
    if (gone.empty())
        return false;
    for (const std::wstring& k : gone) {
        std::erase(stage_, k);
        for (Set& old : recent_)
            std::erase(old, k);
    }
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    for (const std::wstring& k : gone)
        recent_.insert(recent_.begin(), Set{k}); // each on a card of its own
    FlyAdopted(flyIn);
    const bool hasPicture = std::ranges::any_of(flyIn, [&](HWND h) { return Flying(h); });
    if (hasPicture)
        Launch();
    Log(L"stage: %s minimized to the strip (%s)", gone.front().c_str(), hasPicture ? L"picture" : L"placeholder");
    Save();
    return true;
}

// The window in front, if on the stage: its picture kept fresh (every few seconds, and as the pointer
// nears its title bar, where it is minimized), since a minimized window has none to take.
void StageManager::KeepPicture()
{
    if (!minimizeToStrip_)
        return;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    auto seen = root ? up_.find(root) : up_.end();
    const ULONGLONG now = GetTickCount64();
    if (root != front_) {
        front_ = root;
        frontSince_ = now;
    }
    if (seen == up_.end() || IsIconic(root))
        return;
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(root, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(root, &r))
        return;
    seen->second = r; // where it comes back to
    POINT p{};
    GetCursorPos(&p);
    const float s = PopupScale(WorkArea());
    // Near the caption buttons (where a minimize comes from) the picture is kept fresh; elsewhere
    // seldom: PrintWindow makes the app draw itself whole, costly for a browser (#50).
    const bool titleBar = PtInRect(&r, p) && p.y < r.top + LONG(56 * s) && p.x > r.right - LONG(180 * s);
    const auto it = pictures_.find(root);
    const bool hasPicture = it != pictures_.end() && it->second.picture && it->second.picture->ok;
    const int misses = it == pictures_.end() ? 0 : it->second.misses;
    const ULONGLONG age = it == pictures_.end() || it->second.taken == 0 ? ULLONG_MAX : now - it->second.taken;
    if (!ShouldTakeStagePicture(hasPicture, titleBar, misses, now - frontSince_, age))
        return;
    pictures_[root].taken = now; // asked for: not again meanwhile
    RequestPicture(root);
}

// Windows that went (closed, hidden to the tray) or came back some other way are no longer waiting.
void StageManager::Prune()
{
    const ULONGLONG now = GetTickCount64();
    std::erase_if(parked_, [&](const auto& kv) {
        HWND h = kv.first;
        if (!IsWindow(h))
            return true;
        if (!IsWindowVisible(h)) {
            SetTransitions(h, true);
            return true;
        }
        // Minimizing is asynchronous (and waits for the picture): a window just put away may not be down yet.
        if (kv.second.down && !IsIconic(h) && now - kv.second.since > kSettleMs) {
            SetTransitions(h, true); // came back some other way
            return true;
        }
        return false;
    });
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) { return std::ranges::none_of(parked_, [&](const auto& kv) { return kv.second.key == k; }); });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    // Pictures of windows no longer waiting are old by the time they wait again; but not those of the
    // stage's (on their way back from the strip, or up): minimized, they go back with them.
    std::unordered_set<HWND> staged;
    if (minimizeToStrip_)
        for (const TaskItem& t : items_)
            if (Has(stage_, t.key))
                staged.insert(t.windows.begin(), t.windows.end());
    std::erase_if(pictures_, [&](const auto& kv) {
        if (parked_.contains(kv.first) || (IsWindow(kv.first) && now - kv.second.taken < kFreshMs) || up_.contains(kv.first) || Flying(kv.first) ||
            (staged.contains(kv.first) && IsWindow(kv.first)))
            return false;
        ForgetImages(L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(kv.first)) + L"|" + std::to_wstring(kv.second.gen), true);
        return true;
    });
    Save();
}

std::vector<HWND> StageManager::ParkedInOrder() const
{
    std::vector<std::pair<uint64_t, HWND>> bySeq;
    bySeq.reserve(parked_.size());
    for (const auto& [h, p] : parked_)
        bySeq.emplace_back(p.seq, h);
    std::ranges::sort(bySeq);
    std::vector<HWND> out;
    out.reserve(bySeq.size());
    for (const auto& [seq, h] : bySeq)
        out.push_back(h);
    return out;
}

void StageManager::Save()
{
    json::JsonArray list;
    for (HWND h : ParkedInOrder()) {
        json::JsonObject o;
        o.SetNamedValue(L"hwnd", json::JsonValue::CreateNumberValue(double(reinterpret_cast<uintptr_t>(h))));
        o.SetNamedValue(L"key", json::JsonValue::CreateStringValue(parked_[h].key));
        list.Append(o);
    }
    json::JsonArray sets;
    for (const Set& set : recent_) {
        json::JsonArray keys;
        for (const std::wstring& k : set)
            keys.Append(json::JsonValue::CreateStringValue(k));
        sets.Append(keys);
    }
    json::JsonObject root;
    root.SetNamedValue(L"parked", list);
    root.SetNamedValue(L"sets", sets);
    const std::wstring path = StatePath();
    if (!SaveStageState(std::wstring(root.Stringify()), saved_, [&](const std::wstring& text) { return WriteTextFile(path, text); }))
        Log(L"stage: could not write %s", path.c_str());
}

void StageManager::Load()
{
    json::JsonObject root{nullptr};
    std::wstring text = ReadTextFile(StatePath());
    if (text.empty() || !json::JsonObject::TryParse(text, root))
        return;
    saved_ = text;
    if (root.HasKey(L"parked") && root.GetNamedValue(L"parked").ValueType() == json::JsonValueType::Array)
        for (const auto& v : root.GetNamedArray(L"parked")) {
            if (v.ValueType() != json::JsonValueType::Object)
                continue;
            auto o = v.GetObject();
            if (!o.HasKey(L"hwnd") || !o.HasKey(L"key") || o.GetNamedValue(L"hwnd").ValueType() != json::JsonValueType::Number ||
                o.GetNamedValue(L"key").ValueType() != json::JsonValueType::String)
                continue;
            HWND h = reinterpret_cast<HWND>(uintptr_t(o.GetNamedNumber(L"hwnd")));
            // A window handle is only reused once its window is gone; one still down is the same.
            if (!IsWindow(h) || !IsWindowVisible(h) || !IsIconic(h))
                continue;
            Parked p;
            p.key = std::wstring(o.GetNamedString(L"key"));
            if (p.key.find(L'|') == std::wstring::npos)
                p.key = WindowKey(p.key, h); // kept by an earlier version, by app
            p.seq = ++seq_;
            parked_[h] = p;
        }
    if (root.HasKey(L"sets") && root.GetNamedValue(L"sets").ValueType() == json::JsonValueType::Array)
        for (const auto& v : root.GetNamedArray(L"sets")) {
            if (v.ValueType() != json::JsonValueType::Array)
                continue;
            Set set;
            for (const auto& k : v.GetArray()) {
                if (k.ValueType() != json::JsonValueType::String)
                    continue;
                const std::wstring key(k.GetString());
                // An app (an earlier version's set): its windows.
                for (const auto& [h, p] : parked_)
                    if ((p.key == key || (key.find(L'|') == std::wstring::npos && AppOf(p.key) == key)) && !Has(set, p.key))
                        set.push_back(p.key);
            }
            for (const std::wstring& k : set) // each on a card of its own (an earlier version kept the stage's windows together)
                recent_.push_back(Set{k});
        }
    // Apps only known from their windows: each a set of its own.
    for (const auto& [h, p] : parked_)
        if (std::ranges::none_of(recent_, [&](const Set& set) { return Has(set, p.key); }))
            recent_.push_back(Set{p.key});
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) { return std::ranges::none_of(parked_, [&](const auto& kv) { return kv.second.key == k; }); });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
}

// ---- beside the strip --------------------------------------------------------------------------

// A window over the strip's place moves beside it as it takes the stage: up, at once; minimized,
// its place to come back to (its picture lands there). Maximized windows stay as they are, under
// the strip, and so do minimized ones that come back maximized (IsZoomed is false while they are down):
// moved, the picture would land beside the strip and the window then come up over all of the screen.
void StageManager::MoveBeside(HWND window)
{
    if (!window || !IsWindow(window) || !IsWindowEnabled(window) || IsZoomed(window) || !OnMonitor(window, monitor_))
        return;
    const bool down = IsIconic(window) != FALSE;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (down ? (GetWindowPlacement(window, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED)) : Transient(window))
        return;
    auto parked = parked_.find(window);
    RECT frame{};
    if (down) {
        if (parked == parked_.end() || !enabled_ || autoHide_ || IsRectEmpty(&rect_))
            return;
        frame = parked->second.frame;
        if (IsRectEmpty(&frame)) {
            EnqueueAction({Action::Kind::MoveBeside, window, {}, Zone(), WorkArea(), onRight_});
            return;
        }
    } else if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        return;
    }
    RECT to = frame;
    if (!Beside(to))
        return;
    const LONG dl = to.left - frame.left, dr = to.right - frame.right;
    if (parked != parked_.end())
        parked->second.frame = to;
    if (down) {
        EnqueueAction({Action::Kind::MoveBeside, window, frame, Zone(), WorkArea(), onRight_});
    } else {
        RECT r{};
        GetWindowRect(window, &r);
        SetWindowPos(window, nullptr, r.left + dl, r.top, r.right + dr - r.left - dl, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    }
    Log(L"stage: %p moved beside the strip", window);
}

// ---- the strip ---------------------------------------------------------------------------------

MONITORINFO StageManager::MonitorInfo() const
{
    MONITORINFO mi{sizeof(mi)};
    if (monitor_ && GetMonitorInfoW(monitor_, &mi))
        return mi;
    if (const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY); primary && GetMonitorInfoW(primary, &mi))
        return mi;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    mi.rcMonitor = mi.rcWork;
    return mi;
}

RECT StageManager::WorkArea() const { return MonitorInfo().rcWork; }

int StageManager::Capacity() const
{
    RECT wa = WorkArea();
    float avail = float(wa.bottom - wa.top) / PopupScale(wa) - 2 * kVPad - (Vertical(barEdge_) ? 0 : kBarRoom);
    return std::clamp(int((avail + kSlotGap) / kSlot), 1, kMaxEntries);
}

std::vector<StageManager::Entry> StageManager::BuildStripEntries() const
{
    std::vector<Entry> next;
    if (!enabled_)
        return next;
    for (const Set& set : recent_) {
        std::vector<std::pair<uint64_t, HWND>> mine;
        for (const auto& [h, p] : parked_) {
            if (!Has(set, p.key) || !IsWindow(h))
                continue;
            BOOL here = TRUE; // windows put away on another virtual desktop wait there
            if (desktops_ && SUCCEEDED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) && !here)
                continue;
            mine.emplace_back(p.seq, h);
        }
        if (mine.empty())
            continue;
        std::ranges::sort(std::ranges::reverse_view(mine)); // most recent first
        Entry e;
        e.keys = set;
        std::ranges::stable_sort(e.keys, [&](const std::wstring& a, const std::wstring& b) {
            auto newest = [&](const std::wstring& k) {
                uint64_t n = 0;
                for (const auto& [seq, h] : mine)
                    if (parked_.at(h).key == k)
                        n = std::max(n, seq);
                return n;
            };
            return newest(a) > newest(b);
        });
        for (size_t i = 0; i < mine.size() && i < kStack; ++i)
            e.windows.push_back(mine[i].second);
        next.push_back(std::move(e));
    }
    return next;
}

void StageManager::Rebuild(bool keepAnchor)
{
    std::vector<Entry> next = BuildStripEntries();
    scrollCount_ = next.size();
    const size_t cap = size_t(Capacity());
    scroll_.Fit(scrollCount_, cap);
    if (keepAnchor && !entries_.empty()) {
        auto anchor = std::ranges::find_if(next, [&](const Entry& e) { return e.keys == entries_.front().keys; });
        if (anchor != next.end())
            scroll_.Anchor(size_t(anchor - next.begin()), scrollCount_, cap);
    }
    if (!next.empty())
        std::ranges::rotate(next, next.begin() + scroll_.Offset());
    if (next.size() > cap)
        next.resize(cap);
    // An index can now name another group. Check identity before the old keys are moved/cleared.
    auto sameSlot = [&](int index) {
        return index >= 0 && size_t(index) < next.size() && size_t(index) < entries_.size() && next[size_t(index)].keys == entries_[size_t(index)].keys;
    };
    if (!sameSlot(pressed_))
        pressed_ = -1;
    if (!sameSlot(closeHot_))
        closeHot_ = -1;
    if (!sameSlot(closePressed_))
        closePressed_ = -1;
    const bool visible = shown_ && surface_.Visible();
    // A set keeps its card (where it is, how far it is hovered).
    for (Entry& e : next) {
        auto old = std::ranges::find_if(entries_, [&](const Entry& o) { return o.keys == e.keys; });
        if (old != entries_.end()) {
            e.hover = old->hover;
            e.drop = old->drop;
            e.y = old->y;
            e.vy = old->vy;
            e.appear = old->appear;
            e.va = old->va;
            e.closeAt = old->closeAt;
            old->keys.clear(); // taken
        } else {
            e.appear = visible ? 0.f : 1.f; // a new card fades in; a strip coming out slides as a whole
        }
    }
    // Cards that went fade out where they were.
    for (Entry& old : entries_) {
        if (old.keys.empty() || !visible || old.appear < 0.02f)
            continue;
        old.targetAppear = 0;
        old.hover = motion::Follow{};
        old.drop = motion::Follow{};
        leaving_.push_back(std::move(old));
    }
    if (!visible)
        leaving_.clear();
    const bool changed = next.size() != entries_.size() ||
                         !std::equal(next.begin(), next.end(), entries_.begin(), [](const Entry& a, const Entry& b) { return a.keys == b.keys; }) ||
                         !leaving_.empty();
    entries_ = std::move(next);
    if (hover_ >= int(entries_.size()))
        hover_ = -1;
    if (pressed_ >= int(entries_.size()))
        pressed_ = -1;
    Layout();
    if (entries_.empty() && leaving_.empty()) {
        Hide();
        return;
    }
    if (surface_.Visible()) {
        surface_.Move(rect_);
        surface_.Invalidate();
    }
    if (changed)
        Watch();
}

// The strip's window has room for Capacity() boxes, in the middle of what the bar leaves of the
// screen's height, from the screen's edge; the boxes stand in the middle of it, and glide when
// that changes.
void StageManager::Layout()
{
    RECT wa = WorkArea();
    const float s = PopupScale(wa);
    const int cap = Capacity();
    size_ = D2D1::SizeF(kStripW, float(cap) * kSlot - kSlotGap + 2 * kVPad);
    const size_t n = entries_.size();
    const float used = n ? float(n) * kSlot - kSlotGap : 0;
    const float top = (size_.height - used) / 2;
    const bool snap = !surface_.Visible();
    for (size_t i = 0; i < n; ++i) {
        Entry& e = entries_[i];
        e.targetY = top + float(i) * kSlot;
        if (snap || e.appear == 0) {
            e.y = e.targetY; // a new card fades in at its place
            e.vy = 0;
        }
    }
    const LONG w = LONG(std::lround(size_.width * s)), h = LONG(std::lround(size_.height * s));
    const LONG room = LONG(std::lround(kBarRoom * s));
    const LONG x0 = wa.left + (!onRight_ && barEdge_ == BarEdge::Left ? room : 0);
    const LONG x1 = wa.right - (onRight_ && barEdge_ == BarEdge::Right ? room : 0);
    const LONG y0 = wa.top + (barEdge_ == BarEdge::Top ? room : 0);
    const LONG y1 = wa.bottom - (barEdge_ == BarEdge::Bottom ? room : 0);
    LONG x = onRight_ ? x1 - w : x0;
    LONG y = y0 + std::max<LONG>(0, (y1 - y0 - h) / 2);
    rect_ = RECT{x, y, x + w, y + h};
}

float StageManager::Reveal(int index, double nowMs) const
{
    // Out: one card after the other, from the top. In: all together.
    const double delay = shown_ && revealFrom_ < 0.01f ? kRevealStagger * index : 0;
    const float t = float(std::clamp((nowMs - revealStart_ - delay) / (shown_ ? kRevealInMs : kRevealOutMs), 0.0, 1.0));
    const float target = shown_ ? 1.f : 0.f;
    return revealFrom_ + (target - revealFrom_) * (shown_ ? EaseOut3(t) : t * t);
}

bool StageManager::Animate(double nowMs)
{
    const float dt = lastStep_ > 0 ? float(std::clamp((nowMs - lastStep_) / 1000.0, 0.0, 0.05)) : 0.f;
    lastStep_ = nowMs;
    bool moving = false;
    auto step = [&](Entry& e) {
        kGlide.Step(e.y, e.vy, e.targetY, dt);
        kFadeSpring.Step(e.appear, e.va, e.targetAppear, dt);
        if (motion::Spring::AtRest(e.y, e.vy, e.targetY, 0.2f))
            e.y = e.targetY, e.vy = 0;
        else
            moving = true;
        if (motion::Spring::AtRest(e.appear, e.va, e.targetAppear, 0.004f))
            e.appear = e.targetAppear, e.va = 0;
        else
            moving = true;
        e.appear = std::clamp(e.appear, 0.f, 1.f);
    };
    for (Entry& e : entries_)
        step(e);
    for (Entry& e : leaving_)
        step(e);
    std::erase_if(leaving_, [](const Entry& e) { return e.appear <= 0.01f; });
    const double span = shown_ ? kRevealInMs + kRevealStagger * double(entries_.size()) : kRevealOutMs;
    if (nowMs - revealStart_ < span)
        moving = true;
    if (!moving)
        lastStep_ = 0;
    return moving;
}

// The boxes' span, with room for their shadows and a hovered box's growth: the rest of the column
// lets clicks through. While cards move the region only grows (where they go is in it already); it
// is fitted again when they rest.
void StageManager::UpdateRegion(bool moving)
{
    float top = size_.height, bottom = 0;
    for (const auto* list : {&entries_, &leaving_})
        for (const Entry& e : *list) {
            top = std::min({top, e.y, e.targetY});
            bottom = std::max({bottom, e.y + kBoxH, e.targetY + kBoxH});
        }
    RECT wr{};
    if (!GetWindowRect(surface_.Hwnd(), &wr))
        return;
    const float s = surface_.Scale();
    const LONG inset = rect_.top - wr.top;
    RECT r{0, std::max<LONG>(0, inset + LONG(std::floor((top - 24) * s))), wr.right - wr.left,
           std::min<LONG>(wr.bottom - wr.top, inset + LONG(std::ceil((bottom + 34) * s)))};
    if (bottom <= top)
        r = RECT{0, 0, 0, 0};
    if (moving && !IsRectEmpty(&region_))
        UnionRect(&r, &r, &region_);
    if (EqualRect(&r, &region_))
        return;
    region_ = r;
    HRGN rgn = CreateRectRgn(r.left, r.top, r.right, r.bottom);
    if (!SetWindowRgn(surface_.Hwnd(), rgn, FALSE))
        DeleteObject(rgn);
}

RECT StageManager::Zone() const
{
    const MONITORINFO mi = MonitorInfo();
    return onRight_ ? RECT{rect_.left, rect_.top, mi.rcMonitor.right, rect_.bottom} : RECT{mi.rcMonitor.left, rect_.top, rect_.right, rect_.bottom};
}

bool StageManager::Covered()
{
    const ULONGLONG now = GetTickCount64();
    if (coveredAt_ && now - coveredAt_ < kCoverMs)
        return covered_;
    coveredAt_ = now;
    // An app filling the whole screen (a game, a video): out of its way.
    covered_ = FullScreenInFront(monitor_);
    if (covered_)
        return covered_;
    const RECT zone = Zone();
    std::vector<HWND> windows = excludedWindows_; // an excluded app's window over the strip counts too
    for (const TaskItem& t : items_)
        windows.insert(windows.end(), t.windows.begin(), t.windows.end());
    for (HWND h : windows) {
        if (h == dragWindow_ || parked_.contains(h) || !IsWindowVisible(h) || IsIconic(h) || !OnMonitor(h, monitor_))
            continue;
        RECT r{}, hit{};
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(h, &r))
            continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue; // on another virtual desktop
        if (IntersectRect(&hit, &r, &zone))
            return covered_ = true;
    }
    return covered_;
}

bool StageManager::NeedsAdopt(ULONGLONG now) const
{
    if (!minimizeToStrip_)
        return false;
    if (std::ranges::any_of(up_, [this, now](const auto& u) { return !parked_.contains(u.first) && IsIconic(u.first) && !IsRestoring(u.first, now); }))
        return true;
    for (const TaskItem& t : items_) {
        if (!Has(stage_, t.key))
            continue;
        for (HWND h : t.windows) {
            if (parked_.contains(h) || !IsWindowVisible(h))
                continue;
            if (IsIconic(h)) {
                if (!IsRestoring(h, now))
                    return true;
            } else if (!up_.contains(h) && OnMonitor(h, monitor_)) {
                return true;
            }
        }
    }
    return false;
}

// Every kWatchMs while on. Always shown, the strip is out unless a window covers its place; then,
// as auto-hidden, it comes out over the windows while the pointer rests on that edge of the screen,
// and goes once the pointer has left it.
void StageManager::Watch()
{
    const ULONGLONG now = GetTickCount64();
    POINT p{};
    GetCursorPos(&p);
    const bool anyButtonDown = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
    const bool dragging = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
    // A click on the desktop clears the stage, while pressing to drag a desktop file must not.
    if (enabled_) {
        HWND fg = GetForegroundWindow();
        wchar_t cls[16] = {};
        const bool atDesktop = fg && GetClassNameW(fg, cls, 16) && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"));
        // A window minimized on the stage goes to the strip even when nothing else told of it.
        if (NeedsAdopt(now) && AdoptMinimized())
            Rebuild();
        if (StepStageDesktopStow(desktopStow_, atDesktop, anyButtonDown, p, GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG), now, kDesktopMs)) {
            if (AdoptMinimized())
                Rebuild();
            if (!stage_.empty()) {
                pending_.clear();
                Stage(Set{}, false, nullptr, false);
                Rebuild();
            }
        }
        KeepPicture();
    }
    if (!enabled_ || entries_.empty()) {
        peek_ = false;
        edgeSince_ = 0;
        StepDrag(POINT{});
        if (leaving_.empty())
            Hide();
        return;
    }
    const MONITORINFO mi = MonitorInfo();
    const bool adjacentAtPointerY = MonitorFromPoint(POINT{onRight_ ? mi.rcMonitor.right : mi.rcMonitor.left - 1, p.y}, MONITOR_DEFAULTTONULL) != nullptr;
    const StageEdgeResult edge =
        EvaluateStageEdge(mi.rcMonitor, mi.rcWork, p, onRight_, adjacentAtPointerY, PopupScale(mi.rcWork), dragging, dragWindow_ != nullptr, kEdgeMs);
    edgeSince_ = edge.atEdge ? (edgeSince_ ? edgeSince_ : now) : 0;
    if (!autoHide_ && !Covered()) {
        peek_ = false;
        Show();
        StepDrag(p);
        return;
    }
    if (!peek_ && edgeSince_ && now - edgeSince_ >= edge.dwellMs && !FullScreenInFront(monitor_)) {
        peek_ = true;
        leftAt_ = 0;
    }
    if (peek_) {
        const RECT zone = Zone();
        if (edge.atEdge || inside_ || PtInRect(&zone, p))
            leftAt_ = 0;
        else if (!leftAt_)
            leftAt_ = now;
        else if (now - leftAt_ >= kPeekLeaveMs)
            peek_ = false;
    }
    if (peek_)
        Show();
    else
        Hide();
    StepDrag(p);
}

void StageManager::Show()
{
    if (shown_) {
        if (surface_.Visible())
            surface_.Move(rect_);
        return;
    }
    const double now = ClockMs();
    const bool visible = surface_.Visible();
    revealFrom_ = visible ? Reveal(0, now) : 0.f; // turning back on its way in
    revealStart_ = now;
    shown_ = true;
    closeHot_ = closePressed_ = -1;
    if (!visible) {
        hover_ = pressed_ = dragHover_ = -1;
        leaving_.clear();
        for (Entry& e : entries_) {
            e.y = e.targetY, e.vy = 0;
            e.appear = e.targetAppear = 1, e.va = 0;
            e.hover = e.drop = motion::Follow{};
        }
        region_ = RECT{};
        const MONITORINFO mi = MonitorInfo();
        POINT anchor{onRight_ ? mi.rcMonitor.right : mi.rcMonitor.left, (rect_.top + rect_.bottom) / 2};
        surface_.Show(rect_, false, anchor, false); // the cards slide out themselves
    } else {
        surface_.Move(rect_);
        surface_.Invalidate();
    }
}

void StageManager::Hide()
{
    if (!shown_)
        return;
    const double now = ClockMs();
    revealFrom_ = Reveal(0, now), revealStart_ = now, shown_ = false, inside_ = false;
    hover_ = pressed_ = dragHover_ = closeHot_ = closePressed_ = -1;
    surface_.Invalidate(); // the cards slide back into the edge; then the window goes
}

std::shared_ptr<const WindowPicture> StageManager::PictureOf(HWND window, const std::wstring& app, std::wstring& cacheKey)
{
    auto it = pictures_.find(window);
    if (it != pictures_.end() && it->second.picture && it->second.picture->ok) {
        cacheKey = L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(window)) + L"|" + std::to_wstring(it->second.gen);
        return it->second.picture;
    }
    // No picture (put away before Atrium started, or one that cannot be printed): its app's icon.
    cacheKey = L"stage|p|" + AppOf(app);
    const float s = PopupScale(WorkArea());
    const bool dark = AppsDark();
    auto& ph = placeholders_[AppOf(app)];
    if (!ph.picture || !ph.hasIcon || ph.scale != s || ph.dark != dark) {
        const int w = int(std::lround(kBoxW * s)), h = int(std::lround(kBoxW * s / 1.6f));
        // The largest icon the app has, up to 52 DIP.
        IconImage icon;
        for (int px : {int(std::lround(52 * s)), 64, 48, 32}) {
            icon = tasks_.Icon(AppOf(app), px);
            if (!icon.Empty())
                break;
        }
        if (!ph.picture || ph.scale != s || ph.dark != dark || !icon.Empty()) {
            if (ph.picture)
                ForgetImages(cacheKey, false);
            ph.picture = std::make_shared<const WindowPicture>(PlaceholderPicture(icon, w, h, dark));
            ph.hasIcon = !icon.Empty();
            ph.dark = dark;
            ph.scale = s;
        }
    }
    return ph.picture;
}

bool StageManager::OnTimerMessage(WPARAM w)
{
    if (w == kWatchTimer) {
        Watch();
        return true;
    }
    if (w == kDownTimer) {
        KillTimer(surface_.Hwnd(), kDownTimer);
        std::vector<HWND> late;
        for (const auto& [h, p] : parked_)
            if (!p.down && GetTickCount64() - p.since >= kDownMs - 20)
                late.push_back(h);
        for (HWND h : late)
            PutDown(h, false); // no picture in time: it waits without one
        if (std::ranges::any_of(parked_, [](const auto& kv) { return !kv.second.down; }))
            SetTimer(surface_.Hwnd(), kDownTimer, kDownMs / 4, nullptr);
        return true;
    }
    if (w == kHideTimer) {
        KillTimer(surface_.Hwnd(), kHideTimer);
        if (shown_ && entries_.empty() && leaving_.empty())
            Hide(); // slides away, then comes back here
        else if (!shown_)
            surface_.Hide(false);
        return true;
    }
    if (w == kRaiseTimer) {
        KillTimer(surface_.Hwnd(), kRaiseTimer);
        if (raise_ && IsWindow(raise_) && IsWindowVisible(raise_) && !IsIconic(raise_))
            SetWindowPos(raise_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        raise_ = nullptr;
        return true;
    }
    return false;
}

bool StageManager::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    (void)result;
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_STAGE_PICTURE: PictureTaken(); return true;
    case WM_TIMER: return OnTimerMessage(w);
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // Wheel messages carry screen pixels, unlike the other Surface mouse messages.
        const POINT content = surface_.ScreenToContent(pt);
        if (!shown_ || EntryAt(content) < 0)
            return false;
        pressed_ = -1;
        if (scroll_.Wheel(GET_WHEEL_DELTA_WPARAM(w), m == WM_MOUSEHWHEEL, scrollCount_, size_t(Capacity())))
            hover_ = -1, Rebuild(false); // an explicit scroll changes the anchor; model-only rebuilds keep it
        surface_.Invalidate();
        return true;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (!inside_)
            Foresee();
        inside_ = true;
        int i = shown_ ? EntryAt(pt) : -1;
        if (i != hover_) {
            hover_ = i;
            surface_.Invalidate();
        }
        CloseInput(m, pt);
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false, inside_ = false;
        hover_ = pressed_ = closeHot_ = closePressed_ = -1;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        if (CloseInput(m, pt))
            return true;
        Foresee();
        pressed_ = shown_ ? EntryAt(pt) : -1;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        if (CloseInput(m, pt))
            return true;
        int i = EntryAt(pt);
        bool same = shown_ && i >= 0 && i == pressed_;
        pressed_ = -1;
        surface_.Invalidate();
        if (same) {
            Set keys = entries_[size_t(i)].keys;
            Log(L"stage: %s picked in the strip%s", keys.front().c_str(), (w & MK_SHIFT) ? L" (added)" : L"");
            Pick(keys, (w & MK_SHIFT) != 0);
        }
        return true;
    }
    default: return false;
    }
}

} // namespace atrium
