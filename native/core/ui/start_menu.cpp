#include "ui/start_menu.h"

#include "clock.h"
#include "log.h"

#include <imm.h>
#include <sddl.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <windowsx.h>
#define SECURITY_WIN32
#include <security.h>
#include <secext.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "advapi32.lib")

namespace atrium {

namespace {

constexpr float kW = 640, kH = 720;  // DIP
constexpr float kPad = 20;           // outer padding
constexpr float kSearchTop = 20, kSearchH = 44;
constexpr float kHeaderTop = 78, kContentTop = 118;
constexpr float kShelfH = 64, kShelfGap = 12; // the shelf sits kShelfGap from the sides and the bottom
constexpr int kCols = 6;
constexpr float kTileH = 104, kRowH = 46, kGroupH = 34;
constexpr float kIcon = 44, kRowIcon = 28;
constexpr UINT_PTR kStatusTimer = 1;
constexpr double kStageMs = 280, kStaggerMs = 16, kRippleMs = 520;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
float Ease(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
// Ease out with a slight overshoot.
float EaseBack(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    const float c1 = 1.5f, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3.f) + c1 * std::pow(t - 1, 2.f);
}

// Initial for the all-apps list: A-Z, the pinyin initial of a Chinese name
// (by the zh-CN pronunciation sort against the first character of each
// initial), or '#'.
std::wstring GroupOf(const std::wstring& name)
{
    if (name.empty())
        return L"#";
    wchar_t c = name[0];
    if (c >= 0xFF21 && c <= 0xFF5A)
        c = wchar_t(c - 0xFEE0); // full-width Latin
    if (c < 0x80)
        return iswalpha(c) ? std::wstring(1, wchar_t(towupper(c))) : L"#";
    if (c >= 0x4E00 && c <= 0x9FFF) {
        static const wchar_t kFirst[] = L"吖八嚓咑妸发旮铪丌咔垃嘸拏噢妑七呥仨他屲夕丫帀";
        static const wchar_t kLetter[] = L"ABCDEFGHJKLMNOPQRSTWXYZ";
        int found = -1;
        for (int i = 0; kLetter[i]; ++i) {
            if (CompareStringEx(L"zh-CN", 0, &c, 1, &kFirst[i], 1, nullptr, nullptr, 0) == CSTR_LESS_THAN)
                break;
            found = i;
        }
        return found >= 0 ? std::wstring(1, kLetter[found]) : L"#";
    }
    if (iswalpha(c)) {
        // Accented Latin: the base letter.
        wchar_t base[4] = {};
        if (FoldStringW(MAP_COMPOSITE, &c, 1, base, 4) > 0 && base[0] < 0x80 && iswalpha(base[0]))
            return std::wstring(1, wchar_t(towupper(base[0])));
    }
    return L"#";
}

} // namespace

StartMenu::StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog)
    : surface_(instance, L"Atrium Start", true), tasks_(tasks), catalog_(catalog)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onMargin = [this] { Close(false); }; // a press next to the menu
}

float StartMenu::W() const { return float(surface_.Rect().right - surface_.Rect().left) / surface_.Scale(); }
float StartMenu::H() const { return float(surface_.Rect().bottom - surface_.Rect().top) / surface_.Scale(); }

void StartMenu::Rebuild()
{
    groups_.resize(apps_.size());
    order_.resize(apps_.size());
    for (size_t i = 0; i < apps_.size(); ++i) {
        groups_[i] = GroupOf(apps_[i].name);
        order_[i] = i;
    }
    // '#' first, then A-Z; within a group the catalog's (user locale) order.
    std::stable_sort(order_.begin(), order_.end(), [&](size_t a, size_t b) { return groups_[a] < groups_[b]; });
}

void StartMenu::LoadAccount()
{
    if (!userName_.empty())
        return;
    // Screenshots and screen recordings: ATRIUM_PRIVACY=1 shows neither the
    // account's name and picture nor the network's name.
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
    if (private_) {
        userName_ = L"\u7528\u6237"; // 用户
        return;
    }
    wchar_t buf[256] = {};
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, buf, &n) && buf[0]) {
        userName_ = buf;
    } else {
        DWORD m = 256;
        if (GetUserNameW(buf, &m))
            userName_ = buf;
    }
    // The account picture Windows keeps for the user.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return;
    BYTE info[256];
    DWORD len = 0;
    LPWSTR sid = nullptr;
    if (GetTokenInformation(token, TokenUser, info, sizeof(info), &len) && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info)->User.Sid, &sid)) {
        std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\") + sid;
        for (const wchar_t* value : {L"Image192", L"Image240", L"Image96", L"Image448"}) {
            wchar_t path[MAX_PATH] = {};
            DWORD size = sizeof(path);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS &&
                GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && LoadShellImage(path, 96, avatar_, true))
                break;
        }
        LocalFree(sid);
    }
    CloseHandle(token);
}

