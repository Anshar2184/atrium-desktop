#include "ui/widget_faces.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace atrium {

namespace {

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// The colours of a widget: the glass's ink, and macOS's accents.
struct Pal {
    D2D1_COLOR_F text, sub, faint, face, red, orange, green, track, blue; // blue: the calendar's accent
};

Pal GlassPal(const Theme& t)
{
    return {t.text, t.subtext, t.faint, Alpha(t.text, 0.10f), Rgba(0xFF453A, 0.98f), Rgba(0xFF9F0A, 0.98f), Rgba(0x30D158, 0.98f),
            Alpha(t.text, 0.18f), Rgba(0x0A84FF, 0.98f)};
}

const wchar_t* const kWeekLong[] = {L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六", L"星期日"};
const wchar_t* const kWeekShort[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
const wchar_t* const kMonths[] = {L"一月", L"二月", L"三月", L"四月", L"五月", L"六月",
                                  L"七月", L"八月", L"九月", L"十月", L"十一月", L"十二月"};

// The world clock's cities (macOS shows four in a medium Clock widget).
struct City {
    const wchar_t* name;
    const wchar_t* zone;
};
const City kCities[] = {{L"北京", L"China Standard Time"}, {L"东京", L"Tokyo Standard Time"}, {L"伦敦", L"GMT Standard Time"}, {L"纽约", L"Eastern Standard Time"}};

bool ZoneInfo(const wchar_t* key, DYNAMIC_TIME_ZONE_INFORMATION& out)
{
    static std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION> zones; // read once
    if (zones.empty()) {
        DYNAMIC_TIME_ZONE_INFORMATION d{};
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; ++i)
            zones[d.TimeZoneKeyName] = d;
    }
    auto it = zones.find(key);
    if (it == zones.end())
        return false;
    out = it->second;
    return true;
}

int64_t Minutes(const SYSTEMTIME& t)
{
    FILETIME f{};
    SystemTimeToFileTime(&t, &f);
    return int64_t((ULARGE_INTEGER{f.dwLowDateTime, f.dwHighDateTime}.QuadPart) / 600000000ULL);
}

// The time in a city, and how far it is from here: "今天 +1小时".
SYSTEMTIME CityTime(const City& city, const SYSTEMTIME& now, std::wstring& when)
{
    SYSTEMTIME utc, ct = now;
    GetSystemTime(&utc);
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    if (ZoneInfo(city.zone, z))
        SystemTimeToTzSpecificLocalTimeEx(&z, &utc, &ct);
    int64_t diff = (Minutes(ct) - Minutes(now)) / 60;
    int dayDiff = int((Minutes(ct) / 1440) - (Minutes(now) / 1440));
    when = dayDiff > 0 ? L"明天" : dayDiff < 0 ? L"昨天" : L"今天";
    when += (diff >= 0 ? L" +" : L" ") + std::to_wstring(diff) + L"小时";
    return ct;
}

std::wstring HourMinute(const SYSTEMTIME& t)
{
    wchar_t b[16];
    swprintf_s(b, L"%d:%02d", t.wHour, t.wMinute);
    return b;
}

int FirstWeekday()
{
    DWORD fd = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&fd), 2))
        return int(fd % 7);
    return 0;
}

