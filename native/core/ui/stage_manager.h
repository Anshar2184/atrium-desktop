// Stage Manager, after macOS's: the window in front stays on the stage, the others wait in a strip
// at the side of the screen, each on a card of its own (two windows of one app are two cards),
// standing at an angle in perspective, with its shadow and its app's icon, nothing behind them. A
// click there swaps the sets; Shift and a click brings a set onto the stage beside the one there,
// and the windows on the stage go back to the strip each on a card of its own (a window dragged onto a
// card joins its set, and they come back together). Activating a window any other way (the bar, Alt+Tab, a
// notification) changes the stage too.
//
// Waiting windows are minimized without activation: Windows' own state, so nothing is lost if
// Atrium goes away. A minimized window has no picture, so one is taken just before
// (ui/window_snapshot.h). The window then goes at once (its own minimize animation turned off) and
// its picture flies from where it was into its place in the strip; picked there, the picture flies
// back and the window comes up under it as it lands. Which windows Atrium put away is kept in %LOCALAPPDATA%\Atrium\stage.json,
// so that a restart picks them up again; turning Stage Manager off or quitting Atrium brings them
// all back.
//
// Only the selected desktop display is managed. Always shown, a window taking the stage over the
// strip's place moves beside it (so that the strip stays out; stageAvoid off, it stays where it is); the strip steps aside while a window
// covers its place anyway (maximized, or put there), and comes out over it while the pointer rests on
// that edge. Auto-hidden, it only comes out that way.
#pragma once

#include "bar_edge.h"
#include "motion.h"
#include "shell/task_model.h"
#include "ui/stage_merge.h"
#include "ui/stage_scroll.h"
#include "ui/stage_open.h"
#include "ui/stage_watch.h"
#include "stage_excluded.h"
#include "ui/surface.h"
#include "ui/window_snapshot.h"

#include <shobjidl.h>
#include <winrt/base.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atrium {

template<class Writer> [[nodiscard]] bool SaveStageState(const std::wstring& text, std::wstring& saved, Writer&& write)
{
    if (text == saved)
        return true;
    if (!std::forward<Writer>(write)(text))
        return false;
    saved = text;
    return true;
}

