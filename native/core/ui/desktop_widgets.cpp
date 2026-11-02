#include "ui/desktop_widgets.h"

#include "clock.h"
#include "log.h"
#include "settings.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <map>

#pragma comment(lib, "dwmapi.lib")

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

// macOS's widget grid: a small widget is a square, a medium one two squares wide.
constexpr float kCell = 164, kGap = 16, kMargin = 24, kRadius = 22;
constexpr double kGlideMs = 240;
constexpr UINT_PTR kTickTimer = 1, kGlideTimer = 3, kSafetyTimer = 4;
constexpr const wchar_t* kHostClass = L"AtriumWidgetsHost";
constexpr UINT WM_WIDGET_REMOVE = WM_APP + 1; // lParam Widget*: removed once its own message is done
constexpr UINT WM_WIDGET_RESTACK = WM_APP + 2; // a burst of window events, one restack

DesktopWidgets* g_widgets = nullptr; // the WinEvent hook's way back

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
float Ease(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

const wchar_t* KindName(WidgetKind k) { return k == WidgetKind::Clock ? L"clock" : k == WidgetKind::Calendar ? L"calendar" : L"battery"; }
const wchar_t* SizeName(WidgetSize s) { return s == WidgetSize::Small ? L"small" : L"medium"; }
bool ParseKind(const std::wstring& s, WidgetKind& k)
{
    if (s == L"clock")
        k = WidgetKind::Clock;
    else if (s == L"calendar")
        k = WidgetKind::Calendar;
    else if (s == L"battery")
        k = WidgetKind::Battery;
    else
        return false;
    return true;
}
WidgetSize ParseSize(const std::wstring& s) { return s == L"medium" ? WidgetSize::Medium : WidgetSize::Small; }
bool Supports(WidgetKind k, WidgetSize s) { return s == WidgetSize::Small || k != WidgetKind::Battery; }
int SpanOf(WidgetSize s) { return s == WidgetSize::Medium ? 2 : 1; }
float WidthOf(WidgetSize s) { return float(SpanOf(s)) * kCell + float(SpanOf(s) - 1) * kGap; }

std::wstring LayoutPath()
{
    std::wstring p = SettingsPath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\widgets.json";
}

std::wstring ClassOf(HWND h)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return cls;
}

bool IsDesktopWindow(HWND h)
{
    std::wstring c = ClassOf(h);
    return c == L"Progman" || c == L"WorkerW";
}

// The first city of the local time zone's name: "(UTC+08:00) 北京，重庆，…" -> "北京".
std::wstring LocalCity()
{
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID)
        return {};
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\" + std::wstring(tz.TimeZoneKeyName);
    wchar_t display[256] = {};
    DWORD size = sizeof(display);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Display", RRF_RT_REG_SZ, nullptr, display, &size) != ERROR_SUCCESS)
        return {};
    std::wstring d = display;
    size_t open = d.find(L") ");
    if (open != std::wstring::npos)
        d = d.substr(open + 2);
    size_t end = d.find_first_of(L"，,");
    return end == std::wstring::npos ? d : d.substr(0, end);
}

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

// The colours of a widget: the glass's ink, and macOS's accents.
struct Pal {
    D2D1_COLOR_F text, sub, faint, face, red, orange, green, track;
};

Pal GlassPal(const Theme& t)
{
    return {t.text, t.subtext, t.faint, Alpha(t.text, 0.10f), Rgba(0xFF453A, 0.98f), Rgba(0xFF9F0A, 0.98f), Rgba(0x30D158, 0.98f),
            Alpha(t.text, 0.18f)};
}

