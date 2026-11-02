#include "ui/glass_menu.h"

#include "log.h"
#include "render/ui_font.h"

#include <windowsx.h>

#include <algorithm>

namespace atrium {

namespace {

// Rows inset from the glass by kPad; their plates concentric with its corners.
constexpr float kRadius = 14, kPad = 6, kRowH = 32, kSepH = 11, kHeaderH = 28, kTextPx = 13.5f, kGutter = 24, kMinW = 200, kMaxW = 360;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

bool Choosable(const MenuEntry& e) { return !e.text.empty() && !e.disabled && !e.header; }

} // namespace

GlassMenu::GlassMenu(HINSTANCE instance) : surface_(instance, L"Atrium Menu", true)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(); };
    surface_.SetRadius(kRadius);
}

D2D1_SIZE_F GlassMenu::Size(float scale) const
{
    winrt::com_ptr<IDWriteFactory> dw;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put()));
    float text = 0, h = 2 * kPad;
    for (const MenuEntry& e : entries_) {
        h += e.text.empty() ? kSepH : e.header ? kHeaderH : kRowH;
        if (!e.text.empty())
            text = std::max(text, MeasureUiText(dw.get(), e.text, kTextPx * scale, e.header ? DWRITE_FONT_WEIGHT_MEDIUM : DWRITE_FONT_WEIGHT_NORMAL) / scale);
    }
    float w = std::clamp(std::ceil(text) + 2 * kPad + 2 * 12 + (marks_ ? kGutter : 0), kMinW, kMaxW);
    return D2D1::SizeF(w, h);
}

void GlassMenu::Open(std::vector<MenuEntry> entries, POINT at, MenuPlace place, std::function<void(int)> onPick, std::function<void()> onClosed)
{
    if (surface_.Visible()) {
        // Replaced by another menu: the old one closes as if dismissed (its owner hears of it).
        auto closed = std::move(onClosed_);
        onClosed_ = nullptr;
        onPick_ = nullptr;
        surface_.Hide(false);
        if (closed)
            closed();
    }
    entries_ = std::move(entries);
    scroll_ = 0;
    onPick_ = std::move(onPick);
    onClosed_ = std::move(onClosed);
    hover_ = pressed_ = -1;
    hoverAnim_.clear();
    marks_ = std::any_of(entries_.begin(), entries_.end(), [](const MenuEntry& e) { return e.checked || !e.icon.empty(); });

    RECT probe{at.x, at.y, at.x + 1, at.y + 1};
    const float s = PopupScale(probe);
    D2D1_SIZE_F size = Size(s);
    contentH_ = size.height;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    // Taller than the work area (a long overflow list): it scrolls.
    LONG w = LONG(std::lround(size.width * s)), gap = LONG(std::lround(4 * s));
    LONG h = std::min(LONG(std::lround(size.height * s)), LONG(work.bottom - work.top) - 2 * gap);
    LONG x = place == MenuPlace::AboveCentered ? at.x - w / 2 : at.x;
    LONG y = place == MenuPlace::Below ? at.y + gap : at.y - gap - h;
    if (place == MenuPlace::Below && y + h > work.bottom)
        y = at.y - gap - h; // no room below: above the point
    if (place != MenuPlace::Below && y < work.top)
        y = at.y + gap;
    if (x + w > work.right)
        x = place == MenuPlace::AboveCentered ? work.right - w : at.x - w;
    x = std::max(x, work.left);
    y = std::clamp(y, work.top, std::max(work.top, work.bottom - h));
    surface_.Show(RECT{x, y, x + w, y + h}, true, at);
    Log(L"menu: opened (%zu rows)", entries_.size());
}

void GlassMenu::Close()
{
    if (!surface_.Visible())
        return;
    surface_.Hide();
    auto closed = std::move(onClosed_);
    onClosed_ = nullptr;
    onPick_ = nullptr;
    if (closed)
        closed();
}

void GlassMenu::Pick(int index)
{
    if (index < 0 || index >= int(entries_.size()) || !Choosable(entries_[size_t(index)]))
        return;
    int id = entries_[size_t(index)].id;
    auto pick = std::move(onPick_);
    auto closed = std::move(onClosed_);
    onPick_ = nullptr;
    onClosed_ = nullptr;
    surface_.Hide();
    Log(L"menu: picked %d", id);
    if (pick)
        pick(id);
    if (closed)
        closed();
}

