#include "ui/start_menu.h"

#include "clock.h"
#include "log.h"
#include "motion.h"
#include "render/wifi_glyph.h"

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

// DIP. The panel above, the status capsule (the bar's height) kGap below it.
constexpr float kW = 560, kMainH = 392, kGap = 12;
constexpr float kSearchX = 24, kSearchTop = 20, kSearchH = 44;
constexpr int kCols = 7, kRows = 3, kPerPage = kCols * kRows;
constexpr float kGridTop = 86, kCellW = 72, kCellH = 88, kIcon = 40;
constexpr float kRowH = 52, kRowIcon = 28, kListTop = 84, kListBottom = 348;
constexpr float kDotsY = 366;
constexpr float kPowerW = 176, kPowerRowH = 44, kPowerPad = 12;
constexpr UINT_PTR kStatusTimer = 1;
constexpr double kStageMs = 260, kStaggerMs = 12, kRippleMs = 520, kPowerMs = 220;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

} // namespace

StartMenu::StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog)
    : surface_(instance, L"Atrium Start", true), tasks_(tasks), catalog_(catalog)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(false); }; // a press next to the menu
}

float StartMenu::W() const { return float(surface_.Rect().right - surface_.Rect().left) / surface_.Scale(); }
float StartMenu::H() const { return float(surface_.Rect().bottom - surface_.Rect().top) / surface_.Scale(); }
D2D1_RECT_F StartMenu::MainRect() const { return D2D1::RectF(0, 0, W(), H() - barH_ - kGap); }
D2D1_RECT_F StartMenu::ShelfRect() const { return D2D1::RectF(0, H() - barH_, W(), H()); }
float StartMenu::ButtonSize() const { return std::min(44.f, barH_ - 12); }
D2D1_RECT_F StartMenu::PowerRect() const
{
    // Right-aligned with the power button, the last in the capsule.
    D2D1_RECT_F shelf = ShelfRect();
    float right = std::min(shelf.right - (barH_ - ButtonSize()) / 2 + 6, W());
    float bottom = shelf.top - 8, h = 4 * kPowerRowH + 2 * kPowerPad;
    return D2D1::RectF(right - kPowerW, bottom - h, right, bottom);
}

float StartMenu::Covered(const D2D1_RECT_F& r, double now) const
{
    // Content under the power card fades as it opens: the card's glass is
    // below the content layer, so the content would show through it.
    if (!powerOpen_)
        return 1;
    D2D1_RECT_F p = PowerRect();
    if (r.right <= p.left || r.left >= p.right || r.bottom <= p.top || r.top >= p.bottom)
        return 1;
    return 1 - motion::EaseOut(float((now - powerMs_) / (kPowerMs * 0.6)));
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
        userName_ = L"用户"; // 用户
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
    results_.clear();
    page_ = 0;
    pageShown_ = 0;
    wheelAcc_ = 0;
    scroll_ = scrollTarget_ = 0;
    sel_ = -1;
    hover_.clear();
    pressed_.clear();
    powerOpen_ = false;
    apps_ = catalog_.Apps();
    pins_ = catalog_.StartPins();
    catalog_.Refresh(); // new installs and pins since last time; the menu updates when ready
    if (onStatusRequest)
        onStatusRequest();
    LoadAccount();
    RECT r = PopupRect(bar, D2D1::SizeF(kW, kMainH + kGap + barH_), PopupAlign::Left);
    Restage();
    // Pops out of the logo.
    POINT anchor{(logo.left + logo.right) / 2, r.bottom};
    if (logo.right <= logo.left)
        anchor.x = r.left + (r.right - r.left) / 6;
    surface_.Show(r, true, anchor);
    SetTimer(surface_.Hwnd(), kStatusTimer, 2000, nullptr);
    Log(L"start: opened (%zu apps, %zu start pins, %s)", apps_.size(), pins_.size(), catalog_.Loaded() ? L"catalog ready" : L"catalog loading");
}

