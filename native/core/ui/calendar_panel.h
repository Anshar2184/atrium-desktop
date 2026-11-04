// The calendar, opened from the bar's clock: a month on a piece of the bar's
// glass (the Chinese lunar date under each day, festivals in the accent
// colour), and below it a capsule shaped like the bar with the time and the
// selected day. Drawn by Atrium instead of the system's calendar flyout; the
// lunar calendar comes from Windows' ICU.
#pragma once

#include "motion.h"
#include "shell/lunar.h"
#include "ui/surface.h"

#include <map>
#include <string>
#include <vector>

namespace atrium {

class CalendarPanel {
public:
    explicit CalendarPanel(HINSTANCE instance);
    ~CalendarPanel();

    // anchor: the clock on screen; bar: the bar's glass shape.
    void Open(RECT anchor, RECT bar);
    void Close();
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void SetRadius(float dip) { radius_ = dip; surface_.SetRadius(dip); }
    void SetBarHeight(float dip) { barH_ = dip; }
    HWND Hwnd() const { return surface_.Hwnd(); }

private:
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    struct Day {
        int y, m, d;
        std::wstring lunar; // the lunar day, or the festival
        bool festival = false;
    };
    void Paint(Canvas& c);
    void PaintMonth(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintCapsule(Canvas& c, const Theme& t, D2D1_RECT_F cap);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::wstring HitAt(POINT client) const;
    void Activate(const std::wstring& id);
    void ShowMonth(int y, int m, int direction);
    void Place();
    float Hover(Canvas& c, const std::wstring& id, bool on);
    D2D1_SIZE_F Size() const;
    Surface surface_;
    ChineseCalendar lunar_;
    int firstWeekday_ = 0; // 0 Monday .. 6 Sunday
    int year_ = 0, month_ = 0;      // shown
    SYSTEMTIME selected_{};          // the selected day (today when opened)
    std::vector<Day> days_;          // 42 cells from the first shown day
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_;
    bool tracking_ = false;
    RECT anchor_{}, bar_{};
    float radius_ = 30, barH_ = 60;
    ULONGLONG lastClosed_ = 0;
    std::map<std::wstring, motion::Follow> hoverAnim_;
    double monthMs_ = 0;
    int monthDir_ = 0;
};

} // namespace atrium
