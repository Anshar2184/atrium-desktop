#include "ui/start_menu.h"

#include "log.h"

#include <imm.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "imm32.lib")

namespace atrium {

namespace {

constexpr float kW = 600, kH = 640;      // DIP
constexpr float kPad = 20, kRadius = 22;  // outer padding; corner radius (matches the bar's language)
constexpr float kSearchTop = 20, kSearchH = 40;
constexpr float kHeaderTop = 76, kContentTop = 112, kFooterH = 68;
constexpr int kCols = 6;
constexpr float kTileH = 88, kRowH = 44;
constexpr UINT_PTR kStatusTimer = 1;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

} // namespace

StartMenu::StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog)
    : surface_(instance, L"Atrium Start", true), tasks_(tasks), catalog_(catalog)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
}

float StartMenu::W() const { return float(surface_.Rect().right - surface_.Rect().left) / surface_.Scale(); }
float StartMenu::H() const { return float(surface_.Rect().bottom - surface_.Rect().top) / surface_.Scale(); }

void StartMenu::Open(RECT bar, HWND previous)
{
    previous_ = previous;
    query_.clear();
    view_ = View::Pinned;
    scroll_ = 0;
    sel_ = -1;
    hover_.clear();
    apps_ = catalog_.Apps();
    catalog_.Refresh(); // new installs since last time; the list updates when ready
    status_ = QuerySystemStatus();
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    float s = float(dpi) / 96.f;
    const RECT& wa = mi.rcWork;
    int w = int(std::lround(kW * s)), h = int(std::lround(kH * s)), gap = int(std::lround(10 * s));
    int bottom = bar.top - gap;
    h = std::min(h, int(bottom - wa.top - gap)); // short screens: shorter menu, the lists scroll
    int left = std::clamp(int(bar.left), int(wa.left) + gap, int(wa.right) - gap - w);
    surface_.Show(RECT{left, bottom - h, left + w, bottom}, true);
    SetTimer(surface_.Hwnd(), kStatusTimer, 2000, nullptr);
    Log(L"start: opened (%zu apps, %s)", apps_.size(), catalog_.Loaded() ? L"catalog ready" : L"catalog loading");
}

void StartMenu::Close(bool launched)
{
    if (!surface_.Visible())
        return;
    KillTimer(surface_.Hwnd(), kStatusTimer);
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
    std::wstring spots; // DIP centres of the fixed buttons, for tests
    for (const Hit& h : hits_)
        if (h.id == L"toggle" || h.id == L"tray" || h.id == L"power" || h.id == L"network" || h.id == L"volume")
            spots += L" " + h.id + L"@" + std::to_wstring(int((h.rect.left + h.rect.right) / 2)) + L"," + std::to_wstring(int((h.rect.top + h.rect.bottom) / 2));
    Log(L"start: spots%s", spots.c_str());
    Log(L"start: state visible=%d foreground=%d view=%s query='%s' entries=%zu first='%s' catalog=%zu", surface_.Visible() ? 1 : 0,
        GetForegroundWindow() == surface_.Hwnd() ? 1 : 0, view_ == View::All ? L"all" : L"pinned", query_.c_str(), entries.size(),
        entries.empty() ? L"" : entries[0].name.c_str(), apps_.size());
}

void StartMenu::Refresh()
{
    if (!surface_.Visible())
        return;
    apps_ = catalog_.Apps();
    surface_.Invalidate();
}

std::vector<StartMenu::Entry> StartMenu::Entries() const
{
    std::vector<Entry> out;
    if (query_.empty() && view_ == View::Pinned) {
        for (const TaskItem& t : tasks_.Snapshot())
            if (t.pinned)
                out.push_back({t.key, t.name, true, 0});
        return out;
    }
    for (size_t i = 0; i < apps_.size(); ++i)
        if (AppCatalog::Matches(apps_[i], query_))
            out.push_back({apps_[i].id, apps_[i].name, false, i});
    if (!query_.empty()) {
        // Names that start with the query first, then the other matches.
        std::stable_partition(out.begin(), out.end(), [&](const Entry& e) {
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, e.name.c_str(), int(std::min(e.name.size(), query_.size())),
                                   query_.c_str(), int(query_.size()), nullptr, nullptr, 0) == CSTR_EQUAL;
        });
    }
    return out;
}

