#include "ui/tray_panel.h"

#include "clock.h"
#include "log.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr int kCols = 6;
constexpr float kPad = 14, kCell = 56, kIcon = 32, kLabelH = 44;
constexpr double kStageMs = 240, kStaggerMs = 14, kRippleMs = 520;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
float Ease(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

} // namespace

TrayPanel::TrayPanel(HINSTANCE instance, TrayModel& model) : surface_(instance, L"Atrium Tray", true), model_(model)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onMargin = [this] { Close(); };
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
}

D2D1_SIZE_F TrayPanel::Size() const
{
    int rows = std::max(1, int((icons_.size() + kCols - 1) / kCols));
    return D2D1::SizeF(2 * kPad + kCols * kCell, kPad + float(rows) * kCell + 4 + kLabelH + 6);
}

void TrayPanel::Place()
{
    HMONITOR mon = MonitorFromRect(&bar_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    float s = float(dpi) / 96.f;
    D2D1_SIZE_F size = Size();
    int w = int(std::lround(size.width * s)), h = int(std::lround(size.height * s)), gap = int(std::lround(12 * s));
    int bottom = bar_.top - gap;
    int cx = (anchor_.left + anchor_.right) / 2;
    int left = std::max(int(mi.rcWork.left) + gap, std::min(cx - w / 2, int(mi.rcWork.right) - gap - w));
    RECT r{left, bottom - h, left + w, bottom};
    if (surface_.Visible())
        surface_.Move(r);
    else
        surface_.Show(r, true, POINT{cx, bottom});
}

void TrayPanel::Open(RECT anchor, RECT bar, HWND previous)
{
    previous_ = previous;
    anchor_ = anchor;
    bar_ = bar;
    icons_ = model_.Icons();
    hover_.clear();
    pressed_.clear();
    hoverAnim_.clear();
    openedMs_ = ClockMs();
    model_.Refresh(); // the tooltips and the set change; the panel updates when read
    Place();
    Log(L"tray: panel opened (%zu icons)", icons_.size());
}

void TrayPanel::Close()
{
    if (!surface_.Visible())
        return;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
}

void TrayPanel::Refresh()
{
    if (!surface_.Visible())
        return;
    icons_ = model_.Icons();
    Place();
    surface_.Invalidate();
}

float TrayPanel::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& [v, last] = hoverAnim_[id];
    double dt = last > 0 ? std::clamp(c.now - last, 0.0, 100.0) : 0;
    last = c.now;
    float target = on ? 1.f : 0.f;
    v += (target - v) * (1 - std::exp(-float(dt) / 1000.f * (on ? 20.f : 11.f)));
    if (std::fabs(target - v) < 0.01f)
        v = target;
    else
        c.Continue();
    return v;
}

