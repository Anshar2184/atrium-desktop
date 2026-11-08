// Stage Manager, after macOS's: the app in front stays on the stage, the others wait in a strip at
// the side of the screen as live pictures (DWM thumbnails) on small pieces of the bar's glass. A
// click there swaps the sets; Shift and a click brings a set onto the stage beside the one there,
// and the apps on the stage together go back to the strip as one set, to come back together.
// Activating a window any other way (the bar, Alt+Tab, a notification) changes the stage too.
//
// Waiting windows are minimized without activation: Windows' own state, so nothing is lost if
// Atrium goes away, and their pictures stay (DWM keeps a minimized window's last frame). Which
// windows Atrium put away is kept in %LOCALAPPDATA%\Atrium\stage.json, so that a restart picks
// them up again; turning Stage Manager off or quitting Atrium brings them all back.
//
// Only the bar's monitor is managed. The strip steps aside while a window on the stage covers its
// place, and comes back while the pointer rests on that edge of the screen.
#pragma once

#include "motion.h"
#include "shell/task_model.h"
#include "ui/surface.h"
#include "ui/thumbnail_thread.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <winrt/base.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace atrium {

class StageManager {
public:
    StageManager(HINSTANCE instance, TaskModel& tasks);
    ~StageManager();
    StageManager(const StageManager&) = delete;
    StageManager& operator=(const StageManager&) = delete;

    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
    void SetOnRight(bool onRight);
    void SetMonitor(HMONITOR monitor);
    // The task model changed (every group, also those the bar does not show).
    void Update(const std::vector<TaskItem>& items);
    // Everything put away comes back (on exit).
    void RestoreAll();
    // The room the strip takes at the side of the screen (DIP), 0 while off: widgets keep clear of it.
    float Reserve() const;
    bool OnRight() const { return onRight_; }

    std::function<void()> onReserve; // Reserve() or OnRight() changed

private:
    struct Parked {
        std::wstring key;
        RECT source{};    // the window without its invisible borders, in its own px; empty: all of it
        uint64_t seq = 0; // when it was put away
        ULONGLONG since = 0;
    };
    using Set = std::vector<std::wstring>; // apps that come and go together
    struct Entry {
        Set keys;
        std::wstring name;
        std::vector<HWND> windows; // front first, at most kStack
        std::vector<uint64_t> thumbs; // ThumbnailThread ids, back first registered
        std::vector<DWM_THUMBNAIL_PROPERTIES> shown; // as last given to DWM (unchanged: not again)
        D2D1_RECT_F rect{};  // the card where it is now, content DIP
        D2D1_RECT_F image{}; // the pictures' room
        motion::Follow hover;
        // Cards glide to their place (a spring) and come and go with a fade and a little scale.
        float y = 0, vy = 0, targetY = 0;
        float appear = 0, va = 0, targetAppear = 1;
    };

    void Stage(const Set& keys, bool add, HWND front, bool activate);
    void Park(HWND window, const std::wstring& key);
    void Unpark(const Set& keys, HWND front, bool activate);
    void Pick(const Set& keys, bool add);
    Set SetOf(const std::wstring& key) const; // the set in the strip `key` belongs to, or just it
    void Prune();
    void Save();
    void Load();
    int Capacity() const;

    // The strip.
    void Rebuild();
    void Layout();
    void Show();
    void Hide();
    void Paint(Canvas& c);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    void UpdateThumbs();
    void DropThumbs();
    void DropThumbs(Entry& e);
    RECT SourceOf(HWND window, SIZE size) const; // the part of the window's picture to show
    bool Animate(double nowMs); // steps the cards; true while any moves
    void UpdateRegion(bool moving); // clicks around the cards reach what is under them
    void Watch();          // the strip's timer: is it covered, does the pointer ask for it
    bool Covered() const;  // a window on the stage is where the strip goes
    RECT Zone() const;     // where the strip goes, to the screen's edge, px
    RECT WorkArea() const;
    int EntryAt(POINT content) const;

    TaskModel& tasks_;
    Surface surface_;
    ThumbnailThread thumbs_; // after surface_: its pictures go before the window they are in
    winrt::com_ptr<IVirtualDesktopManager> desktops_; // the strip shows the current virtual desktop's windows
    bool enabled_ = false, onRight_ = false;
    bool applied_ = false;             // the settings have said once whether it is on
    std::wstring saved_;               // stage.json as last written
    HWND raise_ = nullptr;             // to put back on top once the windows coming back have
    HMONITOR monitor_ = nullptr;
    std::vector<TaskItem> items_;
    Set stage_;                        // the apps on the stage
    std::vector<Set> recent_;          // waiting sets, most recent first
    std::unordered_map<HWND, Parked> parked_;
    uint64_t seq_ = 0;
    Set pending_;                      // picked in the strip, not in front yet
    ULONGLONG pendingUntil_ = 0;

    std::vector<Entry> entries_;
    std::vector<Entry> leaving_;       // cards fading out (picked, or their windows gone)
    double lastStep_ = 0;
    RECT region_{};                    // the window region last set, client px
    RECT rect_{};                      // the strip, screen px: room for Capacity() cards
    D2D1_SIZE_F size_{};               // DIP
    int hover_ = -1, pressed_ = -1;
    bool tracking_ = false, inside_ = false;
    bool peek_ = false;                // shown over the stage because the pointer asked
    ULONGLONG edgeSince_ = 0, leftAt_ = 0;
    float thumbAlpha_ = 0;
    double fadeStart_ = 0;
};

} // namespace atrium