const wchar_t* const kWeekLong[] = {L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六", L"星期日"};
const wchar_t* const kWeekShort[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
const wchar_t* const kMonths[] = {L"一月", L"二月", L"三月", L"四月", L"五月", L"六月",
                                  L"七月", L"八月", L"九月", L"十月", L"十一月", L"十二月"};

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

} // namespace

struct DesktopWidgets::Widget {
    WidgetKind kind = WidgetKind::Clock;
    WidgetSize size = WidgetSize::Small;
    int col = 0, row = 0;
    std::unique_ptr<Surface> surface;
    bool down = false, dragging = false;
    POINT downAt{};
    RECT downRect{};
    RECT glideFrom{}, glideTo{};
    double glideMs = 0;
};

DesktopWidgets::DesktopWidgets(HINSTANCE instance) : instance_(instance)
{
    g_widgets = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    // A hidden top-level window: timers, display and theme changes, the menu's owner.
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Atrium Widgets", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    city_ = LocalCity();
    wchar_t test[8] = {};
    test_ = GetEnvironmentVariableW(L"ATRIUM_WIDGETS_TEST", test, 8) && test[0] == L'1';
}

DesktopWidgets::~DesktopWidgets()
{
    SetEnabled(false);
    if (host_)
        DestroyWindow(host_);
    g_widgets = nullptr;
}

void DesktopWidgets::SetEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (on) {
        status_ = QuerySystemStatus();
        Load();
        Build();
        hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(host_, kTickTimer, 1000, nullptr);
        SetTimer(host_, kSafetyTimer, 3000, nullptr);
        Restack();
        Log(L"widgets: on (%zu)", widgets_.size());
    } else {
        if (hook_)
            UnhookWinEvent(hook_);
        hook_ = nullptr;
        KillTimer(host_, kTickTimer);
        KillTimer(host_, kSafetyTimer);
        Clear();
        Log(L"widgets: off");
    }
}

void DesktopWidgets::SetMonitor(HMONITOR monitor)
{
    if (monitor == monitor_)
        return;
    monitor_ = monitor;
    if (enabled_)
        for (auto& w : widgets_)
            Place(*w, false);
}

// ---- layout ------------------------------------------------------------------

RECT DesktopWidgets::WorkArea() const
{
    MONITORINFO mi{sizeof(mi)};
    HMONITOR m = monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(m, &mi);
    return mi.rcWork;
}

float DesktopWidgets::Scale() const { return PopupScale(WorkArea()); }

int DesktopWidgets::Cols() const
{
    RECT wa = WorkArea();
    return std::max(1, int((float(wa.right - wa.left) / Scale() - 2 * kMargin + kGap) / (kCell + kGap)));
}

int DesktopWidgets::Rows() const
{
    RECT wa = WorkArea();
    // The bar floats over the bottom of the work area: leave it a row's worth of room.
    return std::max(1, int((float(wa.bottom - wa.top) / Scale() - 2 * kMargin - 90 + kGap) / (kCell + kGap)));
}

RECT DesktopWidgets::CellRect(int col, int row, WidgetSize size) const
{
    RECT wa = WorkArea();
    float s = Scale();
    int x = wa.left + int(std::lround((kMargin + float(col) * (kCell + kGap)) * s));
    int y = wa.top + int(std::lround((kMargin + float(row) * (kCell + kGap)) * s));
    return RECT{x, y, x + int(std::lround(WidthOf(size) * s)), y + int(std::lround(kCell * s))};
}

bool DesktopWidgets::Fits(int col, int row, WidgetSize size, const Widget* except) const
{
    if (col < 0 || row < 0 || col + SpanOf(size) > Cols() || row >= Rows())
        return false;
    for (const auto& w : widgets_) {
        if (w.get() == except)
            continue;
        bool apart = col + SpanOf(size) <= w->col || w->col + SpanOf(w->size) <= col || row != w->row;
        if (!apart)
            return false;
    }
    return true;
}

bool DesktopWidgets::FreeCellNear(float col, float row, WidgetSize size, const Widget* except, int& outCol, int& outRow) const
{
    float best = 1e9f;
    for (int r = 0; r < Rows(); ++r)
        for (int c = 0; c < Cols(); ++c) {
            if (!Fits(c, r, size, except))
                continue;
            float d = (float(c) - col) * (float(c) - col) + (float(r) - row) * (float(r) - row);
            if (d < best) {
                best = d;
                outCol = c;
                outRow = r;
            }
        }
    return best < 1e9f;
}

