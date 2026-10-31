// The floating glass bar (coexists with the system taskbar: it sits above the
// work area and never hides or replaces Explorer's taskbar).
//
// Two top-level windows (D08): a click-through layered visual window that
// DirectComposition renders into, and an invisible input window directly below
// it whose rounded region receives the clicks. Both are excluded from capture,
// so the monitor capture shows what lies underneath (D01).
#pragma once

#include "settings.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace atrium {

class Bar {
public:
    // debugCapturable: development only. Leaves the bar visible to screen capture
    // and freezes the backdrop after the first frame so the glass never samples
    // itself; used to take screenshots of the real surface.
    Bar(HINSTANCE instance, bool debugCapturable);
    ~Bar();
    Bar(const Bar&) = delete;
    Bar& operator=(const Bar&) = delete;

    // UI thread only.
    void Apply(const Settings& s);
    void Relayout();
    void SetVisible(bool visible);
    bool Visible() const { return visible_; }

    std::function<void(POINT)> onContextMenu; // right click on the bar (screen px)
    std::function<void()> onCloseRequest;     // WM_CLOSE on a bar window (taskkill, logoff)

private:
    struct Snapshot {
        Settings settings;
        RECT window{};     // visual window, screen px
        RECT monitor{};    // monitor rect, screen px
        HMONITOR hmonitor = nullptr;
        float scale = 1;
        bool visible = true;
        unsigned generation = 0;
    };

    static LRESULT CALLBACK VisualProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM);
    void RenderThread();
    bool RenderSession(); // returns false to stop, true to rebuild (device lost, monitor changed)

    bool debugCapturable_ = false;
    HWND visual_ = nullptr;
    HWND input_ = nullptr;
    Settings settings_;
    bool visible_ = true;

    std::mutex lock_;
    Snapshot snapshot_;
    HANDLE wake_ = nullptr;  // snapshot changed
    HANDLE quit_ = nullptr;
    HANDLE frame_ = nullptr; // a capture frame arrived
    std::thread render_;
};

} // namespace atrium