void TrayPanel::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const D2D1_SIZE_F size = Size();
    const float w = size.width, h = size.height;
    hits_.clear();
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, w, h), radius_, t.panel);

    const float gridTop = kPad, labelTop = h - kLabelH - 6;
    if (icons_.empty()) {
        std::wstring msg = !model_.Loaded() ? L"正在读取…" : model_.Available() ? L"没有托盘图标" : L"读取不到系统托盘";
        c.Text(msg, D2D1::RectF(kPad, gridTop, w - kPad, gridTop + kCell), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
    const int iconPx = int(std::lround(kIcon * c.scale));
    for (size_t i = 0; i < icons_.size(); ++i) {
        const TrayIcon& icon = icons_[i];
        float x = kPad + float(i % kCols) * kCell, y = gridTop + float(i / kCols) * kCell;
        D2D1_RECT_F cell = D2D1::RectF(x, y, x + kCell, y + kCell);
        std::wstring id = L"icon:" + std::to_wstring(i);
        float e = Ease(float((c.now - openedMs_ - kStaggerMs * double(std::min<size_t>(i, 18))) / kStageMs));
        if (e < 1)
            c.Continue();
        c.BeginGroup(e, 1, (cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2, 0, 6 * (1 - e));
        float hv = Hover(c, id, hover_ == id);
        D2D1_RECT_F plate = D2D1::RectF(cell.left + 3, cell.top + 3, cell.right - 3, cell.bottom - 3);
        if (hv > 0.001f)
            c.Fill(plate, 16, Alpha(t.hover, hv));
        if (pressed_ == id)
            c.Fill(plate, 16, t.press);
        if (rippleId_ == id && c.now - rippleMs_ < kRippleMs) {
            c.Ripple(plate, 16, rippleX_, rippleY_, float((c.now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
            c.Continue();
        }
        float cx = (cell.left + cell.right) / 2, cy = (cell.top + cell.bottom) / 2;
        D2D1_RECT_F ir = D2D1::RectF(cx - kIcon / 2, cy - kIcon / 2, cx + kIcon / 2, cy + kIcon / 2);
        c.BeginGroup(1, pressed_ == id ? 0.92f : 1 + 0.08f * hv, cx, cy);
        if (icon.image) {
            wchar_t key[64];
            swprintf_s(key, L"tray|%p|%d", static_cast<const void*>(icon.image.get()), iconPx);
            c.Image(key, *icon.image, ir);
        } else {
            // Not recognised: its initial on a quiet disc.
            std::wstring title = TrayModel::Title(icon.tip);
            c.Fill(ir, kIcon / 2, Alpha(t.hover, 1.4f));
            c.Text(title.empty() ? L"?" : title.substr(0, 1), ir, 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        }
        c.EndGroup();
        c.EndGroup();
        hits_.push_back({cell, id});
    }

    // Below the grid: the name under the pointer, and the system's own tray.
    c.Line(kPad + 6, labelTop, w - kPad - 6, labelTop, t.divider);
    std::wstring label;
    if (hover_.rfind(L"icon:", 0) == 0) {
        size_t i = size_t(std::stoul(hover_.substr(5)));
        if (i < icons_.size())
            label = private_ ? L"托盘图标" : TrayModel::Title(icons_[i].tip);
    }
    if (label.empty())
        label = icons_.empty() ? L"托盘" : std::to_wstring(icons_.size()) + L" 个托盘图标 · 右键打开菜单";
    std::wstring sys = L"系统托盘";
    float bw = c.TextWidth(sys, 12) + 28;
    D2D1_RECT_F b = D2D1::RectF(w - kPad - bw, labelTop + 8, w - kPad, labelTop + kLabelH - 4);
    c.Text(label, D2D1::RectF(kPad + 8, labelTop + 4, b.left - 10, labelTop + kLabelH), 12.5f, hover_.rfind(L"icon:", 0) == 0 ? t.text : t.subtext);
    float hv = Hover(c, L"system", hover_ == L"system");
    c.Fill(b, (b.bottom - b.top) / 2, Alpha(pressed_ == L"system" ? t.press : t.hover, 0.45f + 0.55f * hv));
    c.Text(sys, b, 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
    hits_.push_back({b, L"system"});
}

std::wstring TrayPanel::HitAt(POINT pt) const
{
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (const Hit& h : hits_)
        if (In(h.rect, x, y))
            return h.id;
    return {};
}

void TrayPanel::Activate(const std::wstring& id, bool right)
{
    if (id == L"system") {
        if (!right) {
            Close();
            if (onSystemTray)
                onSystemTray();
        }
        return;
    }
    if (id.rfind(L"icon:", 0) != 0)
        return;
    size_t i = size_t(std::stoul(id.substr(5)));
    if (i >= icons_.size())
        return;
    // We have the foreground: let the app take it for its window or menu.
    AllowSetForegroundWindow(ASFW_ANY);
    model_.Click(icons_[i], right);
    // A left click usually opens the app's window: the panel steps aside. A
    // right click opens the app's menu, which takes the focus and closes it.
    if (!right)
        Close();
}

void TrayPanel::TestClick(const std::wstring& which, bool right)
{
    int index = -1;
    if (!which.empty() && iswdigit(which[0]))
        index = _wtoi(which.c_str());
    else
        for (size_t i = 0; i < icons_.size() && index < 0; ++i)
            if (icons_[i].tip.find(which) != std::wstring::npos)
                index = int(i);
    Log(L"tray: test click on icon %d", index);
    if (index >= 0)
        Activate(L"icon:" + std::to_wstring(index), right);
}

void TrayPanel::LogFind(const std::wstring& which) const
{
    const RECT r = surface_.Rect();
    const float s = surface_.Scale();
    for (size_t i = 0; i < icons_.size() && i < hits_.size(); ++i)
        if (icons_[i].tip.find(which) != std::wstring::npos) {
            const D2D1_RECT_F& h = hits_[i].rect;
            Log(L"tray: found icon:%zu@%d,%d recognised=%d", i, int(float(r.left) + (h.left + h.right) / 2 * s), int(float(r.top) + (h.top + h.bottom) / 2 * s),
                icons_[i].exe.empty() ? 0 : 1);
            return;
        }
    Log(L"tray: not found");
}

void TrayPanel::LogState() const
{
    const RECT r = surface_.Rect();
    const float s = surface_.Scale();
    std::wstring spots;
    for (const Hit& h : hits_)
        spots += L" " + h.id + L"@" + std::to_wstring(int(float(r.left) + (h.rect.left + h.rect.right) / 2 * s)) + L"," +
                 std::to_wstring(int(float(r.top) + (h.rect.top + h.rect.bottom) / 2 * s));
    Log(L"tray: spots%s", spots.c_str());
    int recognised = int(std::count_if(icons_.begin(), icons_.end(), [](const TrayIcon& t) { return !t.exe.empty(); }));
    int pictured = int(std::count_if(icons_.begin(), icons_.end(), [](const TrayIcon& t) { return bool(t.image); }));
    Log(L"tray: state visible=%d foreground=%d icons=%zu recognised=%d pictured=%d available=%d rect=%ld,%ld,%ld,%ld", surface_.Visible() ? 1 : 0,
        GetForegroundWindow() == surface_.Hwnd() ? 1 : 0, icons_.size(), recognised, pictured, model_.Available() ? 1 : 0, r.left, r.top, r.right,
        r.bottom);
}

bool TrayPanel::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
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
    case WM_RBUTTONDOWN:
        pressed_ = HitAt(pt);
        if (!pressed_.empty()) {
            float s = surface_.Scale();
            rippleId_ = pressed_;
            rippleX_ = float(pt.x) / s;
            rippleY_ = float(pt.y) / s;
            rippleMs_ = ClockMs();
            surface_.Ripple(rippleX_, rippleY_);
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        std::wstring h = HitAt(pt);
        bool same = !h.empty() && h == pressed_;
        pressed_.clear();
        surface_.Invalidate();
        if (same)
            Activate(h, m == WM_RBUTTONUP);
        return true;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            Close();
            return true;
        }
        return false;
    }
    (void)result;
    return false;
}

} // namespace atrium
