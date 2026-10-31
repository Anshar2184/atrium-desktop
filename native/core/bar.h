// The floating glass bar (coexists with the system taskbar: it sits above the
// work area and never hides or replaces Explorer's taskbar).
//
// Two top-level windows (D08): a click-through layered visual window that
// DirectComposition renders into, and an invisible input window directly below
// it whose rounded region receives the clicks. Both are excluded from capture,
// so the monitor capture shows what lies underneath (D01).
#pragma once

#include "render/bar_painter.h"
#include "settings.h"
#include "shell/task_model.h"

#include <windows.h>
#include <dwrite_3.h>
#include <ole2.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atrium {

class Bar {
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
    bool Visible() const { return visible_; }

    std::function<IconImage(const std::wstring& key, int px)> iconProvider;
    std::function<void(const std::wstring& key)> onActivate;  // left click on an app
    std::function<void(const std::wstring& key)> onLaunch;    // middle click on an app
    std::function<void(const std::wstring& key, POINT)> onItemMenu; // right click on an app
    std::function<void(POINT)> onContextMenu;                 // right click elsewhere on the bar
    std::function<void()> onClockClick;
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

    bool debugCapturable_ = false;
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
    RECT applied_{};      // last window rect given to SetWindowPos
    int appliedRgn_[4]{}; // bar width, height, radius and inset of the last region
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