void StartMenu::Open(RECT bar, RECT logo, HWND previous)
{
    previous_ = previous;
    query_.clear();
    view_ = View::Pinned;
    scroll_ = scrollTarget_ = 0;
    sel_ = -1;
    hover_.clear();
    pressed_.clear();
    powerOpen_ = false;
    apps_ = catalog_.Apps();
    Rebuild();
    catalog_.Refresh(); // new installs since last time; the list updates when ready
    status_ = QuerySystemStatus();
    LoadAccount();
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    float s = float(dpi) / 96.f;
    const RECT& wa = mi.rcWork;
    int w = int(std::lround(kW * s)), h = int(std::lround(kH * s)), gap = int(std::lround(12 * s));
    int bottom = bar.top - gap;
    h = std::min(h, int(bottom - wa.top - gap)); // short screens: shorter menu, the lists scroll
    int left = std::clamp(int(bar.left), int(wa.left) + gap, int(wa.right) - gap - w);
    Restage();
    // Pops out of the logo.
    POINT anchor{(logo.left + logo.right) / 2, bottom};
    if (logo.right <= logo.left)
        anchor.x = left + w / 6;
    surface_.Show(RECT{left, bottom - h, left + w, bottom}, true, anchor);
    // Read the status again right away: Open may run inside a cross-process
    // SendMessage (test commands), where the network list cannot be queried.
    SetTimer(surface_.Hwnd(), kStatusTimer, 50, nullptr);
    Log(L"start: opened (%zu apps, %s)", apps_.size(), catalog_.Loaded() ? L"catalog ready" : L"catalog loading");
}

void StartMenu::Close(bool launched)
{
    if (!surface_.Visible())
        return;
    KillTimer(surface_.Hwnd(), kStatusTimer);
    powerOpen_ = false;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
    // Give the keyboard back to where it was, unless an app was just started
    // (it takes the foreground itself).
    wchar_t cls[64] = {};
    if (!launched && previous_ && IsWindow(previous_)) {
        GetClassNameW(previous_, cls, 64);
        if (!SetForegroundWindow(previous_))
            SwitchToThisWindow(previous_, TRUE);
    }
    Log(L"start: closed%s%s%s", launched ? L" (launched)" : L"", cls[0] ? L", focus back to " : L"", cls);
}

void StartMenu::Type(const std::wstring& text)
{
    for (wchar_t ch : text)
        SendMessageW(surface_.Hwnd(), WM_CHAR, ch, 0);
}

void StartMenu::LogState() const
{
    auto entries = Entries();
    const RECT r = surface_.Rect();
    const float s = surface_.Scale();
    auto screen = [&](const D2D1_RECT_F& d) {
        return std::to_wstring(int(float(r.left) + (d.left + d.right) / 2 * s)) + L"," + std::to_wstring(int(float(r.top) + (d.top + d.bottom) / 2 * s));
    };
    std::wstring spots; // screen px centres of the fixed buttons and the first tiles, for tests
    for (const Hit& h : hits_) {
        std::wstring name = h.id == L"app:0" || h.id == L"pin:0" ? L"first" : h.id == L"pin:1" ? L"second" : h.id;
        if (name == L"toggle" || name == L"tray" || name == L"power" || name == L"network" || name == L"volume" || name == L"account" ||
            name == L"first" || name == L"second" || name.rfind(L"pw:", 0) == 0)
            spots += L" " + name + L"@" + screen(h.rect);
    }
    Log(L"start: spots%s", spots.c_str());
    Log(L"start: state visible=%d foreground=%d view=%s query='%s' entries=%zu first='%s' catalog=%zu rect=%ld,%ld,%ld,%ld power=%d radius=%.0f",
        surface_.Visible() ? 1 : 0, GetForegroundWindow() == surface_.Hwnd() ? 1 : 0, view_ == View::All ? L"all" : L"pinned", query_.c_str(),
        entries.size(), entries.empty() ? L"" : entries[0].name.c_str(), apps_.size(), r.left, r.top, r.right, r.bottom, powerOpen_ ? 1 : 0, radius_);
}

void StartMenu::Refresh()
{
    if (!surface_.Visible())
        return;
    apps_ = catalog_.Apps();
    Rebuild();
    surface_.Invalidate();
}

void StartMenu::Restage()
{
    stageMs_ = ClockMs();
    hoverAnim_.clear();
}