void DesktopWidgets::Load()
{
    widgets_.clear();
    std::wstring text = ReadTextFile(LayoutPath());
    struct Entry {
        WidgetKind kind;
        WidgetSize size;
        int col, row;
    };
    std::vector<Entry> entries;
    bool read = false;
    if (!text.empty()) {
        json::JsonObject o{nullptr};
        if (json::JsonObject::TryParse(text, o) && o.HasKey(L"widgets")) {
            read = true;
            for (const auto& v : o.GetNamedArray(L"widgets")) {
                if (v.ValueType() != json::JsonValueType::Object)
                    continue;
                json::JsonObject e = v.GetObject();
                WidgetKind k;
                if (!ParseKind(std::wstring(e.GetNamedString(L"kind", L"")), k))
                    continue;
                WidgetSize s = ParseSize(std::wstring(e.GetNamedString(L"size", L"small")));
                if (!Supports(k, s))
                    s = WidgetSize::Small;
                entries.push_back({k, s, int(e.GetNamedNumber(L"col", 0)), int(e.GetNamedNumber(L"row", 0))});
            }
        } else {
            Log(L"widgets: %s is not valid, starting afresh", LayoutPath().c_str());
        }
    }
    if (!read) // first run: a column of widgets at the top left, as macOS starts
        entries = {{WidgetKind::Clock, WidgetSize::Small, 0, 0},
                   {WidgetKind::Battery, WidgetSize::Small, 1, 0},
                   {WidgetKind::Calendar, WidgetSize::Medium, 0, 1},
                   {WidgetKind::Clock, WidgetSize::Medium, 0, 2}};
    for (const Entry& e : entries) {
        auto w = std::make_unique<Widget>();
        w->kind = e.kind;
        w->size = e.size;
        int c = e.col, r = e.row;
        if (!Fits(c, r, e.size, nullptr) && !FreeCellNear(float(c), float(r), e.size, nullptr, c, r))
            continue; // no room left on this screen
        w->col = c;
        w->row = r;
        widgets_.push_back(std::move(w));
    }
}

void DesktopWidgets::Save() const
{
    json::JsonArray list;
    for (const auto& w : widgets_) {
        json::JsonObject e;
        e.SetNamedValue(L"kind", json::JsonValue::CreateStringValue(KindName(w->kind)));
        e.SetNamedValue(L"size", json::JsonValue::CreateStringValue(SizeName(w->size)));
        e.SetNamedValue(L"col", json::JsonValue::CreateNumberValue(w->col));
        e.SetNamedValue(L"row", json::JsonValue::CreateNumberValue(w->row));
        list.Append(e);
    }
    json::JsonObject o;
    o.SetNamedValue(L"widgets", list);
    if (!WriteTextFile(LayoutPath(), std::wstring(o.Stringify())))
        Log(L"widgets: could not save the layout");
}

// ---- windows -----------------------------------------------------------------

DesktopWidgets::Widget& DesktopWidgets::Create(WidgetKind kind, WidgetSize size, int col, int row)
{
    auto w = std::make_unique<Widget>();
    w->kind = kind;
    w->size = size;
    w->col = col;
    w->row = row;
    widgets_.push_back(std::move(w));
    return *widgets_.back();
}

void DesktopWidgets::Build()
{
    for (auto& wp : widgets_) {
        Widget& w = *wp;
        if (w.surface)
            continue;
        w.surface = std::make_unique<Surface>(instance_, L"Atrium Widget", false, 14.f);
        w.surface->SetDesktopLayer(true);
        w.surface->SetRadius(kRadius);
        w.surface->paint = [this, &w](Canvas& c) { Paint(w, c); };
        w.surface->message = [this, &w](UINT m, WPARAM wp2, LPARAM lp, LRESULT& r) { return Message(w, m, wp2, lp, r); };
        Place(w, false);
    }
}

void DesktopWidgets::Clear()
{
    for (auto& w : widgets_)
        if (w->surface)
            w->surface->Hide(false);
    widgets_.clear();
}

void DesktopWidgets::Place(Widget& w, bool glide)
{
    if (!w.surface)
        return;
    RECT to = CellRect(w.col, w.row, w.size);
    if (!w.surface->Visible()) {
        w.surface->Show(to, false, POINT{(to.left + to.right) / 2, (to.top + to.bottom) / 2}, true);
        Restack();
        return;
    }
    if (!glide) {
        w.glideMs = 0;
        w.surface->Move(to);
        return;
    }
    w.glideFrom = w.surface->Rect();
    w.glideTo = to;
    w.glideMs = ClockMs();
    SetTimer(host_, kGlideTimer, 8, nullptr);
}