class StageManager {
public:
    StageManager(HINSTANCE instance, TaskModel& tasks);
    ~StageManager();
    StageManager(const StageManager&) = delete;
    StageManager& operator=(const StageManager&) = delete;

    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
    // Apps Stage Manager leaves alone (screenshot tools and anything the user lists).
    void SetExcluded(std::vector<std::wstring> apps);
    bool Excluded(const std::wstring& app) const { return StageAppExcluded(excluded_, app); }
    // launch: how to start it when none of its windows is up (else the task model starts it by its key).
    void OpenInCurrentGroup(const std::wstring& app, const std::function<void()>& launch = nullptr);
    void SetOnRight(bool onRight);
    void SetBarEdge(BarEdge edge);
    void SetAutoHide(bool autoHide);
    // A window minimized on the stage goes to the strip (setting stageMinimize), on a card of its own.
    // A window whose minimize Stage Manager animates itself (into its strip).
    bool OwnsMinimize(HWND window) const
    {
        return minimizeToStrip_ && enabled_ && (up_.contains(window) || parked_.contains(window) || Flying(window) || InStage(window));
    }
    void SetMinimizeToStrip(bool on);
    // EVENT_SYSTEM_MINIMIZESTART (its hook while the option is on): at once, not when the task model next reports.
    void Minimized(HWND window);
    // A window dragged onto a group in the strip joins it (stageDragMerge).
    void SetDragMerge(bool on);
    void SetAvoid(bool on) { avoid_ = on; } // stageAvoid: a window taking the stage over the strip moves beside it
    void SetCloseButton(bool on); // stageCloseButton: a card under the pointer shows a cross that closes its windows
    void SetScrollWrap(bool on);  // stageScrollWrap: scrolling the strip wraps past the last card back to the first
    // A window coming back from the strip will fill this monitor (maximized): the bar may go at once.
    std::function<void(HMONITOR)> onCoverSoon;
    void MoveSize(HWND window, bool start) { start ? BeginDrag(window) : FinishDrag(window); } // EVENT_SYSTEM_MOVESIZESTART/END
    void SetMonitor(HMONITOR monitor, bool refresh = false);
    // The task model changed (every group, also those the bar does not show).
    void Update(const std::vector<TaskItem>& items);
    // Everything put away comes back (on exit).
    void RestoreAll();
    void SignalSnapshotStop() { snapshots_.SignalStop(); }
    bool WaitSnapshotStop(DWORD timeoutMs) const { return snapshots_.WaitStop(timeoutMs); }

private:
    struct Parked {
        std::wstring key;
        uint64_t seq = 0; // when it was put away
        ULONGLONG since = 0;
        bool down = true; // minimized (it waits for its picture first)
        RECT frame{};     // where it was on the screen, px (it comes back there)
    };
    struct Picture {
        std::shared_ptr<const WindowPicture> picture;
        std::shared_ptr<const WindowPicture> large; // near the window's size, its own corners (flights)
        unsigned gen = 0;          // its images' cache keys change with it
        unsigned shrinkingGen = 0; // gen currently queued/running for background downsampling
        ULONGLONG taken = 0;       // when it was taken (or asked for)
        int misses = 0;            // consecutive failed/blank captures before a picture exists
    };
    struct Placeholder {
        std::shared_ptr<const WindowPicture> picture;
        bool hasIcon = false, dark = false;
        float scale = 0;
    };
    struct Action {
        enum class Kind { MoveBeside, Activate, ShowQuietly } kind = Kind::MoveBeside;
        HWND window = nullptr;
        RECT frame{}, zone{}, workArea{};
        bool onRight = false;
    };
    // A picture in perspective: `rect` (DIP) turned `angle` degrees about the upright axis through
    // (cx, cy), `z` towards the eye, seen from in front of (ox, oy).
    struct Pose {
        D2D1_RECT_F rect{};
        float cx = 0, cy = 0, ox = 0, oy = 0, angle = 0, z = 0;
    };
    // A window's picture on its way between the window's place and the strip, on the flight layer
    // (DIP from the monitor's top left).
    struct Flight {
        HWND window = nullptr;
        std::shared_ptr<const WindowPicture> picture;
        std::wstring key;
        std::shared_ptr<const WindowPicture> large; // the picture becomes this near the window's place
        std::wstring largeKey;
        bool toStrip = true;
        Pose from;        // fromStrip: where it was in the strip
        Pose place;       // the window's place
        double elapsed = 0, last = 0; // ms of the flight drawn, and when the last frame was
        bool restored = false, activate = false;
    };
    using Set = std::vector<std::wstring>; // windows (WindowKey) that come and go together
    struct Entry {
        Set keys;
        std::vector<HWND> windows; // front first, at most kStack
        motion::Follow hover, drop;
        // Cards glide to their place (a spring) and come and go with a fade and a little scale.
        float y = 0, vy = 0, targetY = 0;
        float appear = 0, va = 0, targetAppear = 1;
        D2D1_POINT_2F closeAt{-1e6f, -1e6f}; // its close button, content DIP, as last painted
    };

    static constexpr UINT_PTR kRaiseTimer = 0x5C03, kHideTimer = 0x5C04, kFlightTimer = 0x5C05;
    static constexpr UINT kRaiseMs = 220;
    static constexpr ULONGLONG kPendingMs = 1200;
    static constexpr double kFlightMs = 400;
    static constexpr float kBoxW = 172, kBoxH = 112, kTilt = 22;
    static constexpr int kLargeOld = 1440;
    static constexpr size_t kLargeKept = 4;