void StartMenu::Close(bool launched)
{
    if (!surface_.Visible())
        return;
    KillTimer(surface_.Hwnd(), kStatusTimer);
    catalog_.SetVisibleIcons({});
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

void StartMenu::SetStatus(const SystemStatus& status)
{
    if (!(status_ == status)) {
        status_ = status;
        surface_.Invalidate();
    }
}

void StartMenu::Refresh()
{
    if (!surface_.Visible())
        return;
    apps_ = catalog_.Apps();
    pins_ = catalog_.StartPins();
    if (!query_.empty())
        results_ = Results();
    page_ = std::clamp(page_, 0, Pages() - 1);
    surface_.Invalidate();
}

void StartMenu::Restage()
{
    stageMs_ = ClockMs();
    hoverAnim_.clear();
}

std::vector<CatalogApp> StartMenu::Results() const
{
    std::vector<CatalogApp> out;
    for (const CatalogApp& a : apps_)
        if (AppCatalog::Matches(a, query_))
            out.push_back(a);
    // Names that start with the query first, then the other matches.
    std::stable_partition(out.begin(), out.end(), [&](const CatalogApp& a) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.name.c_str(), int(std::min(a.name.size(), query_.size())), query_.c_str(),
                               int(query_.size()), nullptr, nullptr, 0) == CSTR_EQUAL;
    });
    return out;
}

int StartMenu::TileIconPx(float scale) { return int(std::lround(kIcon * scale * 1.1f)); }

const std::vector<CatalogApp>& StartMenu::Shown() const { return query_.empty() ? pins_ : results_; }

int StartMenu::Pages() const { return std::max(1, int((pins_.size() + kPerPage - 1) / kPerPage)); }

void StartMenu::SetPage(int page)
{
    page = std::clamp(page, 0, Pages() - 1);
    if (page == page_)
        return;
    page_ = page;
    if (sel_ >= 0 && sel_ / kPerPage != page_)
        sel_ = -1;
    surface_.Invalidate();
}

float StartMenu::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& f = hoverAnim_[id];
    float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void StartMenu::Paint(Canvas& c)
{
    visibleIcons_.clear();
    const Theme& t = c.theme;
    const double now = c.now;
    hits_.clear();
    float dt = lastPaint_ > 0 ? float(std::clamp(now - lastPaint_, 0.0, 100.0)) : 16.f;
    lastPaint_ = now;
    auto glide = [&](float& v, float target, float rate, float eps) {
        v = motion::Approach(v, target, dt, rate);
        if (std::fabs(target - v) < eps)
            v = target;
        else
            c.Continue();
    };
    glide(scroll_, scrollTarget_, 16, 0.3f);
    glide(pageShown_, float(page_), 14, 0.002f);

    // The pieces of glass: the panel, the capsule (round like the bar) and,
    // while open, the power card growing up out of the capsule.
    const D2D1_RECT_F main = MainRect(), shelf = ShelfRect();
    std::vector<ShapeDip> shapes{{main, radius_}, {shelf, (shelf.bottom - shelf.top) / 2}};
    if (c.panel) // no glass (yet): plain panels keep the text readable
        for (const ShapeDip& sh : shapes)
            c.Fill(sh.rect, sh.radius, t.panel);

    PaintSearch(c, t);
    if (query_.empty())
        PaintPins(c, t);
    else
        PaintResults(c, t);
    PaintShelf(c, t);
    catalog_.SetVisibleIcons(visibleIcons_);
    if (powerOpen_) {
        float e = float((now - powerMs_) / kPowerMs);
        D2D1_RECT_F card = PowerRect();
        float grow = 0.55f + 0.45f * motion::EaseOutBack(e, 1.5f);
        card.top = card.bottom - (card.bottom - card.top) * grow;
        shapes.push_back({card, 20});
        if (c.panel)
            c.Fill(card, 20, t.panel);
        PaintPower(c, t);
    }
    surface_.SetShapes(shapes);
}