void DesktopWidgets::Glide()
{
    bool any = false;
    double now = ClockMs();
    for (auto& w : widgets_) {
        if (w->glideMs <= 0 || !w->surface)
            continue;
        float k = Ease(float((now - w->glideMs) / kGlideMs));
        // Size changes too (a widget made medium or small).
        auto lerp = [&](LONG a, LONG b) { return LONG(std::lround(float(a) + float(b - a) * k)); };
        RECT r{lerp(w->glideFrom.left, w->glideTo.left), lerp(w->glideFrom.top, w->glideTo.top), lerp(w->glideFrom.right, w->glideTo.right),
               lerp(w->glideFrom.bottom, w->glideTo.bottom)};
        w->surface->Move(r);
        if (k >= 1)
            w->glideMs = 0;
        else
            any = true;
    }
    if (!any)
        KillTimer(host_, kGlideTimer);
}

void DesktopWidgets::Add(WidgetKind kind, WidgetSize size)
{
    if (!Supports(kind, size))
        size = WidgetSize::Small;
    int c = 0, r = 0;
    if (!FreeCellNear(0, 0, size, nullptr, c, r)) {
        Log(L"widgets: no room for another");
        return;
    }
    Create(kind, size, c, r);
    if (enabled_)
        Build();
    Save();
    Log(L"widgets: added %s %s at %d,%d", KindName(kind), SizeName(size), c, r);
}

void DesktopWidgets::Remove(Widget& w)
{
    // Its window may be handling the message that asked for this.
    PostMessageW(host_, WM_WIDGET_REMOVE, 0, reinterpret_cast<LPARAM>(&w));
}

void DesktopWidgets::RemoveNow(Widget& w)
{
    auto it = std::find_if(widgets_.begin(), widgets_.end(), [&](const auto& p) { return p.get() == &w; });
    if (it == widgets_.end())
        return;
    std::unique_ptr<Widget> gone = std::move(*it);
    widgets_.erase(it);
    if (gone->surface)
        gone->surface->Hide(false);
    Save();
    Log(L"widgets: removed %s", KindName(gone->kind));
}

void DesktopWidgets::Resize(Widget& w, WidgetSize size)
{
    if (w.size == size || !Supports(w.kind, size))
        return;
    int c = w.col, r = w.row;
    if (!Fits(c, r, size, &w) && !FreeCellNear(float(c), float(r), size, &w, c, r))
        return;
    w.size = size;
    w.col = c;
    w.row = r;
    Place(w, true);
    w.surface->Invalidate();
    Save();
}

bool DesktopWidgets::IsOurs(HWND h) const
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// ---- the layer -----------------------------------------------------------------

void CALLBACK DesktopWidgets::OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG obj, LONG, DWORD, DWORD)
{
    if (g_widgets && obj == OBJID_WINDOW && !g_widgets->restackPosted_) {
        g_widgets->restackPosted_ = true; // a burst of events, one restack right after it
        PostMessageW(g_widgets->host_, WM_WIDGET_RESTACK, 0, 0);
    }
}

// Just above the desktop, under every window; over it when every app is
// minimised (Win+D raises the desktop over windows at the bottom, D14).
void DesktopWidgets::Restack()
{
    if (!enabled_ || widgets_.empty())
        return;
    if (std::any_of(widgets_.begin(), widgets_.end(), [](const auto& w) { return w->dragging; }))
        return; // the one being dragged stays over the windows
    if (test_) {
        // Screenshots: over the test backdrop.
        for (auto& w : widgets_)
            if (w->surface)
                SetWindowPos(w->surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    HWND fg = GetForegroundWindow();
    // Atrium's own windows (the bar, its menus) and the taskbar change nothing.
    bool neutral = fg && (IsOurs(fg) || ClassOf(fg) == L"Shell_TrayWnd" || ClassOf(fg) == L"Shell_SecondaryTrayWnd");
    // Every app on this screen minimised (or none), and none in front.
    struct Scan {
        const DesktopWidgets* self;
        HMONITOR monitor;
        bool open = false;
    } scan{this, monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* s = reinterpret_cast<Scan*>(l);
            if (!IsWindowVisible(h) || IsIconic(h) || s->self->IsOurs(h) || IsDesktopWindow(h))
                return TRUE;
            LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if ((ex & WS_EX_TOOLWINDOW) || (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) || GetWindowTextLengthW(h) == 0)
                return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            std::wstring c = ClassOf(h);
            if (cloaked || c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd" || c == L"Windows.UI.Core.CoreWindow")
                return TRUE;
            if (MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != s->monitor)
                return TRUE;
            s->open = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&scan));
    bool raise = !scan.open && (!fg || IsDesktopWindow(fg) || neutral);
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (raise) {
        for (auto& w : widgets_)
            if (w->surface)
                SetWindowPos(w->surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, flags);
    } else {
        // The window just above the desktop's top window (Progman, or the
        // WorkerW with the icons); the widgets go right under it.
        HWND above = nullptr, h = GetTopWindow(nullptr);
        bool found = false;
        for (; h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (IsWindowVisible(h) && IsDesktopWindow(h)) {
                found = true;
                break;
            }
            bool widget = std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& w) { return w->surface && w->surface->Hwnd() == h; });
            if (!widget)
                above = h;
        }
        if (!found)
            above = nullptr; // no desktop window: keep them at the bottom of the windows
        HWND after = found ? (above ? above : HWND_TOP) : HWND_BOTTOM;
        for (auto& w : widgets_) {
            if (!w->surface)
                continue;
            // Placed after a window that is not topmost, a topmost one stops being topmost.
            SetWindowPos(w->surface->Hwnd(), after, 0, 0, 0, 0, flags);
            after = w->surface->Hwnd();
        }
    }
    raised_ = raise;
}