    static bool Transient(HWND h);
    static void SetTransitions(HWND h, bool on);
    static std::wstring AppKeyOf(const std::wstring& key);
    static bool HasKey(const Set& set, const std::wstring& key);
    void SyncMoveHook();
    // A window being dragged (stage_drag.cpp): which group it is over, and what happens where it lands.
    void BeginDrag(HWND window);
    void StepDrag(POINT screen);
    void FinishDrag(HWND window);
    void CancelDrag();
    // The cards' close buttons (stage_close.cpp): painted at `corner` (the front picture's top right, the
    // card's own coordinates) and kept for the pointer at `at` (where it shows); the pointer's messages.
    void PaintClose(Canvas& c, Entry& e, int index, D2D1_POINT_2F corner, D2D1_POINT_2F at, float shown);
    int CloseAt(POINT pt) const;
    bool CloseInput(UINT m, POINT pt);
    void CloseEntry(int index);
    void Stage(const Set& keys, bool add, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly = nullptr);
    void Park(HWND window, const std::wstring& key);
    void Unpark(const Set& keys, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly = nullptr, bool moveBeside = true);
    void Pick(const Set& keys, bool add, HWND front = nullptr);
    void PictureTaken();       // WM_STAGE_PICTURE
    void ApplyShrinkResult(const WindowSnapshots::Result& r);
    void ScheduleOldLargeShrinks();
    void Launch();             // the flight layer, up for a flight
    void Foresee();            // pictures of the stage's windows, ahead of a pick
    void PutDown(HWND window, bool fly); // minimizes a window put away, once; its picture flies to the strip
    Set SetOf(const std::wstring& key) const; // the set in the strip `key` belongs to, or just it
    void Prune();
    void ReleaseUp();          // the stage's windows get their own minimize animation back
    bool AdoptMinimized();     // the stage's apps whose windows were all minimized go to the strip
    bool NeedsAdopt(ULONGLONG now) const;
    void UnparkDirect(HWND h, bool showQuietly, ULONGLONG now);
    void KeepPicture();        // a picture of the window in front, ready for when it is minimized
    void RequestPicture(HWND window, bool urgent = false);
    void SetLarge(Flight& f);  // the flight's large picture, if the window has one
    bool Beside(RECT& frame) const { return enabled_ && avoid_ && !autoHide_ && !IsRectEmpty(&rect_) && StageBesideRect(frame, Zone(), WorkArea(), onRight_); }
    void MoveBeside(HWND window);   // the window beside the strip (minimized: where it comes back)
    void ForgetImages(const std::wstring& key, bool large)
    {
        surface_.ForgetImage(key);
        surface_.ForgetImage(key + L"|s");
        flight_.ForgetImage(key);
        flight_.ForgetImage(key + L"|s");
        for (const wchar_t* faded : {L"|fd", L"|fd|s", L"|fu", L"|fu|s"})
            surface_.ForgetImage(key + faded);
        if (large)
            flight_.ForgetImage(key + L"|L");
    }
    void EnqueueAction(Action action)
    {
        std::unique_lock lk(actionLock_);
        if (!actionStop_)
            actions_.push_back(action);
        lk.unlock();
        actionCv_.notify_one();
    }
    static void ShowQuietly(HWND h);
    void ActionWorker()
    {
        for (;;) {
            Action a;
            {
                std::unique_lock lk(actionLock_);
                actionCv_.wait(lk, [this] { return actionStop_ || !actions_.empty(); });
                if (actionStop_)
                    return;
                a = actions_.front();
                actions_.pop_front();
            }
            if (!IsWindow(a.window))
                continue;
            if (a.kind != Action::Kind::MoveBeside) {
                a.kind == Action::Kind::Activate ? TaskModel::ActivateWindowDirect(a.window) : ShowQuietly(a.window);
                continue;
            }
            WINDOWPLACEMENT wp{sizeof(wp)};
            if (!IsIconic(a.window) || !IsWindowEnabled(a.window) || IsZoomed(a.window) || !GetWindowPlacement(a.window, &wp) ||
                (wp.flags & WPF_RESTORETOMAXIMIZED))
                continue;
            RECT frame = IsRectEmpty(&a.frame) ? wp.rcNormalPosition : a.frame, to = frame;
            if (!StageBesideRect(to, a.zone, a.workArea, a.onRight))
                continue;
            wp.rcNormalPosition.left += to.left - frame.left;
            wp.rcNormalPosition.right += to.right - frame.right;
            wp.showCmd = SW_SHOWMINNOACTIVE;
            wp.flags = 0;
            SetWindowPlacement(a.window, &wp);
        }
    }
    void Save();
    std::vector<HWND> ParkedInOrder() const; // put away first, first
    void Load();
    int Capacity() const;

