// The genie effect, as macOS minimizes (setting genieMinimize): a window going down pours into its icon on
// the bar, its sides bending towards the icon while the picture slides down into it, the rows nearest the
// bar first; one the bar restores pours back out of its icon the same way and dissolves into the window.
// Windows' own minimize animation is off for the window in front while this is on
// (DWMWA_TRANSITIONS_FORCEDISABLED), once a picture of it is at hand, and back on as soon as it is down,
// so restoring it some other way is Windows' own. A minimized window has nothing to show: the picture is
// kept fresh while it is in front (often while the pointer is near its caption buttons, where a minimize
// comes from), as Stage Manager keeps its pictures; the bar's own click takes a fresh one first. The
// effect's layer is on screen, empty and click-through, before a minimize can come, so the first frame
// is there as the window goes. Windows Stage Manager flies itself are left to it.
#pragma once

#include "ui/surface.h"
#include "ui/window_snapshot.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace atrium {

class GenieMinimize {
public:
    // target: the icon of the window's app on a bar (screen px), else none; skip: a window another
    // animation owns.
    GenieMinimize(HINSTANCE instance, std::function<std::optional<RECT>(HWND)> target, std::function<bool(HWND)> skip);
    ~GenieMinimize();
    GenieMinimize(const GenieMinimize&) = delete;
    GenieMinimize& operator=(const GenieMinimize&) = delete;

    void SetEnabled(bool on);
    void Minimizing(HWND window); // EVENT_SYSTEM_MINIMIZESTART
    // The bar's click (TaskModel::SetWindowSteps): `minimize` runs once a fresh picture of the window is at
    // hand; `restore` runs when the window has poured back out of its icon. False: not for the genie.
    bool Minimize(HWND window, std::function<void()> minimize);
    bool Restore(HWND window, std::function<void()> restore);

private:
    struct Picture {
        std::shared_ptr<const WindowPicture> large;
        ULONGLONG taken = 0; // asked for or taken
        RECT frame{};        // where the window was, screen px
    };
    struct Flight {
        std::shared_ptr<const WindowPicture> picture;
        std::wstring key;
        RECT from{}, to{}; // screen px
        double startMs = 0; // 0: from its first frame on screen
        HWND window = nullptr;
        std::function<void()> restore; // set: out of the icon, then this
        bool restoreDue = false;
        bool restoreTransitions = false;
        bool preLaunched = false; // launched before the window went down: Minimizing() must not re-launch it
    };
    void Watch();                                              // kWatchTimer: the window in front, its picture, its animations
    void Release(HWND window, bool restoreTransitions = true); // its own animations back (unless another owner took it)
    void Hold(HWND window);                                    // its own animations off
    RECT FlightBounds(const RECT& fallback) const;
    void Arm(const RECT& frame); // the layer on the flights' monitor(s), empty, for a while
    void Shutdown();
    void Launch(Flight f);
    void RunPending();            // the bar's minimize, its picture taken (or not in time)
    void RunRestores();           // flights out of the icon that have landed
    void Paint(Canvas& c);
    bool PaintOne(Canvas& c, Flight& f); // true: done
    void PaintFlight(Canvas& c, const Flight& f, float t, float alpha);

    Surface surface_; // over the screen while a window pours into the bar
    WindowSnapshots snapshots_;
    std::function<std::optional<RECT>(HWND)> target_;
    std::function<bool(HWND)> skip_;
    bool enabled_ = false;
    HWINEVENTHOOK hook_ = nullptr;
    HWND front_ = nullptr, held_ = nullptr; // the window in front; the one whose animations are off
    ULONGLONG frontSince_ = 0;
    std::unordered_map<HWND, Picture> pictures_;
    std::vector<Flight> flights_;
    unsigned flightGen_ = 0;
    HWND pending_ = nullptr; // the bar's minimize, waiting for its picture
    std::function<void()> pendingMinimize_;
    double eventMs_ = 0; // when the last minimize came, for the log
};

} // namespace atrium