LRESULT CALLBACK DesktopWidgets::HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<DesktopWidgets*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        switch (m) {
        case WM_TIMER:
            if (w == kTickTimer)
                self->Tick();
            else if (w == kSafetyTimer)
                self->Restack();
            else if (w == kGlideTimer)
                self->Glide();
            return 0;
        case WM_WIDGET_RESTACK:
            self->restackPosted_ = false;
            self->Restack();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            if (self->enabled_)
                for (auto& wd : self->widgets_)
                    self->Place(*wd, false);
            return 0;
        case WM_SETTINGCHANGE:
            if (w == SPI_SETWORKAREA && self->enabled_) {
                for (auto& wd : self->widgets_)
                    self->Place(*wd, false);
            }
            return 0;
        case WM_WIDGET_REMOVE: {
            auto* gone = reinterpret_cast<Widget*>(l);
            if (std::any_of(self->widgets_.begin(), self->widgets_.end(), [&](const auto& p) { return p.get() == gone; }))
                self->RemoveNow(*gone);
            return 0;
        }
        case WM_TIMECHANGE:
            self->city_ = LocalCity();
            self->Tick();
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void DesktopWidgets::Tick()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    bool minute = t.wMinute != lastMinute_;
    lastMinute_ = t.wMinute;
    if (minute || t.wSecond % 30 == 0)
        status_ = QuerySystemStatus();
    for (auto& w : widgets_)
        if (w->surface && (w->kind == WidgetKind::Clock || minute))
            w->surface->Invalidate();
}

// ---- input -------------------------------------------------------------------

