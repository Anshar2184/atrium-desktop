// The floating glass bar (coexists with the system taskbar: it sits above the
// work area and never hides or replaces Explorer's taskbar).
//
// Two top-level windows (D08): a click-through layered visual window that
// DirectComposition renders into, and an invisible input window directly below
// it whose rounded region receives the clicks. Both are excluded from capture,
// so the monitor capture shows what lies underneath (D01).
//
// Stacking (D25): both windows are topmost and are raised again when another
// window covers them (foreground changes, a move ends, a 1 s check); they stay
// below a revealed system taskbar and hide while a full-screen app is in front.
// The render thread also draws the glass layer of Atrium's popups.
#pragma once

#include "render/bar_painter.h"
#include "render/glass_panel.h"
#include "settings.h"
#include "shell/task_model.h"

#include <windows.h>
#include <dwrite_3.h>
#include <ole2.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atrium {

class Bar : public GlassHost {
public:
    // debugCapturable: development only. Leaves the bar visible to screen capture
    // and freezes the backdrop after the first frame so the glass never samples
    // itself; used to take screenshots of the real surface.
    // debugLayout: development only; logs the screen rectangle of every app
    // after each layout (used by tests/e2e-bar.ps1 to click real items).
    Bar(HINSTANCE instance, bool debugCapturable, bool debugLayout = false);
    ~Bar();
    Bar(const Bar&) = delete;
    Bar& operator=(const Bar&) = delete;

    // UI thread only.
    void Apply(const Settings& s);
    void SetItems(std::vector<TaskItem> items);
    void Relayout();
    void SetVisible(bool visible);
    // --debug-capturable: take the frozen backdrop again from the next frame
    // (the lock screen's wallpaper has just appeared).
    void Refreeze() { refreeze_ = true; }
    bool Visible() const { return visible_; }
    // Unread notifications, shown as a badge on the bell.
    void SetUnread(int count);
    // Network, battery and volume for the status icon.
    void SetStatus(const SystemStatus& status);
    // Screen rectangle of an entry ("#start", "#notify", an app key); empty if not shown.
    RECT EntryRect(const std::wstring& key) const;
    // Screen rectangle of the glass shape.
    RECT BarRect() const;
    // The window that had the foreground before a click on the logo activated the bar.
    HWND PreviousForeground() const { return previousForeground_; }
    // Glass layers of popup windows (GlassHost).
    void SetPanel(const GlassPanel& panel) override;
    // Launch feedback: the app's icon hops up to `hops` times (fewer once it runs).
    void Bounce(const std::wstring& key, int hops);
    // Test hook: logs the stacking state now and the next rendered frame.
    void LogState();

    std::function<IconImage(const std::wstring& key, int px)> iconProvider;
    std::function<void(const std::wstring& key)> onActivate;  // left click on an app
    std::function<void(const std::wstring& key)> onLaunch;    // middle click on an app
    std::function<void(const std::wstring& key, POINT)> onItemMenu; // right click on an app
    std::function<void(POINT)> onContextMenu;                 // right click elsewhere on the bar
    std::function<void()> onClockClick;
    // Bar entries; `anchor` is the entry's rectangle on screen (popups open above it).
    std::function<void(RECT anchor)> onStartClick;  // the Atrium logo
    std::function<void(RECT anchor)> onNotifyClick; // the notification bell
    std::function<void(RECT anchor)> onTrayClick;   // the tray entry (Atrium's tray panel)
    std::function<void(RECT anchor)> onStatusClick; // the status icon (Wi-Fi, battery: the control centre)
    std::function<void(int notches)> onStatusWheel; // the wheel over the status icon (volume)
    // The overflow entry: apps that did not fit, in bar order.
    std::function<void(const std::vector<std::wstring>& keys, RECT anchor)> onOverflow;
    // Drag and drop of an app to visible position `toIndex` (0-based, in the
    // list without the dragged item). The bar already shows the new order.
    std::function<void(const std::wstring& key, int toIndex)> onReorder;
    // Files dragged from Explorer: hovering an app brings it to the front,
    // dropping opens the files with it.
    std::function<void(const std::wstring& key)> onFileDragHover;
    std::function<bool(const std::wstring& key)> canOpenFiles;
    std::function<void(const std::wstring& key, const std::vector<std::wstring>& files)> onDropFiles;
    std::function<void()> onCloseRequest; // WM_CLOSE on a bar window (taskkill, logoff)

private:
    struct Snapshot {
        Settings settings;
        BarScene scene;
        RECT window{};     // visual window, screen px
        RECT monitor{};    // monitor rect, screen px
        HMONITOR hmonitor = nullptr;
        float scale = 1;
        bool visible = true;
        unsigned generation = 0;
        std::vector<GlassPanel> panels;
    };
    struct Hit {
        std::wstring key; // app key, L"#clock", or empty for the bar itself
        RECT rect{};
    };
    struct CachedIcon {
        uint64_t version = 0;
        int px = 0;
        uint64_t id = 0;
        std::shared_ptr<const IconImage> image;
    };