    // The strip.
    void Rebuild(bool keepAnchor = true);
    std::vector<Entry> BuildStripEntries() const;
    void Layout();
    void Show();
    void Hide();
    void Paint(Canvas& c);
    void PaintEntry(Canvas& c, Entry& e, int index, bool live, int fade = 0); // fade: 1 away down, -1 away up (stage_fade.h)
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool OnTimerMessage(WPARAM w);
    void ProcessOpenIntent();
    void SyncUpTransitions(const std::unordered_map<HWND, RECT>& up);
    void FlyAdopted(const std::vector<HWND>& flyIn);
    bool Animate(double nowMs);                  // steps the cards; true while any moves
    float Reveal(int index, double nowMs) const; // how far a card is out of the screen's edge, 0..1
    void UpdateRegion(bool moving);              // clicks around the cards reach what is under them
    void Watch();                                // the strip's timer: is it covered, does the pointer ask for it
    bool Covered();                              // a window on the stage is where the strip goes
    RECT Zone() const;                           // where the strip goes, to the screen's edge, px
    RECT WorkArea() const;
    MONITORINFO MonitorInfo() const;
    D2D1_RECT_F Box(const Entry& e) const { return StageBoxRect(size_.width, e.y, onRight_); } // the windows' room, content DIP
    int EntryAt(POINT pt) const;
    std::shared_ptr<const WindowPicture> PictureOf(HWND window, const std::wstring& app, std::wstring& cacheKey);
    // Where window j of a card stands at rest, strip DIP; `angle` its turn.
    Pose PoseOf(const Entry& e, size_t j, const WindowPicture& p, float angle) const;
    bool StripPose(HWND window, Pose& pose);   // at rest in the strip, flight DIP
    Pose PlacePose(const RECT& frame) const;  // a window's place on the screen, flight DIP
    D2D1_POINT_2F StripOffset() const;        // strip DIP to flight DIP
    bool Flying(HWND window) const
    {
        return std::ranges::any_of(flights_, [window](const Flight& f) { return f.window == window; });
    }
    static constexpr ULONGLONG kSettleMs = 2000;
    bool FlyingFromStrip(HWND window) const
    {
        return std::ranges::any_of(flights_, [window](const Flight& f) { return f.window == window && !f.toStrip; });
    }
    bool IsRestoring(HWND window, ULONGLONG now) const;
    void PruneRestoring(ULONGLONG now);
    bool HasPicture(HWND window) const
    {
        auto it = pictures_.find(window);
        return it != pictures_.end() && it->second.picture && it->second.picture->ok;
    }
    bool InStage(HWND window) const
    {
        return std::ranges::any_of(items_, [this, window](const TaskItem& t) {
            return std::ranges::find(stage_, t.key) != stage_.end() && std::ranges::find(t.windows, window) != t.windows.end();
        });
    }
    bool StepFlight(Canvas& c, Flight& f);
    void PaintFlights(Canvas& c);
    void PaintEntryIcons(Canvas& c, Entry& e, const Pose& front, D2D1_RECT_F box, float slide, float k, float hv, int index, bool live, bool icons = true);
    void DrawPicture(Canvas& c, const WindowPicture& p, const std::wstring& key, const Pose& pose, float opacity, float shadow = 1, bool image = true,
                     int fade = 0);
    void DrawFaded(Canvas& c, const WindowPicture& p, const std::wstring& key, const Pose& pose, const D2D1::Matrix4x4F& m, float opacity, float shadow,
                   int fade);

