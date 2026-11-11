#include "shell/lunar.h"

#include "localization.h"
#include "log.h"

#include <windows.h>
#include <icu.h>

#include <algorithm>

#pragma comment(lib, "icu.lib")

namespace atrium {

namespace {

const wchar_t* const kLunarMonths[] = {L"正", L"二", L"三", L"四", L"五", L"六", L"七", L"八", L"九", L"十", L"冬", L"腊"};

std::wstring LunarDay(int d)
{
    static const wchar_t* const digits[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"七", L"八", L"九", L"十"};
    if (d == 10)
        return L"初十";
    if (d == 20)
        return L"二十";
    if (d == 30)
        return L"三十";
    const wchar_t* tens = d < 10 ? L"初" : d < 20 ? L"十" : d < 30 ? L"廿" : L"三";
    return std::wstring(tens) + digits[(d - 1) % 10];
}

std::wstring LunarMonth(int m, bool leap) { return std::wstring(leap ? L"闰" : L"") + kLunarMonths[std::clamp(m, 1, 12) - 1] + L"月"; }

} // namespace

int DaysIn(int y, int m)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

int Weekday(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y -= 1;
    int sunday0 = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7; // 0 Sunday
    return (sunday0 + 6) % 7;
}

ChineseCalendar::ChineseCalendar()
{
    UErrorCode e = U_ZERO_ERROR;
    gregorian_ = ucal_open(nullptr, 0, "en_US@calendar=gregorian", UCAL_GREGORIAN, &e);
    if (U_FAILURE(e))
        gregorian_ = nullptr;
    e = U_ZERO_ERROR;
    chinese_ = ucal_open(nullptr, 0, "zh_CN@calendar=chinese", UCAL_DEFAULT, &e);
    if (U_FAILURE(e))
        chinese_ = nullptr;
    if (!Available())
        Log(L"calendar: ICU unavailable, no lunar dates");
}

ChineseCalendar::~ChineseCalendar()
{
    if (gregorian_)
        ucal_close(static_cast<UCalendar*>(gregorian_));
    if (chinese_)
        ucal_close(static_cast<UCalendar*>(chinese_));
}

bool ChineseCalendar::Lunar(int y, int m, int d, int& lm, int& ld, bool& leap, int& cycleYear) const
{
    if (!Available())
        return false;
    UCalendar* g = static_cast<UCalendar*>(gregorian_);
    UCalendar* c = static_cast<UCalendar*>(chinese_);
    UErrorCode e = U_ZERO_ERROR;
    ucal_clear(g);
    ucal_setDateTime(g, y, m - 1, d, 12, 0, 0, &e);
    UDate t = ucal_getMillis(g, &e);
    ucal_setMillis(c, t, &e);
    lm = ucal_get(c, UCAL_MONTH, &e) + 1;
    ld = ucal_get(c, UCAL_DATE, &e);
    leap = ucal_get(c, UCAL_IS_LEAP_MONTH, &e) != 0;
    cycleYear = ucal_get(c, UCAL_YEAR, &e);
    return U_SUCCESS(e);
}

std::wstring ChineseCalendar::Line(int y, int m, int d) const
{
    int lm = 0, ld = 0, cy = 0;
    bool leap = false;
    if (!Lunar(y, m, d, lm, ld, leap, cy))
        return {};
    if (LanguageIsEnglish())
        return L"Lunar " + std::to_wstring(lm) + L"/" + std::to_wstring(ld);
    static const wchar_t stems[] = L"甲乙丙丁戊己庚辛壬癸";
    static const wchar_t branches[] = L"子丑寅卯辰巳午未申酉戌亥";
    std::wstring year;
    if (cy >= 1)
        year = std::wstring(1, stems[(cy - 1) % 10]) + branches[(cy - 1) % 12] + L"年 ";
    return L"农历" + year + LunarMonth(lm, leap) + LunarDay(ld);
}

std::wstring ChineseCalendar::MonthDay(int y, int m, int d) const
{
    int lm = 0, ld = 0, cy = 0;
    bool leap = false;
    if (!Lunar(y, m, d, lm, ld, leap, cy))
        return {};
    return LanguageIsEnglish() ? L"Lunar " + std::to_wstring(lm) + L"/" + std::to_wstring(ld) : LunarMonth(lm, leap) + LunarDay(ld);
}

std::wstring ChineseCalendar::DayLabel(int y, int m, int d, bool* festival) const
{
    auto mark = [&](const wchar_t* name) {
        if (festival)
            *festival = true;
        return std::wstring(LanguageIsEnglish() ? Tr(name) : name);
    };
    if (festival)
        *festival = false;
    if (m == 1 && d == 1)
        return mark(L"元旦");
    if (m == 5 && d == 1)
        return mark(L"劳动节");
    if (m == 10 && d == 1)
        return mark(L"国庆节");
    int lm = 0, ld = 0, cyc = 0;
    bool leap = false;
    if (!Lunar(y, m, d, lm, ld, leap, cyc))
        return {};
    // The lunar festivals, then the day (the month's name on its first day).
    struct F {
        int m, d;
        const wchar_t* name;
    };
    static const F festivals[] = {{1, 1, L"春节"}, {1, 15, L"元宵节"}, {5, 5, L"端午节"}, {7, 7, L"七夕"},
                                  {8, 15, L"中秋节"}, {9, 9, L"重阳节"}, {12, 8, L"腊八节"}};
    for (const F& f : festivals)
        if (!leap && f.m == lm && f.d == ld)
            return mark(f.name);
    if (lm == 12 && !leap) {
        // New Year's Eve: the day before the first of the first month.
        int ny = y, nm = m, nd = d + 1;
        if (nd > DaysIn(ny, nm))
            nd = 1, nm = nm == 12 ? 1 : nm + 1, ny = nm == 1 ? ny + 1 : ny;
        int lm2 = 0, ld2 = 0, c2 = 0;
        bool leap2 = false;
        if (Lunar(ny, nm, nd, lm2, ld2, leap2, c2) && lm2 == 1 && ld2 == 1)
            return mark(L"除夕");
    }
    if (LanguageIsEnglish())
        return {}; // no lunar day under the Gregorian one; the festivals above still show
    return ld == 1 ? LunarMonth(lm, leap) : LunarDay(ld);
}

} // namespace atrium