void GlassMenu::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    RECT rr = surface_.Rect();
    const float w = float(rr.right - rr.left) / c.scale, h = float(rr.bottom - rr.top) / c.scale;
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, w, h), kRadius, t.panel);
    rows_.assign(entries_.size(), D2D1::RectF(0, 0, 0, 0));
    const float plateR = kRadius - kPad;
    viewH_ = h;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, contentH_ - h));
    c.Clip(D2D1::RectF(0, kPad / 2, w, h - kPad / 2));
    float y = kPad - scroll_;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const MenuEntry& e = entries_[i];
        if (e.text.empty()) {
            c.Line(kPad + 12, y + kSepH / 2, w - kPad - 12, y + kSepH / 2, t.divider);
            y += kSepH;
            continue;
        }
        const float rowH = e.header ? kHeaderH : kRowH;
        D2D1_RECT_F row = D2D1::RectF(kPad, y, w - kPad, y + rowH);
        rows_[i] = row;
        y += rowH;
        float textLeft = row.left + 12 + (marks_ ? kGutter : 0);
        if (e.header) {
            c.Text(e.text, D2D1::RectF(textLeft, row.top, row.right - 12, row.bottom), 12, t.subtext, DWRITE_FONT_WEIGHT_MEDIUM);
            continue;
        }
        auto& f = hoverAnim_[int(i)];
        float hv = f.Step(int(i) == hover_ && !e.disabled, c.now);
        if (f.moving)
            c.Continue();
        if (hv > 0.001f)
            c.Fill(row, plateR, Alpha(t.hover, hv));
        if (int(i) == pressed_ && !e.disabled)
            c.Fill(row, plateR, t.press);
        const D2D1_COLOR_F ink = e.disabled ? t.faint : t.text;
        float cy = (row.top + row.bottom) / 2;
        if (!e.icon.empty())
            c.Image(L"menu|" + std::to_wstring(e.id), e.icon, D2D1::RectF(row.left + 10, cy - 8, row.left + 26, cy + 8), e.disabled ? 0.45f : 1);
        else if (e.checked)
            c.Glyph(L"\uE73E", D2D1::RectF(row.left + 8, cy - 9, row.left + 26, cy + 9), 12, ink);
        c.Text(e.text, D2D1::RectF(textLeft, row.top, row.right - 12, row.bottom), kTextPx, ink);
    }
    c.Unclip();
}

int GlassMenu::RowAt(POINT pt) const
{
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const D2D1_RECT_F& r = rows_[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom && Choosable(entries_[i]))
            return int(i);
    }
    return -1;
}

void GlassMenu::Move(int step)
{
    const int n = int(entries_.size());
    if (n == 0)
        return;
    int i = hover_;
    for (int tries = 0; tries < n; ++tries) {
        i = i < 0 ? (step > 0 ? 0 : n - 1) : (i + step + n) % n;
        if (Choosable(entries_[size_t(i)])) {
            hover_ = i;
            // Keep it in view (rows_ are where the last frame drew them).
            const D2D1_RECT_F& r = rows_[size_t(i)];
            if (r.top < kPad)
                scroll_ -= kPad - r.top;
            else if (r.bottom > viewH_ - kPad)
                scroll_ += r.bottom - (viewH_ - kPad);
            surface_.Invalidate();
            return;
        }
    }
}

bool GlassMenu::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && surface_.Visible())
            Close();
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        int h = RowAt(pt);
        if (h != hover_) {
            hover_ = h;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hover_ = pressed_ = -1;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        pressed_ = RowAt(pt);
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        int h = RowAt(pt);
        bool same = h >= 0 && h == pressed_;
        pressed_ = -1;
        surface_.Invalidate();
        if (same)
            Pick(h);
        return true;
    }
    case WM_MOUSEWHEEL:
        scroll_ -= float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA * kRowH * 3;
        surface_.Invalidate();
        return true;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
            Close();
        else if (w == VK_DOWN || w == VK_UP)
            Move(w == VK_DOWN ? 1 : -1);
        else if (w == VK_RETURN || w == VK_SPACE)
            Pick(hover_);
        else
            return false;
        return true;
    }
    (void)result;
    return false;
}

} // namespace atrium