bool DesktopWidgets::Message(Widget& w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r)
{
    (void)wp;
    (void)lp;
    (void)r;
    switch (m) {
    case WM_LBUTTONDOWN:
        w.down = true;
        w.dragging = false;
        GetCursorPos(&w.downAt);
        w.downRect = w.surface->Rect();
        SetCapture(w.surface->Hwnd());
        return true;
    case WM_MOUSEMOVE: {
        if (!w.down)
            return true;
        POINT p;
        GetCursorPos(&p);
        int dx = p.x - w.downAt.x, dy = p.y - w.downAt.y;
        if (!w.dragging && std::abs(dx) + std::abs(dy) > int(6 * Scale())) {
            w.dragging = true;
            w.glideMs = 0;
            // Over the windows while it moves, as macOS lifts a dragged widget.
            SetWindowPos(w.surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (w.dragging)
            w.surface->Move(RECT{w.downRect.left + dx, w.downRect.top + dy, w.downRect.right + dx, w.downRect.bottom + dy});
        return true;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
        if (!w.down)
            return m == WM_LBUTTONUP;
        w.down = false;
        bool dragged = w.dragging;
        w.dragging = false;
        if (m == WM_LBUTTONUP && GetCapture() == w.surface->Hwnd())
            ReleaseCapture();
        if (dragged) {
            // Snaps to the nearest free place of the grid and glides there.
            RECT cur = w.surface->Rect(), wa = WorkArea();
            float s = Scale();
            float col = (float(cur.left - wa.left) / s - kMargin) / (kCell + kGap), row = (float(cur.top - wa.top) / s - kMargin) / (kCell + kGap);
            int c = w.col, rr = w.row;
            if (FreeCellNear(col, row, w.size, &w, c, rr)) {
                w.col = c;
                w.row = rr;
            }
            Place(w, true);
            Save();
            Restack();
            Log(L"widgets: %s moved to %d,%d", KindName(w.kind), w.col, w.row);
        } else if (m == WM_LBUTTONUP) {
            Activate(w);
        }
        return true;
    }
    case WM_RBUTTONUP:
        Menu(w);
        return true;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        return true;
    }
    return false;
}

void DesktopWidgets::Activate(Widget& w)
{
    POINT p;
    GetCursorPos(&p);
    POINT c = w.surface->ScreenToContent(p);
    float s = w.surface->Scale();
    w.surface->Ripple(float(c.x) / s, float(c.y) / s);
    if (w.kind == WidgetKind::Clock)
        ShellExecuteW(nullptr, L"open", L"ms-clock:", nullptr, nullptr, SW_SHOWNORMAL);
    else if (w.kind == WidgetKind::Calendar && onCalendar)
        onCalendar();
    else if (w.kind == WidgetKind::Battery)
        ShellExecuteW(nullptr, L"open", status_.battery ? L"ms-settings:batterysaver" : L"ms-settings:powersleep", nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"widgets: %s opened", KindName(w.kind));
}

void DesktopWidgets::Menu(Widget& w)
{
    enum { kSmall = 1, kMedium, kRemove, kAddClock, kAddWorld, kAddCalendar, kAddMonth, kAddBattery };
    HMENU menu = CreatePopupMenu(), add = CreatePopupMenu();
    auto size = [&](UINT id, WidgetSize s, const wchar_t* label) {
        UINT f = MF_STRING | (w.size == s ? MF_CHECKED : 0) | (Supports(w.kind, s) ? 0 : MF_GRAYED);
        AppendMenuW(menu, f, id, label);
    };
    size(kSmall, WidgetSize::Small, L"小");
    size(kMedium, WidgetSize::Medium, L"中");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kRemove, L"移除小组件");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(add, MF_STRING, kAddClock, L"时钟");
    AppendMenuW(add, MF_STRING, kAddWorld, L"世界时钟");
    AppendMenuW(add, MF_STRING, kAddCalendar, L"日历");
    AppendMenuW(add, MF_STRING, kAddMonth, L"日历（月视图）");
    AppendMenuW(add, MF_STRING, kAddBattery, L"电池");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(add), L"添加小组件");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(host_); // the menu closes on a click elsewhere
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, host_, nullptr);
    DestroyMenu(menu); // and its submenu
    switch (cmd) {
    case kSmall:
        Resize(w, WidgetSize::Small);
        break;
    case kMedium:
        Resize(w, WidgetSize::Medium);
        break;
    case kRemove:
        Remove(w); // w is gone
        break;
    case kAddClock:
        Add(WidgetKind::Clock, WidgetSize::Small);
        break;
    case kAddWorld:
        Add(WidgetKind::Clock, WidgetSize::Medium);
        break;
    case kAddCalendar:
        Add(WidgetKind::Calendar, WidgetSize::Small);
        break;
    case kAddMonth:
        Add(WidgetKind::Calendar, WidgetSize::Medium);
        break;
    case kAddBattery:
        Add(WidgetKind::Battery, WidgetSize::Small);
        break;
    }
}

// ---- painting ------------------------------------------------------------------