// Search: an outlined capsule at the top of the panel; typing anywhere goes here.
void StartMenu::PaintSearch(Canvas& c, const Theme& t)
{
    const float w = W();
    D2D1_RECT_F search = D2D1::RectF(kSearchX, kSearchTop, w - kSearchX, kSearchTop + kSearchH);
    const float r = kSearchH / 2;
    float sh = Hover(c, L"search", hover_ == L"search" || !query_.empty());
    c.Fill(search, r, Alpha(t.card, 0.6f + 0.4f * sh));
    c.Stroke(search, r, query_.empty() ? t.border : Alpha(t.accent, 0.85f), query_.empty() ? 1.f : 1.5f);
    c.Glyph(L"\uE721", D2D1::RectF(search.left + 10, search.top, search.left + 44, search.bottom), 16, t.subtext);
    D2D1_RECT_F field = D2D1::RectF(search.left + 46, search.top, search.right - 48, search.bottom);
    if (query_.empty())
        c.Text(L"搜索应用", field, 15, t.faint); // 搜索应用
    else
        c.Text(query_, field, 15, t.text);
    float caretX = field.left + std::min(c.TextWidth(query_, 15), field.right - field.left - 2) + 1;
    c.Line(caretX, search.top + 12, caretX, search.bottom - 12, Alpha(t.accent, 0.9f), 1.5f);
    hits_.push_back({search, L"search"});
    if (!query_.empty()) {
        D2D1_RECT_F clear = D2D1::RectF(search.right - 40, search.top + 6, search.right - 8, search.bottom - 6);
        float hv = Hover(c, L"clear", hover_ == L"clear");
        c.Fill(clear, 16, Alpha(pressed_ == L"clear" ? t.press : t.hover, 0.5f + 0.5f * hv));
        c.Glyph(L"\uE711", clear, 10, t.text);
        hits_.push_back({clear, L"clear"});
    }
}

