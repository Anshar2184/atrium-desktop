// Desktop widgets after macOS's (Sonoma and later): pieces of the bar's glass
// on the desktop, in a grid from the top left of the bar's monitor. They
// stand just above the desktop, under every window, always clear glass with
// its ink and a few colours of macOS's (the second hand, the weekday, the
// battery's ring); Win+D (every app minimised) lifts them over the desktop. Drag one to move it (it snaps to the grid); right-click for its
// sizes, to remove it or to add another. The layout is saved in
// %LOCALAPPDATA%\Atrium\widgets.json.
#pragma once

#include "shell/lunar.h"
#include "shell/system_status.h"
#include "ui/surface.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

enum class WidgetKind { Clock, Calendar, Battery };
enum class WidgetSize { Small, Medium };

class DesktopWidgets {
public:
    explicit DesktopWidgets(HINSTANCE instance);
    ~DesktopWidgets();
    DesktopWidgets(const DesktopWidgets&) = delete;
    DesktopWidgets& operator=(const DesktopWidgets&) = delete;

    void SetEnabled(bool on);
    // The bar's monitor: the widgets live in its work area, where its capture gives them glass.
    void SetMonitor(HMONITOR monitor);
    // Adds a widget in the first free place (the tray icon's menu, a widget's menu).
    void Add(WidgetKind kind, WidgetSize size);
    // Test hooks: log what is shown (screen px); "add:<kind>:<size>", "remove:<i>", "size:<i>:<size>", "restack".
    void LogState() const;
    void TestAction(const std::wstring& arg);

    std::function<void()> onCalendar; // a calendar widget was clicked

private:
    struct Widget;
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    void Build();
    void Clear();
    void Load();
    void Save() const;
    Widget& Create(WidgetKind kind, WidgetSize size, int col, int row);
    void Remove(Widget& w);
    void RemoveNow(Widget& w);
    void Resize(Widget& w, WidgetSize size);
    // The grid in the work area of the bar's monitor.
    float Scale() const;
    RECT WorkArea() const;
    int Cols() const;
    int Rows() const;
    RECT CellRect(int col, int row, WidgetSize size) const; // screen px
    bool Fits(int col, int row, WidgetSize size, const Widget* except) const;
    bool FreeCellNear(float col, float row, WidgetSize size, const Widget* except, int& outCol, int& outRow) const;
    void Place(Widget& w, bool glide);
    void Glide();
    void Restack();
    void Tick();
    bool IsOurs(HWND h) const;
    bool Message(Widget& w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r);
    void Menu(Widget& w);
    void Activate(Widget& w);
    void Paint(Widget& w, Canvas& c);

    HINSTANCE instance_;
    HWND host_ = nullptr; // timers, the menu's owner
    HWINEVENTHOOK hook_ = nullptr;
    bool enabled_ = false;
    HMONITOR monitor_ = nullptr;
    std::vector<std::unique_ptr<Widget>> widgets_;
    bool raised_ = false; // every app minimised: over the desktop
    SystemStatus status_;
    ChineseCalendar lunar_;
    std::wstring city_; // the local time zone's first city
    int lastMinute_ = -1;
    bool test_ = false; // ATRIUM_WIDGETS_TEST=1: over everything (screenshots)
    bool restackPosted_ = false;
};

} // namespace atrium
