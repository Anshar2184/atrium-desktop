// Dates for the calendar panel and the calendar widgets: weekdays, month
// lengths and the Chinese lunar calendar (from Windows' ICU), with the
// festivals the calendar marks.
#pragma once

#include <string>

namespace atrium {

int DaysIn(int y, int m);
int Weekday(int y, int m, int d); // 0 Monday .. 6 Sunday

class ChineseCalendar {
public:
    ChineseCalendar();
    ~ChineseCalendar();
    ChineseCalendar(const ChineseCalendar&) = delete;
    ChineseCalendar& operator=(const ChineseCalendar&) = delete;

    bool Available() const { return gregorian_ && chinese_; }
    // The lunar date of a day: month (1-12), day, leap month, year of the cycle; false without ICU.
    bool Lunar(int y, int m, int d, int& lm, int& ld, bool& leap, int& cycleYear) const;
    std::wstring Line(int y, int m, int d) const;     // "农历丙午年 九月廿三"
    std::wstring MonthDay(int y, int m, int d) const; // "九月廿三"
    // What a day's cell shows: a festival (*festival true), else the lunar
    // day (the month's name on its first day); empty without ICU.
    std::wstring DayLabel(int y, int m, int d, bool* festival = nullptr) const;

private:
    void* gregorian_ = nullptr; // ICU UCalendar*
    void* chinese_ = nullptr;
};

} // namespace atrium