// The Windows start menu's pins, 7 x 3 per page; pages slide sideways.
void StartMenu::PaintPins(Canvas& c, const Theme& t)
{
    const float w = W(), x0 = (w - kCols * kCellW) / 2;
    const double now = c.now;
    if (pins_.empty()) {
        std::wstring msg = catalog_.Loaded() ? L"Windows 开始菜单里还没有固定的应用" : L"正在读取…";
        c.Text(msg, D2D1::RectF(kSearchX, kGridTop + 60, w - kSearchX, kGridTop + 90), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        return;
    }
    const int tilePx = TileIconPx(c.scale);
    static const IconImage kNone;
    c.Clip(D2D1::RectF(0, kListTop - 4, w, kGridTop + kRows * kCellH + 4));
    for (size_t i = 0; i < pins_.size(); ++i) {
        int page = int(i) / kPerPage, k = int(i) % kPerPage;
        float x = x0 + float(k % kCols) * kCellW + (float(page) - pageShown_) * w, y = kGridTop + float(k / kCols) * kCellH;
        if (x + kCellW < 0 || x > w)
            continue;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + kCellW, y + kCellH);
        float e = motion::EaseOut(float((now - stageMs_ - kStaggerMs * std::min(k, 18)) / kStageMs));
        if (e < 1)
            c.Continue();
        float shown = Covered(r, now);
        if (shown <= 0.01f)
            continue;
        if (shown < 1)
            c.Continue();
        c.BeginGroup(e * shown, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 0, 10 * (1 - e));
        std::wstring id = L"pin:" + std::to_wstring(i);
        bool selected = int(i) == sel_;
        float hv = Hover(c, id, hover_ == id || selected);
        bool pressed = pressed_ == id;
        D2D1_RECT_F plate = D2D1::RectF(r.left + 2, r.top + 1, r.right - 2, r.bottom - 1);
        if (hv > 0.001f)
            c.Fill(plate, 16, selected && hover_ != id ? Alpha(t.accent, 0.22f * hv) : Alpha(t.hover, hv));
        if (pressed)
            c.Fill(plate, 16, t.press);
        if (rippleId_ == id && now - rippleMs_ < kRippleMs) {
            c.Ripple(plate, 16, rippleX_, rippleY_, float((now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
            c.Continue();
        }
        const CatalogApp& app = pins_[i];
        visibleIcons_.emplace_back(app.id, tilePx);
        std::wstring cacheKey = L"c|" + app.id + L"|" + std::to_wstring(tilePx);
        bool cached = c.Cached(cacheKey);
        IconImage fetched;
        if (!cached)
            fetched = catalog_.Icon(app.id, tilePx);
        const IconImage& icon = cached ? kNone : fetched;
        float cx = (r.left + r.right) / 2, iy = r.top + 8;
        D2D1_RECT_F ir = D2D1::RectF(cx - kIcon / 2, iy, cx + kIcon / 2, iy + kIcon);
        c.BeginGroup(1, pressed ? 0.94f : 1 + 0.08f * hv, cx, iy + kIcon, 0, -2 * hv);
        if (cached || !icon.empty())
            c.Image(cacheKey, icon, ir);
        else
            c.Fill(ir, 10, t.hover);
        c.EndGroup();
        c.Text(app.name, D2D1::RectF(r.left + 2, r.top + 52, r.right - 2, r.bottom + 2), 11.5f, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1, true);
        c.EndGroup();
        if (page == page_)
            hits_.push_back({r, id});
    }
    c.Unclip();

    // Page dots; the current one is drawn out into a short capsule.
    int pages = Pages();
    if (pages > 1) {
        const float dot = 6, gapX = 8;
        float total = 0;
        std::vector<float> widths(static_cast<size_t>(pages));
        for (int p = 0; p < pages; ++p) {
            float on = std::max(0.f, 1 - std::fabs(pageShown_ - float(p)));
            widths[size_t(p)] = dot + 12 * on;
            total += widths[size_t(p)] + (p ? gapX : 0);
        }
        float x = (w - total) / 2;
        for (int p = 0; p < pages; ++p) {
            float on = std::max(0.f, 1 - std::fabs(pageShown_ - float(p)));
            std::wstring id = L"page:" + std::to_wstring(p);
            float hv = Hover(c, id, hover_ == id);
            D2D1_RECT_F d = D2D1::RectF(x, kDotsY - dot / 2, x + widths[size_t(p)], kDotsY + dot / 2);
            D2D1_COLOR_F col = t.faint;
            col.a = t.faint.a + (t.text.a - t.faint.a) * std::max(on, 0.5f * hv);
            c.Fill(d, dot / 2, col);
            hits_.push_back({D2D1::RectF(d.left - gapX / 2, kDotsY - 10, d.right + gapX / 2, kDotsY + 10), id});
            x = d.right + gapX;
        }
    }
}

// Search results: one row per app, the first (or the selected) one opens with Enter.
void StartMenu::PaintResults(Canvas& c, const Theme& t)
{
    const float w = W();
    const double now = c.now;
    contentH_ = float(results_.size()) * kRowH;
    const float viewH = kListBottom - kListTop;
    scrollTarget_ = std::clamp(scrollTarget_, 0.f, std::max(0.f, contentH_ - viewH));
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, contentH_ - viewH));
    if (results_.empty()) {
        c.Text(L"没有找到应用", D2D1::RectF(kSearchX, kGridTop + 60, w - kSearchX, kGridTop + 90), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        return;
    }
    const int rowPx = int(std::lround(kRowIcon * c.scale * 1.1f));
    static const IconImage kNone;
    c.Clip(D2D1::RectF(0, kListTop, w, kListBottom));
    int shown = 0;
    for (size_t i = 0; i < results_.size(); ++i) {
        float y = kListTop + float(i) * kRowH - scroll_;
        if (y + kRowH < kListTop || y > kListBottom)
            continue;
        D2D1_RECT_F r = D2D1::RectF(16, y, w - 16, y + kRowH);
        float e = motion::EaseOut(float((now - stageMs_ - kStaggerMs * std::min(shown++, 8)) / kStageMs));
        if (e < 1)
            c.Continue();
        float visible = Covered(r, now);
        if (visible <= 0.01f)
            continue;
        if (visible < 1)
            c.Continue();
        c.BeginGroup(e * visible, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 0, 8 * (1 - e));
        std::wstring id = L"app:" + std::to_wstring(i);
        bool selected = int(i) == sel_ || (sel_ < 0 && i == 0);
        float hv = Hover(c, id, hover_ == id || selected);
        D2D1_RECT_F plate = D2D1::RectF(r.left, r.top + 2, r.right, r.bottom - 2);
        const float pr = (plate.bottom - plate.top) / 2;
        if (hv > 0.001f)
            c.Fill(plate, pr, selected && hover_ != id ? Alpha(t.accent, 0.22f * hv) : Alpha(t.hover, hv));
        if (pressed_ == id)
            c.Fill(plate, pr, t.press);
        if (rippleId_ == id && now - rippleMs_ < kRippleMs) {
            c.Ripple(plate, pr, rippleX_, rippleY_, float((now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
            c.Continue();
        }
        const CatalogApp& app = results_[i];
        visibleIcons_.emplace_back(app.id, rowPx);
        std::wstring cacheKey = L"c|" + app.id + L"|" + std::to_wstring(rowPx);
        bool cached = c.Cached(cacheKey);
        IconImage fetched;
        if (!cached)
            fetched = catalog_.Icon(app.id, rowPx);
        const IconImage& icon = cached ? kNone : fetched;
        float iy = (r.top + r.bottom) / 2 - kRowIcon / 2;
        D2D1_RECT_F ir = D2D1::RectF(r.left + 14, iy, r.left + 14 + kRowIcon, iy + kRowIcon);
        c.BeginGroup(1, pressed_ == id ? 0.94f : 1 + 0.06f * hv, (ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2);
        if (cached || !icon.empty())
            c.Image(cacheKey, icon, ir);
        else
            c.Fill(ir, 7, t.hover);
        c.EndGroup();
        c.Text(app.name, D2D1::RectF(r.left + 56, r.top, r.right - 80, r.bottom), 14, t.text);
        if (selected)
            c.Text(L"Enter 打开", D2D1::RectF(r.right - 100, r.top, r.right - 18, r.bottom), 12, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
        c.EndGroup();
        hits_.push_back({r, id});
    }
    c.Unclip();
    if (contentH_ > viewH + 1) {
        float th = std::max(28.f, viewH * viewH / contentH_);
        float ty = kListTop + (viewH - th) * (scroll_ / (contentH_ - viewH));
        c.Fill(D2D1::RectF(w - 9, ty, w - 5, ty + th), 2, t.faint);
    }
}

// The capsule: the account on the left; network, volume, battery, tray and
// power on the right, round buttons like the bar's.
void StartMenu::PaintShelf(Canvas& c, const Theme& t)
{
    const D2D1_RECT_F shelf = ShelfRect();
    const float cy = (shelf.top + shelf.bottom) / 2, bh = ButtonSize();
    const float edge = (shelf.bottom - shelf.top - bh) / 2; // concentric with the capsule

    // Right side, built from the right edge.
    float x = shelf.right - edge;
    auto button = [&](const std::wstring& id, const std::wstring& glyph, const std::wstring& label, bool dim) {
        float tw = label.empty() ? 0 : c.TextWidth(label, 12) + 4;
        float bw = label.empty() ? bh : 12 + 20 + 4 + tw + 12;
        D2D1_RECT_F b = D2D1::RectF(x - bw, cy - bh / 2, x, cy + bh / 2);
        float hv = Hover(c, id, hover_ == id || (id == L"power" && powerOpen_));
        if (hv > 0)
            c.Fill(b, bh / 2, Alpha(pressed_ == id ? t.press : t.hover, hv));
        D2D1_RECT_F g = label.empty() ? b : D2D1::RectF(b.left + 10, b.top, b.left + 34, b.bottom);
        if (id == L"network" && status_.connected && status_.wifi)
            c.WifiFan((g.left + g.right) / 2, cy, 12, WifiSteps(status_.signal), t.text);
        else
            c.Glyph(glyph, g, 16, dim ? t.faint : t.text);
        if (!label.empty())
            c.Text(label, D2D1::RectF(b.left + 36, b.top, b.right - 8, b.bottom), 12, dim ? t.faint : t.subtext);
        hits_.push_back({b, id});
        x = b.left - 2;
    };
    button(L"power", L"\uE7E8", L"", false);
    c.Line(x - 4, cy - 12, x - 4, cy + 12, t.divider);
    x -= 8;
    button(L"tray", L"\uE70E", L"", false);
    if (status_.battery) {
        int level = std::clamp(status_.batteryPercent / 10, 0, 10);
        std::wstring glyph(1, wchar_t((status_.charging ? 0xEBAB : 0xEBA0) + level));
        button(L"battery", glyph, std::to_wstring(status_.batteryPercent) + L"%", false);
    }
    if (status_.audio)
        button(L"volume", status_.muted ? L"\uE74F" : status_.volume < 34 ? L"\uE993" : status_.volume < 67 ? L"\uE994" : L"\uE995", L"", status_.muted);
    button(L"network", !status_.connected ? L"\uEB55" : status_.wifi ? L"\uE701" : L"\uE839", L"", !status_.connected);

    // Account, in what is left.
    std::wstring name = userName_.empty() ? L"用户" : userName_;
    const float left = shelf.left + edge;
    float nw = std::clamp(c.TextWidth(name, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD), 0.f, std::max(0.f, x - left - 70));
    D2D1_RECT_F acct = D2D1::RectF(left, cy - bh / 2, left + 8 + 28 + 10 + nw + 16, cy + bh / 2);
    float ah = Hover(c, L"account", hover_ == L"account");
    if (ah > 0)
        c.Fill(acct, bh / 2, Alpha(pressed_ == L"account" ? t.press : t.hover, ah));
    D2D1_RECT_F pic = D2D1::RectF(acct.left + 8, cy - 14, acct.left + 36, cy + 14);
    if (!avatar_.empty()) {
        c.ImageCircle(L"avatar", avatar_, pic);
    } else {
        c.Circle((pic.left + pic.right) / 2, cy, 14, Alpha(t.accent, 0.9f));
        bool halo = c.halo;
        c.halo = false;
        c.Text(name.substr(0, 1), pic, 13, t.accentText, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.halo = halo;
    }
    c.Text(name, D2D1::RectF(pic.right + 10, shelf.top, pic.right + 10 + nw + 2, shelf.bottom), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hits_.push_back({acct, L"account"});
}

// Power choices, in their own piece of glass above the power button.
void StartMenu::PaintPower(Canvas& c, const Theme& t)
{
    D2D1_RECT_F card = PowerRect();
    float e = float((c.now - powerMs_) / kPowerMs);
    if (e < 1)
        c.Continue();
    c.BeginGroup(motion::EaseOut(e * 1.3f), 0.92f + 0.08f * motion::EaseOutBack(e, 1.5f), card.right - 22, card.bottom);
    static const wchar_t* const kGlyphs[] = {L"\uE708", L"\uE72C", L"\uE7E8", L"\uE72E"};
    static const wchar_t* const kNames[] = {L"睡眠", L"重启", L"关机", L"锁定"};
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F row = D2D1::RectF(card.left + 8, card.top + kPowerPad + kPowerRowH * float(i), card.right - 8,
                                      card.top + kPowerPad + kPowerRowH * float(i + 1));
        std::wstring id = L"pw:" + std::to_wstring(i);
        float hv = Hover(c, id, hover_ == id || powerSel_ == i);
        if (hv > 0)
            c.Fill(row, 14, powerSel_ == i && hover_ != id ? Alpha(t.accent, 0.24f * hv) : Alpha(t.hover, hv));
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
    // Last painted is on top (the power card over the panel).
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        const Hit& h = *it;
        if (!In(h.rect, x, y))
            continue;
        // Rows scrolled out of the list are not clickable.
        if (h.id.rfind(L"app:", 0) == 0 && (y < kListTop || y > kListBottom))
            continue;
        return h.id;
    }
    return {};
}

void StartMenu::Launch(const CatalogApp& app)
{
    AppCatalog::Launch(app);
    Close(true);
}

void StartMenu::RunPower(int index)
{
    if (index < 0 || index > 3)
        return;
    Close(true);
    if (index == 3) { // Windows' own lock
        if (onSystem)
            onSystem(L"lock");
        return;
    }
    RunPowerAction(index == 0 ? PowerAction::Sleep : index == 1 ? PowerAction::Restart : PowerAction::Shutdown);
}

void StartMenu::Activate(const std::wstring& id)
{
    if (id.rfind(L"pin:", 0) == 0 || id.rfind(L"app:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(4)));
        const auto& list = id[0] == L'p' ? pins_ : results_;
        if (i < list.size())
            Launch(list[i]);
    } else if (id.rfind(L"page:", 0) == 0) {
        SetPage(std::stoi(id.substr(5)));
    } else if (id == L"clear") {
        query_.clear();
        results_.clear();
        sel_ = -1;
        Restage();
        surface_.Invalidate();
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
    const auto& list = id[0] == L'p' ? pins_ : results_;
    if (i >= list.size())
        return;
    CatalogApp app = list[i];
    if (!menu)
        return;
    menuOpen_ = true;
    menu->Open({{L"打开", 1}, {L"固定到任务栏", 2}}, screen, MenuPlace::Below,
               [this, app](int cmd) {
                   if (cmd == 1)
                       Launch(app);
                   else if (cmd == 2 && onPin)
                       onPin(AppCatalog::PinFor(app));
               },
               [this] {
                   menuOpen_ = false;
                   if (!surface_.Visible())
                       return;
                   // Back to the start menu, unless the click that closed the menu went to another window.
                   HWND fg = GetForegroundWindow();
                   if (!fg || fg == surface_.Hwnd() || fg == menu->Hwnd())
                       SetForegroundWindow(surface_.Hwnd());
                   else
                       Close(false);
               });
    surface_.Invalidate();
}

void StartMenu::MoveSelection(int dx, int dy)
{
    const auto& list = Shown();
    if (list.empty())
        return;
    int n = int(list.size());
    int next;
    if (!query_.empty()) // rows; with nothing selected the first row counts as selected
        next = (sel_ < 0 ? 0 : sel_) + dy + dx;
    else if (sel_ < 0)
        next = page_ * kPerPage; // the first tile of the page shown
    else if (int to = sel_ + dx + dy * kCols; to >= 0 && to < n)
        next = to;
    else
        next = dx ? std::clamp(to, 0, n - 1) : sel_; // up/down past the grid stays
    sel_ = std::clamp(next, 0, n - 1);
    if (query_.empty())
        SetPage(sel_ / kPerPage);
    EnsureVisible();
    surface_.Invalidate();
}

void StartMenu::EnsureVisible()
{
    if (sel_ < 0 || query_.empty())
        return;
    const float viewH = kListBottom - kListTop, top = float(sel_) * kRowH;
    if (top < scrollTarget_)
        scrollTarget_ = top;
    else if (top + kRowH > scrollTarget_ + viewH)
        scrollTarget_ = top + kRowH - viewH;
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
        cf.ptCurrentPos = POINT{origin.x + LONG((kSearchX + 46) * s), origin.y + LONG((kSearchTop + kSearchH + 4) * s)};
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
            if (onStatusRequest)
                onStatusRequest();
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
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT client = surface_.ScreenToContent(pt);
        float notches = float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;
        if (m == WM_MOUSEWHEEL && HitAt(client) == L"volume") {
            status_.volume = AdjustVolume(int(std::lround(2 * notches)));
            if (notches > 0)
                status_.muted = false;
            if (onStatusRequest)
                onStatusRequest();
        } else if (!query_.empty()) {
            if (m == WM_MOUSEWHEEL)
                scrollTarget_ = std::clamp(scrollTarget_ - notches * 2 * kRowH, 0.f, std::max(0.f, contentH_ - (kListBottom - kListTop)));
        } else {
            // A page per notch (touchpads: per notch's worth of small steps);
            // the wheel turned down, or a swipe to the right, goes forward.
            wheelAcc_ += m == WM_MOUSEWHEEL ? -notches : notches;
            if (std::fabs(wheelAcc_) >= 1) {
                SetPage(page_ + (wheelAcc_ > 0 ? 1 : -1));
                wheelAcc_ = 0;
            }
        }
        surface_.Invalidate();
        return true;
    }
    case WM_IME_STARTCOMPOSITION:
        PlaceIme();
        return false;
    case WM_KEYDOWN:
        if (powerOpen_ && (w == VK_UP || w == VK_DOWN)) {
            powerSel_ = std::clamp(powerSel_ < 0 ? 0 : powerSel_ + (w == VK_DOWN ? 1 : -1), 0, 3);
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
        case VK_PRIOR:
        case VK_NEXT:
            if (query_.empty()) {
                SetPage(page_ + (w == VK_NEXT ? 1 : -1));
                return true;
            }
            return false;
        }
        return false;
    case WM_CHAR: {
        wchar_t ch = wchar_t(w);
        if (ch == 27) { // Esc: fold the power card, clear the search, then close
            if (powerOpen_) {
                powerOpen_ = false;
                surface_.Invalidate();
            } else if (!query_.empty()) {
                Activate(L"clear");
            } else {
                Close(false);
            }
        } else if (ch == 13) {
            if (powerOpen_) {
                RunPower(powerSel_);
            } else {
                const auto& list = Shown();
                int i = sel_ >= 0 ? sel_ : query_.empty() ? -1 : 0;
                if (i >= 0 && i < int(list.size()))
                    Launch(list[size_t(i)]);
            }
        } else if (ch == 8) {
            if (!query_.empty()) {
                query_.pop_back();
                results_ = Results();
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
            results_ = Results();
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
