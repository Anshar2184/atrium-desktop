// Atrium's tray panel: the notification area's icons (TrayModel) in a grid on
// the bar's glass, above the bar's tray entry. A click is the icon's left
// click, a right click opens the app's own menu; the name of the icon under
// the pointer shows below the grid. A button there opens the system's own
// hidden-icons window as a fallback.
#pragma once

#include "shell/tray_model.h"
#include "ui/surface.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace atrium {

class TrayPanel {
public:
    TrayPanel(HINSTANCE instance, TrayModel& model);

    // anchor: the tray entry on screen; bar: the bar's glass shape.
    // previous: the window that had the focus before (it gets it back if a click leaves it nowhere).
    void Open(RECT anchor, RECT bar, HWND previous);
    HWND Previous() const { return previous_; }
    void Close();
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // the model has a new list
    void SetRadius(float dip) { radius_ = dip; surface_.SetRadius(dip); }
    HWND Hwnd() const { return surface_.Hwnd(); }
    // Test hooks: log the icons and their places (screen px); click the icon
    // at an index, or the first whose tooltip contains `which`.
    void LogState() const;
    void TestClick(const std::wstring& which, bool right);
    void LogFind(const std::wstring& which) const; // where the first icon whose tooltip contains `which` is

    std::function<void()> onSystemTray; // the fallback button

private:
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    void Paint(Canvas& c);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    void Place();
    std::wstring HitAt(POINT client) const;
    void Activate(const std::wstring& id, bool right);
    float Hover(Canvas& c, const std::wstring& id, bool on);
    D2D1_SIZE_F Size() const; // content, DIP
    int Cols() const;
    float Margin() const; // DIP: from the panel's edges to what is seen inside
    D2D1_RECT_F CellRect(size_t i) const; // icon i's cell, DIP

    Surface surface_;
    TrayModel& model_;
    std::vector<TrayIcon> icons_;
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_;
    bool tracking_ = false;
    RECT anchor_{}, bar_{};
    HWND previous_ = nullptr;
    float radius_ = 30;
    ULONGLONG lastClosed_ = 0;
    bool private_ = false;
    std::map<std::wstring, std::pair<float, double>> hoverAnim_;
    double openedMs_ = 0;
    std::wstring rippleId_;
    float rippleX_ = 0, rippleY_ = 0;
    double rippleMs_ = 0;
};

} // namespace atrium