    static LRESULT CALLBACK VisualProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnInput(HWND h, UINT m, WPARAM w, LPARAM l);
    std::wstring HitTest(POINT client) const;
    void Publish(); // copies scene state to the render thread
    void UpdateTooltips();   // refreshes the text of a visible tooltip
    void HideTooltip();
    void ShowTooltip();      // above the hovered item, after a short delay
    std::wstring TooltipText(const std::wstring& key) const;
    void BeginDrag(POINT client);
    void UpdateDrag(POINT client);
    void EndDrag(bool commit);
    friend struct BarDropTarget;
    std::wstring FileDragOver(POINT screen); // returns the app under the point
    void FileDragLeave();
    void RenderThread();
    bool RenderSession(); // returns false to stop, true to rebuild (device lost, monitor changed)
    void SetRegion(bool extended); // input region: the shape, plus the room above it while magnified
    static void CALLBACK WinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD thread, DWORD time);
    void GuardStacking(const wchar_t* why);
    bool ForegroundIsFullscreen() const;
    void Raise();

    bool debugCapturable_ = false;
    std::atomic<bool> refreeze_{false};
    bool debugLayout_ = false;
    HWND visual_ = nullptr;
    HWND input_ = nullptr;
    HWND tooltip_ = nullptr;
    Settings settings_;
    std::vector<TaskItem> items_;
    std::map<std::wstring, CachedIcon> icons_;
    uint64_t nextIconId_ = 1;
    BarScene scene_;
    std::vector<Hit> hits_;
    bool tooltipShown_ = false;
    bool visible_ = true;
    bool tracking_ = false;
    std::wstring pressed_;
    bool pressedLeft_ = false;
    POINT pressPt_{};
    // Drag reorder (client px). The dragged item follows the pointer; the
    // others make room at `dragInsert_`.
    bool dragging_ = false;
    std::wstring dragKey_;
    int dragFrom_ = -1, dragInsert_ = -1;
    int dragX_ = 0, dragY_ = 0, grabDx_ = 0;
    // App strip metrics of the last layout (client px).
    int appsLeft_ = 0, pitch_ = 1, plate_ = 0, gap_ = 0, shown_ = 0;
    std::vector<std::wstring> hidden_; // apps behind the overflow entry
    int unread_ = 0;
    SystemStatus status_;
    bool belowTray_ = false;   // yielded to the system taskbar the user is using
    bool trayHeldUp_ = false;  // the system taskbar is up without the user (logged once)
    HWND previousForeground_ = nullptr;
    RECT applied_{};      // last window rect given to SetWindowPos
    int appliedRgn_[6]{}; // bar width, height, radius, insets and extension of the last region
    // Room around the shape inside the windows (px): sides (shadow plus the
    // magnified bar's stretch), top (shadow plus magnified icons), bottom.
    int insetX_ = 0, insetTop_ = 0, inset_ = 0;
    int rise_ = 0;        // how far magnified or hopping icons reach above the shape
    int radius_ = 0;
    bool regionExtended_ = false;
    std::map<std::wstring, std::pair<double, int>> bounces_; // key -> start (ClockMs), hops
    std::map<HWND, GlassPanel> panels_;
    // Stacking guard.
    HWINEVENTHOOK hooks_[2]{};
    bool fullscreen_ = false;
    std::map<HWND, std::pair<int, ULONGLONG>> raises_; // windows we were raised over: count, first time
    HWND lastCover_ = nullptr;
    std::atomic<bool> frameLog_{false}; // the render thread logs its next frame
    bool appliedVisible_ = false;
    std::wstring fileHoverKey_;
    winrt::com_ptr<IDropTarget> dropTarget_;
    bool oleReady_ = false;
    RECT window_{};
    RECT monitor_{};
    HMONITOR hmonitor_ = nullptr;
    float scale_ = 1;
    winrt::com_ptr<IDWriteFactory3> dwrite_; // UI-thread text measurement

    std::mutex lock_;
    Snapshot snapshot_;
    HANDLE wake_ = nullptr;  // snapshot changed
    HANDLE quit_ = nullptr;
    HANDLE frame_ = nullptr; // a capture frame arrived
    std::thread render_;
};

} // namespace atrium