void StartMenu::Paint(Canvas& c)
{
    const Theme t = Theme::Current();
    const float w = W(), h = H();
    hits_.clear();
    c.Fill(D2D1::RectF(0, 0, w, h), kRadius, t.panel);
    c.Stroke(D2D1::RectF(0, 0, w, h), kRadius, t.border);

    // Search field: a capsule, like the bar.
    D2D1_RECT_F search = D2D1::RectF(kPad, kSearchTop, w - kPad, kSearchTop + kSearchH);
    c.Fill(search, kSearchH / 2, t.hover);
    c.Stroke(search, kSearchH / 2, query_.empty() ? t.divider : Alpha(t.accent, 0.8f));
    c.Glyph(L"\uE721", D2D1::RectF(search.left + 8, search.top, search.left + 40, search.bottom), 15, t.subtext);
    D2D1_RECT_F field = D2D1::RectF(search.left + 42, search.top, search.right - 16, search.bottom);
    if (query_.empty()) {
        c.Text(L"搜索应用", field, 14, t.faint); // 搜索应用
    } else {
        c.Text(query_, field, 14, t.text);
    }
    float caretX = field.left + std::min(c.TextWidth(query_, 14), field.right - field.left - 2) + 1;
    c.Line(caretX, search.top + 11, caretX, search.bottom - 11, t.text, 1.2f);

    // Header: what the area below shows, and the switch between pinned and all apps.
    std::wstring title = !query_.empty() ? L"搜索结果" : view_ == View::All ? L"全部应用" : L"已固定";
    c.Text(title, D2D1::RectF(kPad + 8, kHeaderTop, w / 2, kHeaderTop + 24), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (query_.empty()) {
        std::wstring label = view_ == View::All ? L"\u2039  已固定" : L"全部应用  \u203A"; // \u2039 已固定 / 全部应用 \u203A
        float lw = c.TextWidth(label, 12) + 24;
        D2D1_RECT_F b = D2D1::RectF(w - kPad - lw, kHeaderTop - 2, w - kPad, kHeaderTop + 26);
        c.Fill(b, 14, hover_ == L"toggle" ? t.press : t.hover);
        c.Text(label, b, 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
        hits_.push_back({b, L"toggle"});
    }

    // Content.
    const float top = kContentTop, bottom = h - kFooterH - 4;
    viewH_ = bottom - top;
    std::vector<Entry> entries = Entries();
    const bool grid = query_.empty() && view_ == View::Pinned;
    contentH_ = grid ? std::ceil(float(entries.size()) / kCols) * kTileH : float(entries.size()) * kRowH;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, contentH_ - viewH_));
    c.Clip(D2D1::RectF(0, top, w, bottom));
    const int iconPx = int(std::lround((grid ? 32 : 24) * c.scale));
    if (entries.empty()) {
        std::wstring msg = !query_.empty() ? L"没有找到应用" // 没有找到应用
                           : grid           ? L"还没有固定的应用。右键\u201C全部应用\u201D里的应用可以固定。"
                           : catalog_.Loaded() ? L"没有应用" : L"正在读取应用\u2026";
        c.Text(msg, D2D1::RectF(kPad, top + 40, w - kPad, top + 64), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
    const float cellW = (w - 2 * kPad) / kCols;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        std::wstring id = (e.pinned ? L"pin:" : L"app:") + std::to_wstring(i);
        D2D1_RECT_F r;
        if (grid)
            r = D2D1::RectF(kPad + cellW * float(i % kCols), top + kTileH * float(i / kCols) - scroll_, kPad + cellW * float(i % kCols + 1),
                            top + kTileH * float(i / kCols + 1) - scroll_);
        else
            r = D2D1::RectF(kPad, top + kRowH * float(i) - scroll_, w - kPad, top + kRowH * float(i + 1) - scroll_);
        if (r.bottom < top || r.top > bottom)
            continue;
        D2D1_RECT_F plate = D2D1::RectF(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2);
        bool selected = int(i) == sel_ || (sel_ < 0 && i == 0 && !query_.empty());
        if (hover_ == id || selected)
            c.Fill(plate, 12, pressed_ == id ? t.press : selected && hover_ != id ? Alpha(t.accent, 0.16f) : t.hover);
        IconImage icon = e.pinned ? tasks_.Icon(e.key, iconPx) : catalog_.Icon(e.key, iconPx);
        std::wstring cacheKey = (e.pinned ? L"p|" : L"c|") + e.key + L"|" + std::to_wstring(iconPx);
        if (grid) {
            float cx = (r.left + r.right) / 2;
            D2D1_RECT_F ir = D2D1::RectF(cx - 16, r.top + 14, cx + 16, r.top + 46);
            if (icon.empty())
                c.Fill(ir, 8, t.hover);
            else
                c.Image(cacheKey, icon, ir);
            c.Text(e.name, D2D1::RectF(r.left + 6, r.top + 52, r.right - 6, r.top + 72), 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
        } else {
            D2D1_RECT_F ir = D2D1::RectF(r.left + 12, r.top + 10, r.left + 36, r.top + 34);
            if (icon.empty())
                c.Fill(ir, 6, t.hover);
            else
                c.Image(cacheKey, icon, ir);
            c.Text(e.name, D2D1::RectF(r.left + 50, r.top, r.right - 12, r.bottom), 14, t.text);
        }
        hits_.push_back({r, id});
    }
    c.Unclip();
    // Scroll indicator.
    if (contentH_ > viewH_ + 1) {
        float th = std::max(24.f, viewH_ * viewH_ / contentH_);
        float ty = top + (viewH_ - th) * (scroll_ / (contentH_ - viewH_));
        c.Fill(D2D1::RectF(w - 9, ty, w - 6, ty + th), 1.5f, t.faint);
    }

    // Footer: network, volume, battery, tray; power on the right.
    const float fy = h - kFooterH;
    c.Line(kPad, fy, w - kPad, fy, t.divider);
    const float by = fy + (kFooterH - 36) / 2;
    float x = kPad;
    auto button = [&](const std::wstring& id, const std::wstring& glyph, const std::wstring& label, bool dim) {
        float tw = label.empty() ? 0 : c.TextWidth(label, 12) + 6;
        D2D1_RECT_F b = D2D1::RectF(x, by, x + 16 + 18 + tw + 14, by + 36);
        c.Fill(b, 18, hover_ == id ? (pressed_ == id ? t.press : t.hover) : D2D1::ColorF(0, 0, 0, 0));
        c.Glyph(glyph, D2D1::RectF(b.left + 10, b.top, b.left + 34, b.bottom), 15, dim ? t.faint : t.text);
        if (!label.empty())
            c.Text(label, D2D1::RectF(b.left + 38, b.top, b.right - 8, b.bottom), 12, dim ? t.faint : t.subtext);
        hits_.push_back({b, id});
        x = b.right + 4;
    };
    std::wstring net = status_.connected ? (status_.network.empty() ? L"已连接" : status_.network) : L"未连接";
    if (net.size() > 14)
        net = net.substr(0, 13) + L"\u2026";
    button(L"network", status_.connected ? L"\uE701" : L"\uEB55", net, !status_.connected);
    if (status_.audio)
        button(L"volume", status_.muted ? L"\uE74F" : status_.volume < 34 ? L"\uE993" : status_.volume < 67 ? L"\uE994" : L"\uE995",
               status_.muted ? L"静音" : std::to_wstring(status_.volume) + L"%", status_.muted);
    if (status_.battery) {
        int level = std::clamp(status_.batteryPercent / 10, 0, 10);
        std::wstring glyph(1, wchar_t((status_.charging ? 0xEBAB : 0xEBA0) + level));
        button(L"battery", glyph, std::to_wstring(status_.batteryPercent) + L"%", false);
    }
    button(L"tray", L"\uE70E", L"托盘", false); // 托盘
    D2D1_RECT_F power = D2D1::RectF(w - kPad - 36, by, w - kPad, by + 36);
    c.Fill(power, 18, hover_ == L"power" ? (pressed_ == L"power" ? t.press : t.hover) : D2D1::ColorF(0, 0, 0, 0));
    c.Glyph(L"\uE7E8", power, 15, t.text);
    hits_.push_back({power, L"power"});
}

std::wstring StartMenu::HitAt(POINT pt) const
{
    float s = surface_.Scale();
    float x = float(pt.x) / s, y = float(pt.y) / s;
    for (const Hit& h : hits_)
        if (In(h.rect, x, y)) {
            // Rows scrolled under the header or footer are not clickable.
            bool content = h.id.rfind(L"pin:", 0) == 0 || h.id.rfind(L"app:", 0) == 0;
            if (content && (y < kContentTop || y > H() - kFooterH))
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

void StartMenu::Activate(const std::wstring& id)
{
    if (id == L"toggle") {
        view_ = view_ == View::All ? View::Pinned : View::All;
        scroll_ = 0;
        sel_ = -1;
        surface_.Invalidate();
    } else if (id.rfind(L"pin:", 0) == 0 || id.rfind(L"app:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(4)));
        auto entries = Entries();
        if (i < entries.size())
            Launch(entries[i]);
    } else if (id == L"power") {
        PowerMenu();
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
    AppendMenuW(menu, MF_STRING, 1, L"打开"); // 打开
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

void StartMenu::PowerMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"睡眠");   // 睡眠
    AppendMenuW(menu, MF_STRING, 2, L"重启");   // 重启
    AppendMenuW(menu, MF_STRING, 3, L"关机");   // 关机
    RECT r = surface_.Rect();
    float s = surface_.Scale();
    menuOpen_ = true;
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, r.right - int(kPad * s),
                                r.bottom - int((kFooterH - 8) * s), surface_.Hwnd(), nullptr);
    menuOpen_ = false;
    DestroyMenu(menu);
    if (cmd == 0)
        return;
    Close(true);
    RunPowerAction(cmd == 1 ? PowerAction::Sleep : cmd == 2 ? PowerAction::Restart : PowerAction::Shutdown);
}

void StartMenu::MoveSelection(int dx, int dy)
{
    auto entries = Entries();
    if (entries.empty())
        return;
    const bool grid = query_.empty() && view_ == View::Pinned;
    int n = int(entries.size());
    int cur = sel_ < 0 ? (query_.empty() ? -1 : 0) : sel_;
    int next = cur < 0 ? 0 : grid ? cur + dx + dy * kCols : cur + dy + dx;
    sel_ = std::clamp(next, 0, n - 1);
    EnsureVisible();
    surface_.Invalidate();
}

void StartMenu::EnsureVisible()
{
    if (sel_ < 0)
        return;
    const bool grid = query_.empty() && view_ == View::Pinned;
    float rowH = grid ? kTileH : kRowH;
    float y = rowH * float(grid ? sel_ / kCols : sel_);
    if (y < scroll_)
        scroll_ = y;
    else if (y + rowH > scroll_ + viewH_)
        scroll_ = y + rowH - viewH_;
}

void StartMenu::PlaceIme()
{
    float s = surface_.Scale();
    if (HIMC imc = ImmGetContext(surface_.Hwnd())) {
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = POINT{LONG((kPad + 42) * s), LONG((kSearchTop + kSearchH + 4) * s)};
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
    case WM_RBUTTONUP: {
        POINT screen = pt;
        ClientToScreen(surface_.Hwnd(), &screen);
        ContextMenu(HitAt(pt), screen);
        return true;
    }
    case WM_MOUSEWHEEL: {
        POINT client = pt;
        ScreenToClient(surface_.Hwnd(), &client);
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (HitAt(client) == L"volume") {
            AdjustVolume(2 * notches);
            status_ = QuerySystemStatus();
        } else {
            scroll_ -= float(notches) * 3 * (query_.empty() && view_ == View::Pinned ? kTileH / 3 : kRowH);
        }
        surface_.Invalidate();
        return true;
    }
    case WM_IME_STARTCOMPOSITION:
        PlaceIme();
        return false;
    case WM_KEYDOWN:
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
        if (ch == 27) { // Esc: clear the search, then close
            if (!query_.empty()) {
                query_.clear();
                sel_ = -1;
                surface_.Invalidate();
            } else {
                Close(false);
            }
        } else if (ch == 13) {
            auto entries = Entries();
            int i = sel_ >= 0 ? sel_ : query_.empty() ? -1 : 0;
            if (i >= 0 && i < int(entries.size()))
                Launch(entries[size_t(i)]);
        } else if (ch == 8) {
            if (!query_.empty())
                query_.pop_back();
            sel_ = -1;
            scroll_ = 0;
            surface_.Invalidate();
        } else if (ch >= 32 && query_.size() < 64) {
            query_ += ch;
            sel_ = -1;
            scroll_ = 0;
            surface_.Invalidate();
        }
        result = 0;
        return true;
    }
    }
    return false;
}

} // namespace atrium