std::vector<StartMenu::Entry> StartMenu::Entries() const
{
    std::vector<Entry> out;
    if (query_.empty() && view_ == View::Pinned) {
        // The pins, then every app below them (no recommendations).
        for (const TaskItem& t : tasks_.Snapshot())
            if (t.pinned)
                out.push_back({t.key, t.name, true, 0, {}});
    }
    if (query_.empty()) {
        for (size_t i : order_)
            if (i < apps_.size())
                out.push_back({apps_[i].id, apps_[i].name, false, i, groups_[i]});
        return out;
    }
    for (size_t i = 0; i < apps_.size(); ++i)
        if (AppCatalog::Matches(apps_[i], query_))
            out.push_back({apps_[i].id, apps_[i].name, false, i, {}});
    // Names that start with the query first, then the other matches.
    std::stable_partition(out.begin(), out.end(), [&](const Entry& e) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, e.name.c_str(), int(std::min(e.name.size(), query_.size())), query_.c_str(),
                               int(query_.size()), nullptr, nullptr, 0) == CSTR_EQUAL;
    });
    return out;
}

std::vector<StartMenu::Row> StartMenu::Layout(const std::vector<Entry>& entries, float width) const
{
    std::vector<Row> rows;
    // Pinned apps as tiles; in the pinned view the whole list follows them.
    const float cell = width / kCols;
    size_t i = 0;
    for (; i < entries.size() && entries[i].pinned; ++i) {
        float x = cell * float(i % kCols), y = kTileH * float(i / kCols);
        rows.push_back({D2D1::RectF(x, y, x + cell, y + kTileH), int(i), {}});
    }
    float y = i ? kTileH * float((i + kCols - 1) / kCols) + 10 : 0;
    if (i && i < entries.size()) {
        rows.push_back({D2D1::RectF(0, y, width, y + kGroupH + 6), -1, L"\u5168\u90E8\u5E94\u7528"}); // 全部应用
        y += kGroupH + 6;
    }
    std::wstring group;
    for (; i < entries.size(); ++i) {
        if (query_.empty() && entries[i].group != group) {
            group = entries[i].group;
            rows.push_back({D2D1::RectF(0, y, width, y + kGroupH), -1, group});
            y += kGroupH;
        }
        rows.push_back({D2D1::RectF(0, y, width, y + kRowH), int(i), {}});
        y += kRowH;
    }
    return rows;
}

float StartMenu::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& [v, last] = hoverAnim_[id];
    double dt = last > 0 ? std::clamp(c.now - last, 0.0, 100.0) : 0;
    last = c.now;
    float target = on ? 1.f : 0.f;
    float k = 1 - std::exp(-float(dt) / 1000.f * (on ? 20.f : 11.f));
    v += (target - v) * k;
    if (std::fabs(target - v) < 0.01f)
        v = target;
    else
        c.Continue();
    return v;
}