    TaskModel& tasks_;
    Surface surface_;
    Surface flight_; // over the whole screen while pictures fly
    WindowSnapshots snapshots_;
    winrt::com_ptr<IVirtualDesktopManager> desktops_; // the strip shows the current virtual desktop's windows
    bool enabled_ = false, onRight_ = false, autoHide_ = false, minimizeToStrip_ = false, dragMerge_ = true, closeButton_ = true, scrollWrap_ = true,
         avoid_ = true;
    int closeHot_ = -1, closePressed_ = -1; // the card whose close button is under the pointer, pressed
    BarEdge barEdge_ = BarEdge::Bottom;
    std::unordered_map<HWND, RECT> up_; // the stage's windows up on this screen at the last look, and where (px)
    std::unordered_map<HWND, ULONGLONG> restoring_; // unparked without a picture, waiting to finish restoring
    HWINEVENTHOOK minimizeHook_ = nullptr, moveHook_ = nullptr;
    HWND dragWindow_ = nullptr;
    std::wstring dragKey_;
    RECT dragStart_{};
    HWND front_ = nullptr;    // the stage's window in front at the last look
    ULONGLONG frontSince_ = 0; // since when (its picture waits until it has drawn itself)
    bool applied_ = false;             // the settings have said once whether it is on
    std::wstring saved_;               // stage.json as last written
    HWND raise_ = nullptr;             // to put back on top once the windows coming back have
    HMONITOR monitor_ = nullptr;
    std::vector<TaskItem> items_;
    Set stage_;                        // the apps on the stage
    std::vector<Set> recent_;          // waiting sets, most recent first
    std::unordered_map<HWND, Parked> parked_;
    std::unordered_map<HWND, Picture> pictures_;
    std::unordered_map<std::wstring, Placeholder> placeholders_; // by app
    bool shrinkAfterFlight_ = false;
    unsigned pictureGen_ = 0;
    uint64_t seq_ = 0;
    Set pending_;                      // picked in the strip, not in front yet
    ULONGLONG pendingUntil_ = 0;
    StageOpenIntent openIntent_;
    std::vector<HWND> excludedWindows_; // windows of excluded apps: never staged, still covering the strip
    std::vector<std::wstring> excluded_ = DefaultStageExcluded();

    std::vector<Entry> entries_;
    std::vector<Entry> leaving_;       // cards fading out (picked, or their windows gone)
    std::vector<Flight> flights_;
    double lastStep_ = 0;
    RECT region_{};                    // the window region last set, client px
    RECT rect_{};                      // the strip, screen px: room for Capacity() cards
    D2D1_SIZE_F size_{};               // DIP
    int hover_ = -1, pressed_ = -1, dragHover_ = -1;
    StageScroll scroll_;
    size_t scrollCount_ = 0;
    bool tracking_ = false, inside_ = false;
    // Out (or on its way out) of the screen's edge; the cards slide out one after the other.
    bool shown_ = false;
    float revealFrom_ = 0;
    double revealStart_ = 0;
    bool peek_ = false;                // shown over the stage because the pointer asked
    ULONGLONG edgeSince_ = 0, leftAt_ = 0;
    StageDesktopStowState desktopStow_{};
    bool covered_ = false;
    ULONGLONG coveredAt_ = 0;
    std::mutex actionLock_;
    std::condition_variable actionCv_;
    std::deque<Action> actions_;
    bool actionStop_ = false;
    std::thread actionWorker_;
};

} // namespace atrium