void DesktopWidgets::Paint(Widget& w, Canvas& c)
{
    const Theme& t = c.theme;
    const float W = WidthOf(w.size), H = kCell;
    const D2D1_RECT_F card = D2D1::RectF(0, 0, W, H);
    if (c.panel)
        c.Fill(card, kRadius, t.panel);
    SYSTEMTIME now;
    GetLocalTime(&now);

    const Pal p = GlassPal(t);
    {
        const float pad = 14;
        const D2D1_COLOR_F face = p.face;
        if (w.kind == WidgetKind::Clock && w.size == WidgetSize::Small) {
            ClockFace(c, W / 2, H / 2, H / 2 - 9, now, p, face, p.text, true, city_, p.sub);
        } else if (w.kind == WidgetKind::Clock) {
            // The world clock: four faces, dark where it is night.
            SYSTEMTIME utc;
            GetSystemTime(&utc);
            const float colW = (W - 2 * pad) / 4, R = 31;
            for (int i = 0; i < 4; ++i) {
                DYNAMIC_TIME_ZONE_INFORMATION z{};
                SYSTEMTIME ct = now;
                if (ZoneInfo(kCities[i].zone, z))
                    SystemTimeToTzSpecificLocalTimeEx(&z, &utc, &ct);
                bool night = ct.wHour < 6 || ct.wHour >= 18;
                float cx = pad + colW * (float(i) + 0.5f), cy = (H - 2 * R - 42) / 2 + R - 2;
                // Night: a dark face with light ink, as on macOS.
                D2D1_COLOR_F f = night ? Rgba(0x000000, 0.42f) : face, ink = night ? Rgba(0xFFFFFF, 0.96f) : p.text;
                ClockFace(c, cx, cy, R, ct, p, f, ink, false, {}, ink);
                int64_t diff = (Minutes(ct) - Minutes(now)) / 60;
                int dayDiff = int((Minutes(ct) / 1440) - (Minutes(now) / 1440));
                std::wstring when = dayDiff > 0 ? L"明天" : dayDiff < 0 ? L"昨天" : L"今天";
                when += (diff >= 0 ? L" +" : L" ") + std::to_wstring(diff) + L"小时";
                c.Text(kCities[i].name, D2D1::RectF(cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 12.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
                c.Text(when, D2D1::RectF(cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
            }
        } else if (w.kind == WidgetKind::Calendar) {
            // The day: the weekday in red, the date large, the lunar date (a festival in red).
            const int wd = Weekday(now.wYear, now.wMonth, now.wDay);
            const float leftW = w.size == WidgetSize::Small ? W - 2 * pad : 126;
            c.Text(kWeekLong[wd], D2D1::RectF(pad + 2, pad, pad + leftW, pad + 20), 14, p.red, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(std::to_wstring(now.wDay), D2D1::RectF(pad, pad + 16, pad + leftW, pad + 80), 54, p.text, DWRITE_FONT_WEIGHT_NORMAL);
            bool festival = false;
            std::wstring label = lunar_.DayLabel(now.wYear, now.wMonth, now.wDay, &festival);
            std::wstring lunar = lunar_.MonthDay(now.wYear, now.wMonth, now.wDay);
            c.Text(lunar.empty() ? std::wstring(kMonths[now.wMonth - 1]) : L"农历" + lunar, D2D1::RectF(pad + 2, H - pad - 40, pad + leftW, H - pad - 20), 12.5f,
                   p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(festival ? label : std::to_wstring(now.wYear) + L"年" + kMonths[now.wMonth - 1], D2D1::RectF(pad + 2, H - pad - 20, pad + leftW, H - pad),
                   12, festival ? p.red : p.sub, DWRITE_FONT_WEIGHT_NORMAL);
            if (w.size == WidgetSize::Medium) {
                // The month: weekday initials, the days, today in a red disc.
                const float gx = pad + leftW + 6, gw = W - pad - gx, cw = gw / 7;
                int first = 0;
                DWORD fd = 0;
                if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&fd), 2))
                    first = int(fd % 7);
                c.Text(kMonths[now.wMonth - 1], D2D1::RectF(gx + 4, pad - 1, gx + gw, pad + 17), 12.5f, p.red, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                float y = pad + 20;
                for (int i = 0; i < 7; ++i)
                    c.Text(kWeekShort[(first + i) % 7], D2D1::RectF(gx + float(i) * cw, y, gx + float(i + 1) * cw, y + 14), 9.5f, p.sub,
                           DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
                y += 16;
                int lead = (Weekday(now.wYear, now.wMonth, 1) - first + 7) % 7, days = DaysIn(now.wYear, now.wMonth);
                int rows = (lead + days + 6) / 7;
                float rh = std::min(17.f, (H - pad - y) / float(rows));
                for (int d = 1; d <= days; ++d) {
                    int cell = lead + d - 1;
                    float x0 = gx + float(cell % 7) * cw, y0 = y + float(cell / 7) * rh, cx = x0 + cw / 2, cy = y0 + rh / 2;
                    bool today = d == now.wDay;
                    if (today)
                        c.Circle(cx, cy, rh / 2 + 0.5f, p.red);
                    bool weekend = (first + cell % 7) % 7 >= 5;
                    D2D1_COLOR_F ink = today ? Rgba(0xFFFFFF, 0.98f) : weekend ? p.sub : p.text;
                    c.Text(std::to_wstring(d), D2D1::RectF(x0, cy - rh / 2, x0 + cw, cy + rh / 2), 10.5f, ink,
                           today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
                }
            }
        } else {
            // Batteries: a ring round the computer, the level under it.
            const float cx = W / 2, cy = H / 2 - 12, R = 44;
            c.Arc(cx, cy, R, -90, 270, p.track, 9);
            if (status_.battery) {
                int pct = std::clamp(status_.batteryPercent, 0, 100);
                D2D1_COLOR_F level = pct <= 20 && !status_.charging ? p.red : p.green;
                c.Arc(cx, cy, R, -90, -90 + 360.f * float(pct) / 100.f, level, 9);
                c.Glyph(L"\uE7F8", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 24, p.text);
                // The level, with a bolt before it while charging.
                std::wstring n = std::to_wstring(pct) + L"%";
                float tw = c.TextWidth(n, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD), bolt = status_.charging ? 16.f : 0.f, x0 = (W - tw - bolt) / 2;
                if (status_.charging)
                    c.Glyph(L"\uE945", D2D1::RectF(x0 - 2, cy + R + 10, x0 + bolt, cy + R + 36), 13, level);
                c.Text(n, D2D1::RectF(x0 + bolt, cy + R + 10, x0 + bolt + tw + 4, cy + R + 36), 19, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            } else {
                c.Arc(cx, cy, R, -90, 270, p.green, 9);
                c.Glyph(L"\uE83E", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 22, p.text);
                c.Text(L"已接通电源", D2D1::RectF(0, cy + R + 12, W, cy + R + 34), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            }
        }
    }
}

// ---- tests ---------------------------------------------------------------------

void DesktopWidgets::LogState() const
{
    Log(L"widgets: state enabled=%d raised=%d count=%zu cols=%d rows=%d", enabled_ ? 1 : 0, raised_ ? 1 : 0, widgets_.size(), Cols(), Rows());
    for (size_t i = 0; i < widgets_.size(); ++i) {
        const Widget& w = *widgets_[i];
        RECT r = w.surface ? w.surface->Rect() : RECT{};
        // What is right under it: the next visible window that is not a widget.
        std::wstring under;
        bool topmost = false;
        if (w.surface) {
            topmost = (GetWindowLongPtrW(w.surface->Hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            for (HWND h = GetWindow(w.surface->Hwnd(), GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT)) {
                bool widget = std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& o) { return o->surface && o->surface->Hwnd() == h; });
                if (!widget && IsWindowVisible(h)) {
                    under = ClassOf(h);
                    break;
                }
            }
        }
        Log(L"widgets: w%zu kind=%s size=%s cell=%d,%d visible=%d topmost=%d under=%s rect=%ld,%ld,%ld,%ld", i, KindName(w.kind), SizeName(w.size), w.col,
            w.row, w.surface && w.surface->Visible() ? 1 : 0, topmost ? 1 : 0, under.empty() ? L"-" : under.c_str(), r.left, r.top, r.right, r.bottom);
    }
}

void DesktopWidgets::TestAction(const std::wstring& arg)
{
    auto part = [&](int n) {
        size_t b = 0;
        for (int i = 0; i < n && b != std::wstring::npos; ++i)
            b = arg.find(L':', b) == std::wstring::npos ? std::wstring::npos : arg.find(L':', b) + 1;
        if (b == std::wstring::npos)
            return std::wstring();
        size_t e = arg.find(L':', b);
        return arg.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
    };
    std::wstring verb = part(0);
    if (verb == L"add") {
        WidgetKind k;
        if (ParseKind(part(1), k))
            Add(k, ParseSize(part(2)));
    } else if (verb == L"remove" || verb == L"size") {
        size_t i = size_t(_wtoi(part(1).c_str()));
        if (i < widgets_.size()) {
            if (verb == L"remove")
                Remove(*widgets_[i]);
            else
                Resize(*widgets_[i], ParseSize(part(2)));
        }
    } else if (verb == L"restack") {
        Restack();
    }
}

} // namespace atrium