void StartMenu::Paint(Canvas& c)
{
    const Theme t = Theme::Current();
    const float w = W(), h = H(), R = radius_;
    const double now = c.now;
    hits_.clear();
    if (c.panel) // no glass (yet): a plain panel keeps the text readable
        c.Fill(D2D1::RectF(0, 0, w, h), R, t.panel);

    // Smooth scrolling towards the wheel's target.
    float dt = lastPaint_ > 0 ? float(std::clamp(now - lastPaint_, 0.0, 100.0)) : 16.f;
    lastPaint_ = now;
    scroll_ += (scrollTarget_ - scroll_) * (1 - std::exp(-dt / 1000.f * 16.f));
    if (std::fabs(scrollTarget_ - scroll_) < 0.3f)
        scroll_ = scrollTarget_;
    else
        c.Continue();

    // Search field: a capsule, like the bar.
    const float inner = std::clamp(R - kPad / 2, 8.f, kSearchH / 2);
    D2D1_RECT_F search = D2D1::RectF(kPad, kSearchTop, w - kPad, kSearchTop + kSearchH);
    float sh = Hover(c, L"search", hover_ == L"search" || !query_.empty());
    c.Fill(search, inner, t.card);
    if (sh > 0)
        c.Fill(search, inner, Alpha(t.cardHot, sh * 0.6f));
    c.Stroke(search, inner, query_.empty() ? t.divider : Alpha(t.accent, 0.85f), query_.empty() ? 1.f : 1.5f);
    c.Glyph(L"\uE721", D2D1::RectF(search.left + 10, search.top, search.left + 44, search.bottom), 16, t.subtext);
    D2D1_RECT_F field = D2D1::RectF(search.left + 46, search.top, search.right - 18, search.bottom);
    if (query_.empty())
        c.Text(L"搜索应用", field, 15, t.faint);
    else
        c.Text(query_, field, 15, t.text);
    float caretX = field.left + std::min(c.TextWidth(query_, 15), field.right - field.left - 2) + 1;
    c.Line(caretX, search.top + 12, caretX, search.bottom - 12, Alpha(t.accent, 0.9f), 1.5f);
    hits_.push_back({search, L"search"});

    // Header: what the area below shows, and the switch between pinned and all apps.
    std::wstring title = !query_.empty() ? L"搜索结果" : view_ == View::All ? L"全部应用" : L"已固定";
    c.Text(title, D2D1::RectF(kPad + 6, kHeaderTop, w / 2, kHeaderTop + 28), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (query_.empty()) {
        std::wstring label = view_ == View::All ? L"‹  已固定" : L"全部应用  ›";
        float lw = c.TextWidth(label, 12.5f) + 28;
        D2D1_RECT_F b = D2D1::RectF(w - kPad - lw, kHeaderTop, w - kPad, kHeaderTop + 28);
        float hv = Hover(c, L"toggle", hover_ == L"toggle");
        c.Fill(b, 14, t.card);
        c.Fill(b, 14, Alpha(pressed_ == L"toggle" ? t.press : t.cardHot, 0.3f + 0.7f * hv));
        c.Text(label, b, 12.5f, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
        hits_.push_back({b, L"toggle"});
    } else {
        c.Text(L"Enter 打开", D2D1::RectF(w / 2, kHeaderTop, w - kPad - 6, kHeaderTop + 28), 12, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
    }

    // Content: tiles or rows, sliding in staggered when the view changes.
    const float top = kContentTop, bottom = h - kShelfGap - kShelfH - 8;
    viewH_ = bottom - top;
    std::vector<Entry> entries = Entries();
    std::vector<Row> rows = Layout(entries, w - 2 * kPad);
    contentH_ = rows.empty() ? 0 : rows.back().rect.bottom;
    float maxScroll = std::max(0.f, contentH_ - viewH_);
    scrollTarget_ = std::clamp(scrollTarget_, 0.f, maxScroll);
    scroll_ = std::clamp(scroll_, 0.f, maxScroll);
    const bool grid = Grid();
    c.Clip(D2D1::RectF(0, top, w, bottom));
    if (entries.empty()) {
        std::wstring msg = !query_.empty() ? L"没有找到应用"
                           : grid           ? L"还没有应用。"
                           : catalog_.Loaded() ? L"没有应用" : L"正在读取应用…";
        c.Text(msg, D2D1::RectF(kPad, top + 40, w - kPad, top + 64), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
    const int tilePx = int(std::lround(kIcon * c.scale * 1.1f)), rowPx = int(std::lround(kRowIcon * c.scale * 1.1f));
    int shown = 0;
    static const IconImage kNone;
    for (const Row& row : rows) {
        D2D1_RECT_F r = D2D1::RectF(kPad + row.rect.left, top + row.rect.top - scroll_, kPad + row.rect.right, top + row.rect.bottom - scroll_);
        if (r.bottom < top || r.top > bottom)
            continue;
        float e = Ease(float((now - stageMs_ - kStaggerMs * std::min(shown++, 18)) / kStageMs));
        if (e < 1)
            c.Continue();
        c.BeginGroup(e, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 0, 14 * (1 - e));
        if (row.entry < 0) {
            if (row.label.size() > 1) { // a section title
                c.Text(row.label, D2D1::RectF(r.left + 6, r.top + 6, r.right, r.bottom), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            } else {
                c.Text(row.label, D2D1::RectF(r.left + 12, r.top + 8, r.right, r.bottom), 13, t.accent, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                c.Line(r.left + 34, (r.top + r.bottom) / 2 + 4, r.right - 8, (r.top + r.bottom) / 2 + 4, t.divider);
            }
            c.EndGroup();
            continue;
        }
        const Entry& en = entries[size_t(row.entry)];
        std::wstring id = (en.pinned ? L"pin:" : L"app:") + std::to_wstring(row.entry);
        bool selected = row.entry == sel_ || (sel_ < 0 && row.entry == 0 && !query_.empty());
        float hv = Hover(c, id, hover_ == id || selected);
        bool pressed = pressed_ == id;
        D2D1_RECT_F plate = D2D1::RectF(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3);
        const float plateR = std::clamp(R - kPad, 8.f, en.pinned ? 18.f : 14.f);
        const int iconPx = en.pinned ? tilePx : rowPx;
        if (hv > 0.001f)
            c.Fill(plate, plateR, selected && hover_ != id ? Alpha(t.accent, 0.20f * hv) : Alpha(t.cardHot, hv));
        if (pressed)
            c.Fill(plate, plateR, t.press);
        if (rippleId_ == id && now - rippleMs_ < kRippleMs) {
            c.Ripple(plate, plateR, rippleX_, rippleY_, float((now - rippleMs_) / kRippleMs),
                     t.light ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(1, 1, 1, 0.22f));
            c.Continue();
        }
        std::wstring cacheKey = (en.pinned ? L"p|" : L"c|") + en.key + L"|" + std::to_wstring(iconPx);
        bool cached = c.Cached(cacheKey);
        IconImage fetched;
        if (!cached)
            fetched = en.pinned ? tasks_.Icon(en.key, iconPx) : catalog_.Icon(en.key, iconPx);
        const IconImage& icon = cached ? kNone : fetched;
        bool hasIcon = cached || !icon.empty();
        float lift = pressed ? 0.94f : 1 + 0.07f * hv;
        if (en.pinned) {
            float cx = (r.left + r.right) / 2, iy = r.top + 14;
            D2D1_RECT_F ir = D2D1::RectF(cx - kIcon / 2, iy, cx + kIcon / 2, iy + kIcon);
            c.BeginGroup(1, lift, cx, iy + kIcon, 0, -3 * hv);
            if (hasIcon)
                c.Image(cacheKey, icon, ir);
            else
                c.Fill(ir, 10, t.hover);
            c.EndGroup();
            c.Text(en.name, D2D1::RectF(r.left + 6, r.top + 66, r.right - 6, r.top + 88), 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
        } else {
            float iy = (r.top + r.bottom) / 2 - kRowIcon / 2;
            D2D1_RECT_F ir = D2D1::RectF(r.left + 14, iy, r.left + 14 + kRowIcon, iy + kRowIcon);
            c.BeginGroup(1, lift, (ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2);
            if (hasIcon)
                c.Image(cacheKey, icon, ir);
            else
                c.Fill(ir, 7, t.hover);
            c.EndGroup();
            c.Text(en.name, D2D1::RectF(r.left + 56, r.top, r.right - 14, r.bottom), 14, t.text);
        }
        hits_.push_back({r, id});
        c.EndGroup();
    }
    c.Unclip();
    // Scroll indicator.
    if (contentH_ > viewH_ + 1) {
        float th = std::max(28.f, viewH_ * viewH_ / contentH_);
        float ty = top + (viewH_ - th) * (scroll_ / (contentH_ - viewH_));
        c.Fill(D2D1::RectF(w - 10, ty, w - 6, ty + th), 2, t.faint);
    }

    PaintShelf(c, t, w, h);
    if (powerOpen_)
        PaintPower(c, t, w, h);
}

// The shelf at the bottom: the account on the left; network, volume,
// battery, tray and power on the right.
void StartMenu::PaintShelf(Canvas& c, const Theme& t, float w, float h)
{
    D2D1_RECT_F shelf = D2D1::RectF(kShelfGap, h - kShelfGap - kShelfH, w - kShelfGap, h - kShelfGap);
    const float sr = std::clamp(radius_ - kShelfGap, 8.f, kShelfH / 2);
    c.Fill(shelf, sr, t.card);
    const float cy = (shelf.top + shelf.bottom) / 2, bh = 40;

    // Account.
    std::wstring name = userName_.empty() ? L"用户" : userName_;
    float nw = std::min(c.TextWidth(name, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD), 180.f);
    D2D1_RECT_F acct = D2D1::RectF(shelf.left + 10, cy - bh / 2, shelf.left + 10 + 6 + 32 + 10 + nw + 14, cy + bh / 2);
    float ah = Hover(c, L"account", hover_ == L"account");
    if (ah > 0)
        c.Fill(acct, bh / 2, Alpha(pressed_ == L"account" ? t.press : t.cardHot, ah));
    D2D1_RECT_F pic = D2D1::RectF(acct.left + 6, cy - 16, acct.left + 38, cy + 16);
    if (!avatar_.empty()) {
        c.ImageCircle(L"avatar", avatar_, pic);
    } else {
        c.Circle((pic.left + pic.right) / 2, cy, 16, Alpha(t.accent, 0.9f));
        c.Text(name.substr(0, 1), pic, 14, t.accentText, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    c.Text(name, D2D1::RectF(pic.right + 10, shelf.top, pic.right + 10 + nw + 2, shelf.bottom), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hits_.push_back({acct, L"account"});

    // Right side, built from the right edge.
    float x = shelf.right - 10;
    auto chip = [&](const std::wstring& id, const std::wstring& glyph, const std::wstring& label, bool dim) {
        float tw = label.empty() ? 0 : c.TextWidth(label, 12) + 4;
        float bw = label.empty() ? bh : 12 + 20 + 6 + tw + 12;
        D2D1_RECT_F b = D2D1::RectF(x - bw, cy - bh / 2, x, cy + bh / 2);
        float hv = Hover(c, id, hover_ == id || (id == L"power" && powerOpen_));
        if (hv > 0)
            c.Fill(b, bh / 2, Alpha(pressed_ == id ? t.press : t.cardHot, hv));
        D2D1_RECT_F g = label.empty() ? b : D2D1::RectF(b.left + 10, b.top, b.left + 34, b.bottom);
        c.Glyph(glyph, g, 16, dim ? t.faint : t.text);
        if (!label.empty())
            c.Text(label, D2D1::RectF(b.left + 38, b.top, b.right - 8, b.bottom), 12, dim ? t.faint : t.subtext);
        hits_.push_back({b, id});
        x = b.left - 4;
    };
    chip(L"power", L"\uE7E8", L"", false);
    c.Line(x - 2, cy - 12, x - 2, cy + 12, t.divider);
    x -= 6;
    chip(L"tray", L"\uE70E", L"托盘", false);
    if (status_.battery) {
        int level = std::clamp(status_.batteryPercent / 10, 0, 10);
        std::wstring glyph(1, wchar_t((status_.charging ? 0xEBAB : 0xEBA0) + level));
        chip(L"battery", glyph, std::to_wstring(status_.batteryPercent) + L"%", false);
    }
    if (status_.audio)
        chip(L"volume", status_.muted ? L"\uE74F" : status_.volume < 34 ? L"\uE993" : status_.volume < 67 ? L"\uE994" : L"\uE995",
             status_.muted ? L"静音" : std::to_wstring(status_.volume) + L"%", status_.muted);
    std::wstring net = status_.connected ? (status_.network.empty() || private_ ? L"已连接" : status_.network) : L"未连接";
    if (net.size() > 12)
        net = net.substr(0, 11) + L"…";
    chip(L"network", status_.connected ? L"\uE701" : L"\uEB55", net, !status_.connected);
}

// Power choices: a small card that springs up from the power button.
void StartMenu::PaintPower(Canvas& c, const Theme& t, float w, float h)
{
    auto power = std::find_if(hits_.begin(), hits_.end(), [](const Hit& hh) { return hh.id == L"power"; });
    if (power == hits_.end())
        return;
    const float cw = 172, rowH = 40, pad = 6, ch = 3 * rowH + 2 * pad;
    const float right = std::min(power->rect.right + 6, w - 8), bottom = h - kShelfGap - kShelfH - 6;
    D2D1_RECT_F card = D2D1::RectF(right - cw, bottom - ch, right, bottom);
    float e = float((c.now - powerMs_) / 220);
    if (e < 1)
        c.Continue();
    c.BeginGroup(Ease(e * 1.4f), 0.86f + 0.14f * EaseBack(e), right - 18, bottom);
    const float r = std::clamp(radius_ - kShelfGap, 8.f, 18.f);
    c.Fill(card, r, t.light ? D2D1::ColorF(0xFBFBFD, 0.97f) : D2D1::ColorF(0x2A2B31, 0.97f));
    c.Stroke(card, r, t.divider);
    static const wchar_t* const kGlyphs[] = {L"\uE708", L"\uE72C", L"\uE7E8"};
    static const wchar_t* const kNames[] = {L"睡眠", L"重启", L"关机"};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F row = D2D1::RectF(card.left + pad, card.top + pad + rowH * float(i), card.right - pad, card.top + pad + rowH * float(i + 1));
        std::wstring id = L"pw:" + std::to_wstring(i);
        float hv = Hover(c, id, hover_ == id || powerSel_ == i);
        if (hv > 0)
            c.Fill(row, std::max(4.f, r - pad), Alpha(powerSel_ == i && hover_ != id ? Alpha(t.accent, 0.22f) : t.hover, hv * 1.4f));
        c.Glyph(kGlyphs[i], D2D1::RectF(row.left + 6, row.top, row.left + 38, row.bottom), 15, t.text);
        c.Text(kNames[i], D2D1::RectF(row.left + 44, row.top, row.right - 8, row.bottom), 14, t.text);
        hits_.push_back({row, id});
    }
    c.EndGroup();
}

std::wstring StartMenu::HitAt(POINT pt) const
{
    float s = surface_.Scale();
    float x = float(pt.x) / s, y = float(pt.y) / s;
    // Last painted is on top (the power card over the content).
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        const Hit& h = *it;
        if (!In(h.rect, x, y))
            continue;
        // Rows scrolled under the header or the shelf are not clickable.
        bool content = h.id.rfind(L"pin:", 0) == 0 || h.id.rfind(L"app:", 0) == 0;
        if (content && (y < kContentTop || y > H() - kShelfGap - kShelfH - 8))
            continue;
        return h.id;
    }
    return {};
}

void StartMenu::Launch(const Entry& e)
{
    if (e.pinned) {
        tasks_.Launch(e.key);
        Log(L"start: launched pin %s", e.key.c_str());
    } else if (e.catalog < apps_.size()) {
        AppCatalog::Launch(apps_[e.catalog]);
    }
    Close(true);
}

void StartMenu::RunPower(int index)
{
    if (index < 0 || index > 2)
        return;
    Close(true);
    RunPowerAction(index == 0 ? PowerAction::Sleep : index == 1 ? PowerAction::Restart : PowerAction::Shutdown);
}

void StartMenu::Activate(const std::wstring& id)
{
    if (id == L"toggle") {
        view_ = view_ == View::All ? View::Pinned : View::All;
        scroll_ = scrollTarget_ = 0;
        sel_ = -1;
        Restage();
        surface_.Invalidate();
    } else if (id.rfind(L"pin:", 0) == 0 || id.rfind(L"app:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(4)));
        auto entries = Entries();
        if (i < entries.size())
            Launch(entries[i]);
    } else if (id == L"power") {
        powerOpen_ = !powerOpen_;
        powerSel_ = -1;
        powerMs_ = ClockMs();
        surface_.Invalidate();
    } else if (id.rfind(L"pw:", 0) == 0) {
        RunPower(id[3] - L'0');
    } else if (id == L"search") {
        // The field already has the keyboard.
    } else if (!id.empty() && onSystem) {
        Close(true);
        onSystem(id);
    }
}

void StartMenu::ContextMenu(const std::wstring& id, POINT screen)
{
    if (id.rfind(L"pin:", 0) != 0 && id.rfind(L"app:", 0) != 0)
        return;
    size_t i = size_t(std::stoul(id.substr(4)));
    auto entries = Entries();
    if (i >= entries.size())
        return;
    Entry e = entries[i];
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"打开");
    PinnedApp pin;
    if (e.pinned) {
        AppendMenuW(menu, MF_STRING, 2, L"从 Atrium 取消固定");
    } else if (e.catalog < apps_.size()) {
        pin = AppCatalog::PinFor(apps_[e.catalog]);
        AppendMenuW(menu, MF_STRING, 3, L"固定到 Atrium");
    }
    menuOpen_ = true;
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, surface_.Hwnd(), nullptr);
    menuOpen_ = false;
    DestroyMenu(menu);
    if (cmd == 1)
        Launch(e);
    else if (cmd == 2 && onUnpin)
        onUnpin(e.key);
    else if (cmd == 3 && onPin)
        onPin(pin);
    surface_.Invalidate();
}

void StartMenu::MoveSelection(int dx, int dy)
{
    auto entries = Entries();
    if (entries.empty())
        return;
    int n = int(entries.size());
    int pins = int(std::count_if(entries.begin(), entries.end(), [](const Entry& e) { return e.pinned; }));
    int cur = sel_ < 0 ? (query_.empty() ? -1 : 0) : sel_;
    int next;
    if (cur < 0)
        next = 0;
    else if (cur < pins) // tiles: a grid; down from the last row reaches the list
        next = dy > 0 && cur + kCols >= pins ? pins : cur + dx + dy * kCols;
    else // rows; up from the first row returns to the last tile
        next = cur + dy + dx < pins && cur + dy + dx >= 0 && dy < 0 ? pins - 1 : cur + dy + dx;
    sel_ = std::clamp(next, 0, n - 1);
    EnsureVisible();
    surface_.Invalidate();
}

void StartMenu::EnsureVisible()
{
    if (sel_ < 0)
        return;
    for (const Row& row : Layout(Entries(), W() - 2 * kPad))
        if (row.entry == sel_) {
            if (row.rect.top < scrollTarget_)
                scrollTarget_ = row.rect.top;
            else if (row.rect.bottom > scrollTarget_ + viewH_)
                scrollTarget_ = row.rect.bottom - viewH_;
        }
}

void StartMenu::PlaceIme()
{
    float s = surface_.Scale();
    if (HIMC imc = ImmGetContext(surface_.Hwnd())) {
        // Window coordinates: the content starts after the shadow margin.
        RECT win{};
        GetWindowRect(surface_.Hwnd(), &win);
        POINT origin{surface_.Rect().left - win.left, surface_.Rect().top - win.top};
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = POINT{origin.x + LONG((kPad + 46) * s), origin.y + LONG((kSearchTop + kSearchH + 4) * s)};
        ImmSetCompositionWindow(imc, &cf);
        ImmReleaseContext(surface_.Hwnd(), imc);
    }
}

bool StartMenu::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !menuOpen_ && surface_.Visible()) {
            wchar_t cls[64] = {};
            GetClassNameW(reinterpret_cast<HWND>(l), cls, 64);
            Log(L"start: deactivated by %s", cls);
            previous_ = nullptr; // the user clicked elsewhere: that window has the focus now
            Close(false);
        }
        return false;
    case WM_TIMER:
        if (w == kStatusTimer) {
            status_ = QuerySystemStatus();
            SetTimer(surface_.Hwnd(), kStatusTimer, 2000, nullptr);
            surface_.Invalidate();
            return true;
        }
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        std::wstring h = HitAt(pt);
        if (h != hover_) {
            hover_ = h;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hover_.clear();
        pressed_.clear();
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        pressed_ = HitAt(pt);
        if (powerOpen_ && pressed_ != L"power" && pressed_.rfind(L"pw:", 0) != 0)
            powerOpen_ = false; // a press elsewhere folds the power card away
        if (!pressed_.empty() && pressed_ != L"search") {
            float s = surface_.Scale();
            rippleId_ = pressed_;
            rippleX_ = float(pt.x) / s;
            rippleY_ = float(pt.y) / s;
            rippleMs_ = ClockMs();
            surface_.Ripple(rippleX_, rippleY_);
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        std::wstring h = HitAt(pt);
        bool same = !h.empty() && h == pressed_;
        pressed_.clear();
        if (same)
            Activate(h);
        else
            surface_.Invalidate();
        return true;
    }
    case WM_RBUTTONUP:
        ContextMenu(HitAt(pt), surface_.ContentToScreen(pt));
        return true;
    case WM_MOUSEWHEEL: {
        POINT client = surface_.ScreenToContent(pt);
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (HitAt(client) == L"volume") {
            AdjustVolume(2 * notches);
            status_ = QuerySystemStatus();
        } else {
            scrollTarget_ -= float(notches) * 3 * kRowH;
            scrollTarget_ = std::clamp(scrollTarget_, 0.f, std::max(0.f, contentH_ - viewH_));
        }
        surface_.Invalidate();
        return true;
    }
    case WM_IME_STARTCOMPOSITION:
        PlaceIme();
        return false;
    case WM_KEYDOWN:
        if (powerOpen_ && (w == VK_UP || w == VK_DOWN)) {
            powerSel_ = std::clamp(powerSel_ < 0 ? 0 : powerSel_ + (w == VK_DOWN ? 1 : -1), 0, 2);
            surface_.Invalidate();
            return true;
        }
        switch (w) {
        case VK_UP:
            MoveSelection(0, -1);
            return true;
        case VK_DOWN:
            MoveSelection(0, 1);
            return true;
        case VK_LEFT:
            MoveSelection(-1, 0);
            return true;
        case VK_RIGHT:
            MoveSelection(1, 0);
            return true;
        }
        return false;
    case WM_CHAR: {
        wchar_t ch = wchar_t(w);
        if (ch == 27) { // Esc: fold the power card, clear the search, then close
            if (powerOpen_) {
                powerOpen_ = false;
                surface_.Invalidate();
            } else if (!query_.empty()) {
                query_.clear();
                sel_ = -1;
                Restage();
                surface_.Invalidate();
            } else {
                Close(false);
            }
        } else if (ch == 13) {
            if (powerOpen_) {
                RunPower(powerSel_);
            } else {
                auto entries = Entries();
                int i = sel_ >= 0 ? sel_ : query_.empty() ? -1 : 0;
                if (i >= 0 && i < int(entries.size()))
                    Launch(entries[size_t(i)]);
            }
        } else if (ch == 8) {
            if (!query_.empty()) {
                query_.pop_back();
                if (query_.empty())
                    Restage();
            }
            sel_ = -1;
            scroll_ = scrollTarget_ = 0;
            surface_.Invalidate();
        } else if (ch >= 32 && query_.size() < 64) {
            if (query_.empty())
                Restage(); // the results slide in
            query_ += ch;
            sel_ = -1;
            scroll_ = scrollTarget_ = 0;
            powerOpen_ = false;
            surface_.Invalidate();
        }
        result = 0;
        return true;
    }
    }
    return false;
}

} // namespace atrium