// An analogue face: hands as on macOS (a thin neck, then the blade), the
// second hand in orange over the others. numbers: 1-12 inside the rim, else ticks.
void ClockFace(Canvas& c, float cx, float cy, float R, const SYSTEMTIME& t, const Pal& p, D2D1_COLOR_F face, D2D1_COLOR_F ink, bool numbers,
               const std::wstring& label, D2D1_COLOR_F labelInk)
{
    constexpr float kDeg = 3.14159265f / 180.f;
    c.Circle(cx, cy, R, face);
    for (int h = 1; h <= 12; ++h) {
        float a = (float(h) * 30.f - 90.f) * kDeg;
        if (numbers) {
            float r = R * 0.78f, fs = R * 0.21f;
            std::wstring n = std::to_wstring(h);
            c.Text(n, D2D1::RectF(cx + r * std::cos(a) - fs, cy + r * std::sin(a) - fs * 0.72f, cx + r * std::cos(a) + fs, cy + r * std::sin(a) + fs * 0.72f), fs,
                   ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        } else {
            float r0 = R * 0.80f, r1 = R * 0.92f;
            c.Capsule(cx + r0 * std::cos(a), cy + r0 * std::sin(a), cx + r1 * std::cos(a), cy + r1 * std::sin(a), Alpha(ink, h % 3 ? 0.45f : 0.9f),
                      std::max(1.f, R * 0.045f));
        }
    }
    if (!label.empty())
        c.Text(label, D2D1::RectF(cx - R, cy + R * 0.26f, cx + R, cy + R * 0.26f + R * 0.2f), R * 0.15f, labelInk, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    float sec = float(t.wSecond), min = float(t.wMinute) + sec / 60.f, hour = float(t.wHour % 12) + min / 60.f;
    auto hand = [&](float angle, float length, float width) {
        float a = (angle - 90.f) * kDeg, neck = R * 0.16f;
        float dx = std::cos(a), dy = std::sin(a);
        c.Capsule(cx, cy, cx + dx * neck, cy + dy * neck, ink, std::max(1.2f, width * 0.36f));
        c.Capsule(cx + dx * neck, cy + dy * neck, cx + dx * length, cy + dy * length, ink, width);
    };
    hand(hour * 30.f, R * 0.52f, std::max(2.5f, R * 0.075f));
    hand(min * 6.f, R * 0.80f, std::max(2.5f, R * 0.075f));
    c.Circle(cx, cy, std::max(2.2f, R * 0.055f), ink);
    float a = (sec * 6.f - 90.f) * kDeg;
    c.Capsule(cx - std::cos(a) * R * 0.18f, cy - std::sin(a) * R * 0.18f, cx + std::cos(a) * R * 0.86f, cy + std::sin(a) * R * 0.86f, p.orange,
              std::max(1.f, R * 0.02f));
    c.Circle(cx, cy, std::max(1.8f, R * 0.04f), p.orange);
    c.Circle(cx, cy, std::max(0.8f, R * 0.016f), face);
}

constexpr float kPad = 14;

void PaintClock(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    if (size == WidgetSize::Small) {
        ClockFace(c, W / 2, H / 2, H / 2 - 9, w.now, p, p.face, p.text, true, w.city, p.sub);
        return;
    }
    // The world clock: four faces, dark where it is night.
    const float colW = (W - 2 * kPad) / 4, R = 31;
    for (int i = 0; i < 4; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(kCities[i], w.now, when);
        bool night = ct.wHour < 6 || ct.wHour >= 18;
        float cx = kPad + colW * (float(i) + 0.5f), cy = (H - 2 * R - 42) / 2 + R - 2;
        D2D1_COLOR_F face = night ? Rgba(0x000000, 0.42f) : p.face, ink = night ? Rgba(0xFFFFFF, 0.96f) : p.text;
        ClockFace(c, cx, cy, R, ct, p, face, ink, false, {}, ink);
        c.Text(kCities[i].name, D2D1::RectF(cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 12.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(when, D2D1::RectF(cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// Digits instead of hands: the time large, the day under it; the medium one
// lists three more cities on the right.
void PaintDigital(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const int wd = Weekday(w.now.wYear, w.now.wMonth, w.now.wDay);
    const float leftW = size == WidgetSize::Small ? W - 2 * kPad : 150;
    c.Text(w.city.empty() ? L"本地" : w.city, D2D1::RectF(kPad + 2, kPad, kPad + leftW, kPad + 18), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    // The time fits its column with the same margin on both sides: sized for the widest it can be
    // (figures are tabular, so "00:00" is), centred, never clipped at 23:59.
    const float fit = c.TextWidth(L"00:00", 50, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float timeSize = fit > leftW ? 50 * leftW / fit : 50.f;
    c.Text(HourMinute(w.now), D2D1::RectF(kPad, kPad + 22, kPad + leftW, kPad + 84), timeSize, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(kWeekLong[wd], D2D1::RectF(kPad + 2, H - kPad - 40, kPad + leftW, H - kPad - 20), 13, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(w.now.wMonth) + L"月" + std::to_wstring(w.now.wDay) + L"日", D2D1::RectF(kPad + 2, H - kPad - 20, kPad + leftW, H - kPad), 13,
           p.text, DWRITE_FONT_WEIGHT_NORMAL);
    if (size == WidgetSize::Small)
        return;
    const float x0 = kPad + leftW + 8, rowH = (H - 2 * kPad) / 3;
    for (int i = 1; i < 4; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(kCities[i], w.now, when);
        float y = kPad + rowH * float(i - 1);
        if (i > 1)
            c.Line(x0, y, W - kPad, y, Alpha(p.text, 0.12f));
        c.Text(kCities[i].name, D2D1::RectF(x0, y + 4, W - kPad - 70, y + rowH / 2 + 2), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(when, D2D1::RectF(x0, y + rowH / 2, W - kPad - 70, y + rowH - 4), 10.5f, p.sub);
        c.Text(HourMinute(ct), D2D1::RectF(W - kPad - 80, y, W - kPad, y + rowH), 22, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

// The day: the weekday in red, the date large, the lunar date (a festival in
// red); the medium one adds the month, the large one is the month with the
// lunar day under every date.
void PaintCalendar(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const SYSTEMTIME& now = w.now;
    const int first = FirstWeekday();
    const int lead = (Weekday(now.wYear, now.wMonth, 1) - first + 7) % 7, days = DaysIn(now.wYear, now.wMonth);
    if (size == WidgetSize::Large) {
        c.Text(kMonths[now.wMonth - 1], D2D1::RectF(kPad + 4, kPad, W / 2, kPad + 22), 16, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring lunarToday = w.lunar ? w.lunar->MonthDay(now.wYear, now.wMonth, now.wDay) : std::wstring();
        c.Text(lunarToday.empty() ? std::to_wstring(now.wYear) + L"年" : std::to_wstring(now.wYear) + L"年 · 农历" + lunarToday,
               D2D1::RectF(W / 2 - 40, kPad, W - kPad - 4, kPad + 22), 11.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 2);
        const float gx = kPad, gw = W - 2 * kPad, cw = gw / 7;
        float y = kPad + 28;
        for (int i = 0; i < 7; ++i)
            c.Text(kWeekShort[(first + i) % 7], D2D1::RectF(gx + float(i) * cw, y, gx + float(i + 1) * cw, y + 16), 10.5f, p.sub,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        y += 20;
        const int rows = (lead + days + 6) / 7;
        const float rh = (H - kPad - y) / float(std::max(rows, 5));
        for (int d = 1; d <= days; ++d) {
            int cell = lead + d - 1;
            float x0 = gx + float(cell % 7) * cw, y0 = y + float(cell / 7) * rh, cx = x0 + cw / 2;
            bool today = d == now.wDay, weekend = (first + cell % 7) % 7 >= 5;
            if (today)
                c.Fill(D2D1::RectF(cx - 18, y0 + 1, cx + 18, y0 + rh - 1), 10, p.blue);
            D2D1_COLOR_F ink = today ? Rgba(0xFFFFFF, 0.98f) : weekend ? p.sub : p.text;
            c.Text(std::to_wstring(d), D2D1::RectF(x0, y0, x0 + cw, y0 + rh * 0.56f), 14, ink, today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD,
                   1);
            bool festival = false;
            std::wstring label = w.lunar ? w.lunar->DayLabel(now.wYear, now.wMonth, WORD(d), &festival) : std::wstring();
            c.Text(label, D2D1::RectF(x0, y0 + rh * 0.48f, x0 + cw, y0 + rh - 5), 9, today ? Alpha(Rgba(0xFFFFFF, 0.98f), 0.85f) : festival ? p.blue : p.sub,
                   DWRITE_FONT_WEIGHT_NORMAL, 1);
        }
        return;
    }
    const int wd = Weekday(now.wYear, now.wMonth, now.wDay);
    const float leftW = size == WidgetSize::Small ? W - 2 * kPad : 126;
    c.Text(kWeekLong[wd], D2D1::RectF(kPad + 2, kPad, kPad + leftW, kPad + 20), 14, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(now.wDay), D2D1::RectF(kPad, kPad + 16, kPad + leftW, kPad + 80), 54, p.text, DWRITE_FONT_WEIGHT_NORMAL);
    bool festival = false;
    std::wstring label = w.lunar ? w.lunar->DayLabel(now.wYear, now.wMonth, now.wDay, &festival) : std::wstring();
    std::wstring lunar = w.lunar ? w.lunar->MonthDay(now.wYear, now.wMonth, now.wDay) : std::wstring();
    c.Text(lunar.empty() ? std::wstring(kMonths[now.wMonth - 1]) : L"农历" + lunar, D2D1::RectF(kPad + 2, H - kPad - 40, kPad + leftW, H - kPad - 20), 12.5f,
           p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(festival ? label : std::to_wstring(now.wYear) + L"年" + kMonths[now.wMonth - 1], D2D1::RectF(kPad + 2, H - kPad - 20, kPad + leftW, H - kPad), 12,
           festival ? p.blue : p.sub, DWRITE_FONT_WEIGHT_NORMAL);
    if (size == WidgetSize::Small)
        return;
    // The month: weekday initials, the days, today in a red disc.
    const float gx = kPad + leftW + 6, gw = W - kPad - gx, cw = gw / 7;
    c.Text(kMonths[now.wMonth - 1], D2D1::RectF(gx + 4, kPad - 1, gx + gw, kPad + 17), 12.5f, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float y = kPad + 20;
    for (int i = 0; i < 7; ++i)
        c.Text(kWeekShort[(first + i) % 7], D2D1::RectF(gx + float(i) * cw, y, gx + float(i + 1) * cw, y + 14), 9.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    y += 16;
    int rows = (lead + days + 6) / 7;
    float rh = std::min(17.f, (H - kPad - y) / float(rows));
    for (int d = 1; d <= days; ++d) {
        int cell = lead + d - 1;
        float x0 = gx + float(cell % 7) * cw, y0 = y + float(cell / 7) * rh, cx = x0 + cw / 2, cy = y0 + rh / 2;
        bool today = d == now.wDay;
        if (today)
            c.Circle(cx, cy, rh / 2 + 0.5f, p.blue);
        bool weekend = (first + cell % 7) % 7 >= 5;
        D2D1_COLOR_F ink = today ? Rgba(0xFFFFFF, 0.98f) : weekend ? p.sub : p.text;
        // MiSans centres its figures in the line box (ascent 1.044, descent 0.282, figures 0.76
        // high): the box centred on the disc centres the digits.
        c.Text(std::to_wstring(d), D2D1::RectF(x0, cy - rh / 2, x0 + cw, cy + rh / 2), 10.5f, ink,
               today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
}

// Batteries: a ring round the computer, the level under it.
void PaintBattery(Canvas& c, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const float cx = W / 2, cy = H / 2 - 12, R = 44;
    c.Arc(cx, cy, R, -90, 270, p.track, 9);
    if (w.status.battery) {
        int pct = std::clamp(w.status.batteryPercent, 0, 100);
        D2D1_COLOR_F level = pct <= 20 && !w.status.charging ? p.red : p.green;
        c.Arc(cx, cy, R, -90, -90 + 360.f * float(pct) / 100.f, level, 9);
        c.Glyph(L"\uE7F8", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 24, p.text);
        // The level, with a bolt before it while charging.
        std::wstring n = std::to_wstring(pct) + L"%";
        float tw = c.TextWidth(n, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD), bolt = w.status.charging ? 16.f : 0.f, x0 = (W - tw - bolt) / 2;
        if (w.status.charging)
            c.Glyph(L"\uE945", D2D1::RectF(x0 - 2, cy + R + 10, x0 + bolt, cy + R + 36), 13, level);
        c.Text(n, D2D1::RectF(x0 + bolt, cy + R + 10, x0 + bolt + tw + 4, cy + R + 36), 19, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    } else {
        c.Arc(cx, cy, R, -90, 270, p.green, 9);
        c.Glyph(L"\uE83E", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 22, p.text);
        c.Text(L"已接通电源", D2D1::RectF(0, cy + R + 12, W, cy + R + 34), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    (void)H;
}

// The processor, the memory and the system disk: bars (small) or rings
// (medium), green, orange from 70 %, red from 90 %.
void PaintSystem(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    struct Gauge {
        const wchar_t* name;
        float value;
        std::wstring detail;
    };
    wchar_t mem[32], disk[32];
    swprintf_s(mem, L"%.1f / %.0f GB", w.memoryGb, w.memoryTotalGb);
    swprintf_s(disk, L"%.0f GB 可用", w.diskFreeGb);
    const Gauge gauges[] = {{L"CPU", w.cpu, L"处理器"}, {L"内存", w.memory, mem}, {L"磁盘", w.disk, disk}};
    auto colour = [&](float v) { return v >= 0.9f ? p.red : v >= 0.7f ? p.orange : p.green; };
    auto pct = [](float v) { return std::to_wstring(int(std::lround(std::clamp(v, 0.f, 1.f) * 100))) + L"%"; };
    if (size == WidgetSize::Small) {
        c.Text(L"系统", D2D1::RectF(kPad + 2, kPad - 2, W - kPad, kPad + 16), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float rowH = (H - 2 * kPad - 18) / 3;
        for (int i = 0; i < 3; ++i) {
            float y = kPad + 18 + rowH * float(i);
            c.Text(gauges[i].name, D2D1::RectF(kPad + 2, y + 2, W / 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(pct(gauges[i].value), D2D1::RectF(W / 2, y + 2, W - kPad - 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
            D2D1_RECT_F track = D2D1::RectF(kPad + 2, y + 26, W - kPad - 2, y + 32);
            c.Fill(track, 3, p.track);
            float v = std::clamp(gauges[i].value, 0.f, 1.f);
            if (v > 0.01f)
                c.Fill(D2D1::RectF(track.left, track.top, track.left + std::max(6.f, (track.right - track.left) * v), track.bottom), 3, colour(v));
        }
        return;
    }
    const float colW = (W - 2 * kPad) / 3, R = 34, cy = kPad + 6 + R;
    for (int i = 0; i < 3; ++i) {
        float cx = kPad + colW * (float(i) + 0.5f), v = std::clamp(gauges[i].value, 0.f, 1.f);
        c.Arc(cx, cy, R, -90, 270, p.track, 8);
        if (v > 0.005f)
            c.Arc(cx, cy, R, -90, -90 + 360.f * v, colour(v), 8);
        c.Text(pct(v), D2D1::RectF(cx - R, cy - 12, cx + R, cy + 12), 17, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].name, D2D1::RectF(cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].detail, D2D1::RectF(cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// A note: its title, then the text; while editing, the caret, and the text
// scrolled up when the caret goes below the card.
void PaintNotes(Canvas& c, const WidgetContent& n, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    c.Text(L"备忘录", D2D1::RectF(kPad + 2, kPad - 2, W - kPad, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_RECT_F box = D2D1::RectF(kPad + 2, kPad + 24, W - kPad - 2, H - kPad);
    hits.push_back({D2D1::RectF(0, 0, W, H), L"text"});
    const float size = 13.5f;
    if (n.text.empty() && !n.editing) {
        c.Text(L"点按以记录…", box, size, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    if (!n.editing) {
        c.Text(n.text, box, size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    D2D1_RECT_F caret = c.Caret(n.text, box, size, DWRITE_FONT_WEIGHT_NORMAL, n.caret);
    float scroll = std::max(0.f, caret.bottom - box.bottom);
    c.Clip(box);
    c.Text(n.text, D2D1::RectF(box.left, box.top - scroll, box.right, box.top - scroll + 10000), size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    caret.top -= scroll;
    caret.bottom -= scroll;
    if (n.caretOn)
        c.Fill(caret, 0.75f, p.orange);
    c.Unclip();
    hits.push_back({caret, L"caret"});
}

// Reminders: the title and the count still to do, then the list
// in their original order, each with its circle; a last row adds one.
void PaintReminders(Canvas& c, const WidgetContent& r, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    int open = int(std::count_if(r.items.begin(), r.items.end(), [](const Reminder& i) { return !i.done; }));
    c.Text(L"提醒事项", D2D1::RectF(kPad + 2, kPad - 2, W - kPad - 40, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(open), D2D1::RectF(W - kPad - 48, kPad - 6, W - kPad - 2, kPad + 22), 22, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    const float bottom = H - kReminderBottom, view = bottom - kReminderTop;
    const float limit = std::max(0.f, float(r.items.size() + 1) * kReminderRow - view);
    const float scroll = std::clamp(r.scroll, 0.f, limit);
    auto hit = [&](D2D1_RECT_F rect, std::wstring id) {
        rect.top = std::max(rect.top, kReminderTop);
        rect.bottom = std::min(rect.bottom, bottom);
        if (rect.bottom > rect.top)
            hits.push_back({rect, std::move(id)});
    };
    c.Clip(D2D1::RectF(kPad - 4, kReminderTop, W - kPad, bottom));
    int first = int(scroll / kReminderRow);
    int last = std::min(int(r.items.size()), int(std::ceil((scroll + view) / kReminderRow)));
    for (int idx = first; idx < last; ++idx) {
        const Reminder& it = r.items[size_t(idx)];
        float y = kReminderTop + kReminderRow * float(idx) - scroll;
        float cx = kPad + 9, cy = y + kReminderRow / 2;
        if (it.done) {
            c.Circle(cx, cy, 8, p.orange);
            c.Circle(cx, cy, 3.2f, Rgba(0xFFFFFF, 0.98f));
        } else {
            c.Arc(cx, cy, 7.4f, -90, 270, p.sub, 1.5f);
        }
        hit(D2D1::RectF(kPad - 4, y, kPad + 22, y + kReminderRow), L"toggle:" + std::to_wstring(idx));
        D2D1_RECT_F text = D2D1::RectF(kPad + 26, y, W - kPad, y + kReminderRow);
        bool editing = r.editing && r.item == idx;
        c.Text(it.text.empty() && !editing ? L"新提醒" : it.text, text, 13.5f, it.done ? p.sub : it.text.empty() ? p.faint : p.text);
        if (it.done && !it.text.empty())
            c.Line(text.left, cy, std::min(text.right, text.left + c.TextWidth(it.text, 13.5f)), cy, p.sub);
        if (editing) {
            // One line: the caret after the text up to it.
            float x = text.left + c.TextWidth(it.text.substr(0, std::min(r.caret, it.text.size())), 13.5f);
            D2D1_RECT_F caret = D2D1::RectF(std::min(x, text.right - 2), cy - 9, std::min(x, text.right - 2) + 1.5f, cy + 9);
            if (r.caretOn)
                c.Fill(caret, 0.75f, p.orange);
            hit(caret, L"caret");
        }
        hit(text, L"item:" + std::to_wstring(idx));
    }
    if (!r.editing) {
        float y = kReminderTop + kReminderRow * float(r.items.size()) - scroll;
        if (y < bottom) {
            c.Text(L"+", D2D1::RectF(kPad - 1, y, kPad + 19, y + kReminderRow), 17, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 1);
            c.Text(r.items.empty() ? L"点按以添加提醒" : L"新提醒", D2D1::RectF(kPad + 26, y, W - kPad, y + kReminderRow), 13.5f, p.faint);
            hit(D2D1::RectF(kPad - 4, y, W - kPad, y + kReminderRow), L"add");
        }
    }
    c.Unclip();
    if (limit > 0) {
        float thumb = std::max(18.f, view * view / (view + limit));
        float y = kReminderTop + (view - thumb) * scroll / limit;
        c.Fill(D2D1::RectF(W - 7, y, W - 5, y + thumb), 1, p.faint);
    }
}

// A photo filling the card; without one, how to choose a folder.
void PaintPhotos(Canvas& c, const WidgetContent& ph, const Pal& p, float W, float H)
{
    if (ph.image && !ph.image->empty()) {
        c.ImageRounded(ph.imageKey, *ph.image, D2D1::RectF(0, 0, W, H), kWidgetRadius);
        return;
    }
    c.Glyph(L"\uE91B", D2D1::RectF(0, H / 2 - 46, W, H / 2 - 6), 30, p.sub);
    c.Text(L"照片", D2D1::RectF(0, H / 2, W, H / 2 + 20), 14, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"右键选择照片文件夹", D2D1::RectF(0, H / 2 + 20, W, H / 2 + 38), 11.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
}

} // namespace

float ReminderScrollMax(const WidgetContent& content, WidgetSize size)
{
    return std::max(0.f, float(content.items.size() + 1) * kReminderRow - (WidgetHeight(size) - kReminderTop - kReminderBottom));
}

bool WidgetSupports(WidgetKind kind, WidgetSize size)
{
    switch (kind) {
    case WidgetKind::Battery:
        return size == WidgetSize::Small;
    case WidgetKind::Clock:
    case WidgetKind::Digital:
    case WidgetKind::System:
        return size != WidgetSize::Large;
    default:
        return true;
    }
}

void PaintWidget(Canvas& c, WidgetKind kind, WidgetSize size, const WidgetContent& content, const WidgetWorld& world, std::vector<WidgetHit>& hits)
{
    const Pal p = GlassPal(c.theme);
    const float W = WidgetWidth(size), H = WidgetHeight(size);
    hits.clear();
    switch (kind) {
    case WidgetKind::Clock:
        PaintClock(c, size, world, p, W, H);
        break;
    case WidgetKind::Digital:
        PaintDigital(c, size, world, p, W, H);
        break;
    case WidgetKind::Calendar:
        PaintCalendar(c, size, world, p, W, H);
        break;
    case WidgetKind::Battery:
        PaintBattery(c, world, p, W, H);
        break;
    case WidgetKind::System:
        PaintSystem(c, size, world, p, W, H);
        break;
    case WidgetKind::Notes:
        PaintNotes(c, content, p, W, H, hits);
        break;
    case WidgetKind::Reminders:
        PaintReminders(c, content, p, W, H, hits);
        break;
    case WidgetKind::Photos:
        PaintPhotos(c, content, p, W, H);
        break;
    }
}

} // namespace atrium
