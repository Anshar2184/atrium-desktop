#include "ui/notifications.h"

#include "log.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr float kW = 384, kPad = 12, kRadius = 20, kCardR = 14, kGap = 8, kPeek = 10, kHeader = 48;
constexpr int kMaxNotices = 50;
constexpr ULONGLONG kMergeMs = 120000;
constexpr UINT_PTR kPopupTimer = 1;

bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

std::wstring Ago(ULONGLONG t)
{
    ULONGLONG s = (GetTickCount64() - t) / 1000;
    if (s < 60)
        return L"刚刚"; // 刚刚
    if (s < 3600)
        return std::to_wstring(s / 60) + L" 分钟前"; // N 分钟前
    return std::to_wstring(s / 3600) + L" 小时前"; // N 小时前
}

float MonitorScale(RECT r)
{
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY);
    UINT x = 96, y = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y);
    return float(x) / 96.f;
}

} // namespace

Notifications::Notifications(HINSTANCE instance)
{
    popup_.surface = std::make_unique<Surface>(instance, L"Atrium Notification", false);
    center_.surface = std::make_unique<Surface>(instance, L"Atrium Notifications", true);
    for (Mode mode : {Mode::Popup, Mode::Center}) {
        Surface& s = *ViewOf(mode).surface;
        s.paint = [this, mode](Canvas& c) { Paint(mode, c); };
        s.message = [this, mode](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(mode, m, w, l, r); };
    }
}

void Notifications::SetOptions(double timeoutSeconds, bool hideBody)
{
    timeout_ = std::clamp(timeoutSeconds, 3.0, 30.0);
    hideBody_ = hideBody;
    // Hidden text also keeps the cards out of screenshots and screen sharing.
    popup_.surface->ExcludeFromCapture(hideBody);
    center_.surface->ExcludeFromCapture(hideBody);
    popup_.surface->Invalidate();
    center_.surface->Invalidate();
}

int Notifications::Unread() const
{
    return int(std::count_if(notices_.begin(), notices_.end(), [](const Notice& n) { return n.unread; }));
}

int Notifications::QuietState() const
{
    if (quietOverride_)
        return quietOverride_;
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&state)))
        return QUNS_ACCEPTS_NOTIFICATIONS;
    return int(state);
}

bool Notifications::BodyHidden() const { return hideBody_ || QuietState() == QUNS_PRESENTATION_MODE; }

float Notifications::CardHeight() const { return 14 + 16 + 6 + 20 + 4 + 36 + 12; }

void Notifications::Post(Notice n)
{
    n.time = GetTickCount64();
    // Repeats of the listener's notices, and bursts of the same title from one
    // app, become one card with a count.
    for (auto it = notices_.begin(); it != notices_.end(); ++it) {
        bool sameExternal = n.externalId && it->externalId == n.externalId && it->source == n.source;
        bool burst = it->appId == n.appId && it->app == n.app && it->title == n.title && n.time - it->time < kMergeMs;
        if (sameExternal)
            return;
        if (burst) {
            Notice merged = *it;
            merged.body = n.body;
            merged.action = n.action.empty() ? merged.action : n.action;
            merged.count += 1;
            merged.time = n.time;
            merged.unread = true;
            notices_.erase(it);
            n = merged;
            Log(L"notify: merged into %llu (x%d)", n.id, n.count);
            break;
        }
    }
    if (!n.id)
        n.id = nextId_++;
    notices_.insert(notices_.begin(), n);
    if (notices_.size() > kMaxNotices)
        notices_.resize(kMaxNotices);
    Log(L"notify: posted %llu from %s (%s)", n.id, n.app.c_str(), n.source.c_str());

    // The open list shows it right away; otherwise it pops up, unless the user
    // is in a full-screen app, presenting or in quiet hours.
    if (center_.surface->Visible()) {
        Changed();
        return;
    }
    int quiet = QuietState();
    if (quiet != QUNS_ACCEPTS_NOTIFICATIONS && quiet != QUNS_APP) {
        Log(L"notify: popup suppressed (user notification state %d)", quiet);
        Changed();
        return;
    }
    std::erase(popupIds_, n.id);
    popupIds_.insert(popupIds_.begin(), n.id);
    Layout(Mode::Popup);
    SetTimer(popup_.surface->Hwnd(), kPopupTimer, UINT(timeout_ * 1000), nullptr);
    Changed();
}

void Notifications::HidePopup(const wchar_t* why)
{
    if (!popup_.surface->Visible())
        return;
    KillTimer(popup_.surface->Hwnd(), kPopupTimer);
    popup_.surface->Hide();
    popupIds_.clear();
    Log(L"notify: popup hidden (%s)", why);
}

void Notifications::ToggleCenter()
{
    if (center_.surface->Visible()) {
        center_.surface->Hide();
        centerClosed_ = GetTickCount64();
        Log(L"notify: center closed");
        return;
    }
    if (GetTickCount64() - centerClosed_ < 400)
        return; // the click on the bell that just closed it
    HidePopup(L"center opened");
    expanded_ = notices_.size() <= 1;
    center_.scroll = 0;
    Layout(Mode::Center);
    // Opening the list marks everything as read.
    for (auto& n : notices_)
        n.unread = false;
    Changed();
    Log(L"notify: center opened (%zu)", notices_.size());
}

void Notifications::Clear()
{
    size_t n = notices_.size();
    notices_.clear();
    popupIds_.clear();
    HidePopup(L"cleared");
    Log(L"notify: cleared %zu", n);
    Changed();
}

void Notifications::Remove(uint64_t id)
{
    std::erase_if(notices_, [&](const Notice& n) { return n.id == id; });
    std::erase(popupIds_, id);
    if (popupIds_.empty())
        HidePopup(L"last card closed");
    Changed();
}

void Notifications::Activate(uint64_t id)
{
    auto it = std::find_if(notices_.begin(), notices_.end(), [&](const Notice& n) { return n.id == id; });
    if (it == notices_.end())
        return;
    Notice n = *it;
    bool ok = onActivate ? onActivate(n) : false;
    Log(L"notify: activated %llu action '%s' %s", n.id, n.action.c_str(), ok ? L"ok" : L"failed");
    HidePopup(L"activated");
    if (center_.surface->Visible()) {
        center_.surface->Hide();
        centerClosed_ = GetTickCount64();
    }
    Remove(id);
}

void Notifications::Changed()
{
    if (onUnread)
        onUnread(Unread());
    if (popup_.surface->Visible() || !popupIds_.empty())
        Layout(Mode::Popup);
    if (center_.surface->Visible())
        Layout(Mode::Center);
}

std::vector<const Notice*> Notifications::Shown(Mode mode) const
{
    std::vector<const Notice*> out;
    if (mode == Mode::Center) {
        for (const auto& n : notices_)
            out.push_back(&n);
    } else {
        for (uint64_t id : popupIds_)
            for (const auto& n : notices_)
                if (n.id == id)
                    out.push_back(&n);
    }
    return out;
}

void Notifications::Layout(Mode mode)
{
    View& v = ViewOf(mode);
    auto shown = Shown(mode);
    if (mode == Mode::Popup && shown.empty()) {
        HidePopup(L"empty");
        return;
    }
    RECT bell = bellRect ? bellRect() : RECT{};
    RECT bar = barRect ? barRect() : bell;
    float s = MonitorScale(bar);
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;

    const size_t n = shown.size();
    const float card = CardHeight();
    float content;
    if (n == 0)
        content = 64;
    else if (mode == Mode::Popup || (!expanded_ && n > 1))
        content = card + kPeek * float(std::min<size_t>(2, n - 1));
    else
        content = float(n) * card + float(n - 1) * kGap;
    float header = mode == Mode::Center ? kHeader : 0;
    int gap = int(std::lround(10 * s)), margin = int(std::lround(8 * s));
    int bottom = bar.top - gap;
    float maxContent = float(bottom - wa.top - margin) / s - header - 2 * kPad;
    v.contentH = content;
    v.viewH = std::min(content, std::max(card, maxContent));
    v.scroll = std::clamp(v.scroll, 0.f, std::max(0.f, v.contentH - v.viewH));
    int w = int(std::lround(kW * s)), h = int(std::lround((header + v.viewH + 2 * kPad) * s));
    // Right-aligned with the bell, inside the work area.
    int right = std::clamp(int(bell.right) + int(std::lround(14 * s)), int(wa.left) + margin + w, int(wa.right) - margin);
    RECT r{right - w, std::max(int(wa.top) + margin, bottom - h), right, bottom};
    if (!v.surface->Visible())
        v.surface->Show(r, mode == Mode::Center);
    else
        v.surface->Move(r);
}

float Notifications::PaintCard(Canvas& c, const Notice& n, float x, float y, float w, int index, View& v, bool full)
{
    const Theme t = Theme::Current();
    const float h = CardHeight();
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    // Cards behind the newest one are a shade darker so their edges read as a stack.
    D2D1_COLOR_F fill = full ? (t.light ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(1, 1, 1, 0.065f))
                             : (t.light ? D2D1::ColorF(0.86f, 0.86f, 0.89f, 0.95f) : D2D1::ColorF(1, 1, 1, 0.035f));
    std::wstring cardId = L"card:" + std::to_wstring(index), closeId = L"close:" + std::to_wstring(index);
    bool hot = v.hover == cardId || v.hover == closeId;
    c.Fill(r, kCardR, hot && full ? (t.light ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(1, 1, 1, 0.10f)) : fill);
    c.Stroke(r, kCardR, t.divider);
    if (!full)
        return h;
    float cx = x + 14, cy = y + 14;
    if (n.unread)
        c.Circle(cx + 3, cy + 8, 3.5f, t.accent);
    float nameX = cx + (n.unread ? 12 : 0);
    std::wstring app = n.app.empty() ? L"Atrium" : n.app;
    if (n.count > 1)
        app += L"  ·  " + std::to_wstring(n.count) + L" 条"; // · N 条
    c.Text(app, D2D1::RectF(nameX, cy, x + w - 120, cy + 16), 12, t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(Ago(n.time), D2D1::RectF(x + w - 120, cy, x + w - 44, cy + 16), 12, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
    c.Text(n.title, D2D1::RectF(cx, cy + 22, x + w - 14, cy + 42), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::wstring body = BodyHidden() ? L"内容已隐藏" : n.body; // 内容已隐藏
    c.Text(body, D2D1::RectF(cx, cy + 46, x + w - 14, cy + 46 + 36), 13, BodyHidden() ? t.faint : t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    // Dismiss: a round button in the top-right corner, shown with the pointer on the card.
    D2D1_RECT_F close = D2D1::RectF(x + w - 36, y + 8, x + w - 10, y + 34);
    if (hot) {
        c.Fill(close, 13, v.hover == closeId ? t.press : t.hover);
        c.Glyph(L"\uE711", close, 10, t.text);
    }
    v.hits.push_back({close, closeId});
    return h;
}

void Notifications::Paint(Mode mode, Canvas& c)
{
    View& v = ViewOf(mode);
    const Theme t = Theme::Current();
    RECT rr = v.surface->Rect();
    const float w = float(rr.right - rr.left) / c.scale, h = float(rr.bottom - rr.top) / c.scale;
    v.hits.clear();
    c.Fill(D2D1::RectF(0, 0, w, h), kRadius, t.panel);
    c.Stroke(D2D1::RectF(0, 0, w, h), kRadius, t.border);
    auto shown = Shown(mode);
    float top = kPad;
    if (mode == Mode::Center) {
        c.Text(L"通知", D2D1::RectF(kPad + 8, kPad, w / 2, kPad + 32), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD); // 通知
        float x = w - kPad;
        auto button = [&](const std::wstring& id, const std::wstring& label) {
            float bw = c.TextWidth(label, 12) + 24;
            D2D1_RECT_F b = D2D1::RectF(x - bw, kPad + 2, x, kPad + 30);
            c.Fill(b, 14, v.hover == id ? t.press : t.hover);
            c.Text(label, b, 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
            v.hits.push_back({b, id});
            x = b.left - 6;
        };
        if (!shown.empty())
            button(L"clear", L"全部清除"); // 全部清除
        if (expanded_ && shown.size() > 1)
            button(L"collapse", L"收起"); // 收起
        top += kHeader;
    }
    const float cw = w - 2 * kPad;
    if (shown.empty()) {
        c.Text(L"没有新通知", D2D1::RectF(kPad, top, w - kPad, top + 64), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1); // 没有新通知
        return;
    }
    c.Clip(D2D1::RectF(0, top, w, top + v.viewH));
    const bool stack = mode == Mode::Popup || (!expanded_ && shown.size() > 1);
    if (stack) {
        // Collapsed: older cards peek out below the newest one.
        size_t behind = std::min<size_t>(2, shown.size() - 1);
        for (size_t k = behind; k >= 1; --k) {
            float inset = 10.f * float(k);
            PaintCard(c, *shown[k], kPad + inset, top + kPeek * float(k), cw - 2 * inset, int(k), v, false);
        }
        PaintCard(c, *shown[0], kPad, top, cw, 0, v, true);
        D2D1_RECT_F all = D2D1::RectF(kPad, top, kPad + cw, top + v.contentH);
        // One card: clicking it runs its action. Several: clicking expands the list.
        v.hits.push_back({all, shown.size() > 1 ? L"stack" : L"card:0"});
    } else {
        float y = top - v.scroll;
        for (size_t i = 0; i < shown.size(); ++i) {
            float ch = CardHeight();
            if (y + ch >= top && y <= top + v.viewH) {
                PaintCard(c, *shown[i], kPad, y, cw, int(i), v, true);
                v.hits.push_back({D2D1::RectF(kPad, y, kPad + cw, y + ch), L"card:" + std::to_wstring(i)});
            }
            y += ch + kGap;
        }
    }
    c.Unclip();
}

void Notifications::Click(Mode mode, const std::wstring& id)
{
    auto shown = Shown(mode);
    auto index = [&](size_t prefix) { return size_t(std::stoul(id.substr(prefix))); };
    if (id == L"clear") {
        Clear();
    } else if (id == L"collapse") {
        expanded_ = false;
        Layout(Mode::Center);
    } else if (id == L"stack") {
        if (mode == Mode::Popup) {
            HidePopup(L"expanded");
            ToggleCenter(); // the full list, expanded
            expanded_ = true;
            Layout(Mode::Center);
        } else {
            expanded_ = true;
            Layout(Mode::Center);
        }
        Log(L"notify: expanded");
    } else if (id.rfind(L"close:", 0) == 0) {
        size_t i = index(6);
        if (i < shown.size()) {
            Log(L"notify: closed %llu", shown[i]->id);
            Remove(shown[i]->id);
        }
    } else if (id.rfind(L"card:", 0) == 0) {
        size_t i = index(5);
        if (i < shown.size())
            Activate(shown[i]->id);
    }
}

bool Notifications::Message(Mode mode, UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    View& v = ViewOf(mode);
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    auto hitAt = [&](POINT p) {
        float s = v.surface->Scale(), x = float(p.x) / s, y = float(p.y) / s;
        // Close buttons sit on top of their cards: they are tested first.
        for (const Hit& h : v.hits)
            if (h.id.rfind(L"close:", 0) == 0 && In(h.rect, x, y) && (v.hover == h.id || v.hover == L"card:" + h.id.substr(6)))
                return h.id;
        for (const Hit& h : v.hits)
            if (h.id.rfind(L"close:", 0) != 0 && In(h.rect, x, y))
                return h.id;
        return std::wstring();
    };
    switch (m) {
    case WM_TIMER:
        if (w == kPopupTimer && mode == Mode::Popup) {
            HidePopup(L"timeout");
            return true;
        }
        return false;
    case WM_ACTIVATE:
        if (mode == Mode::Center && LOWORD(w) == WA_INACTIVE && v.surface->Visible()) {
            v.surface->Hide();
            centerClosed_ = GetTickCount64();
            Log(L"notify: center closed (focus moved)");
        }
        return false;
    case WM_MOUSEMOVE: {
        if (!v.tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, v.surface->Hwnd(), 0};
            v.tracking = TrackMouseEvent(&tme) != FALSE;
            if (mode == Mode::Popup)
                KillTimer(v.surface->Hwnd(), kPopupTimer); // reading: do not time out
        }
        std::wstring h = hitAt(pt);
        // Hovering anywhere on a card shows its close button.
        if (h == L"stack")
            h = L"card:0";
        if (h != v.hover) {
            v.hover = h;
            v.surface->Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        v.tracking = false;
        v.hover.clear();
        v.pressed.clear();
        if (mode == Mode::Popup && v.surface->Visible())
            SetTimer(v.surface->Hwnd(), kPopupTimer, UINT(timeout_ * 1000), nullptr);
        v.surface->Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        v.pressed = hitAt(pt);
        return true;
    case WM_LBUTTONUP: {
        std::wstring h = hitAt(pt);
        bool same = !h.empty() && h == v.pressed;
        v.pressed.clear();
        if (same)
            Click(mode, h);
        return true;
    }
    case WM_MOUSEWHEEL:
        v.scroll -= float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA * 60;
        v.scroll = std::clamp(v.scroll, 0.f, std::max(0.f, v.contentH - v.viewH));
        v.surface->Invalidate();
        return true;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && mode == Mode::Center) {
            v.surface->Hide();
            centerClosed_ = GetTickCount64();
            return true;
        }
        return false;
    }
    (void)result;
    return false;
}

void Notifications::LogState() const
{
    for (Mode mode : {Mode::Popup, Mode::Center}) {
        const View& v = ViewOf(mode);
        if (!v.surface->Visible())
            continue;
        std::wstring spots;
        for (const Hit& h : v.hits)
            spots += L" " + h.id + L"@" + std::to_wstring(int((h.rect.left + h.rect.right) / 2)) + L"," +
                     std::to_wstring(int((h.rect.top + h.rect.bottom) / 2));
        Log(L"notify: spots %s%s", mode == Mode::Popup ? L"popup" : L"center", spots.c_str());
    }
    RECT p = popup_.surface->Rect(), c = center_.surface->Rect();
    HWND fg = GetForegroundWindow();
    Log(L"notify: state count=%zu unread=%d popup=%d popupCards=%zu center=%d expanded=%d bodyHidden=%d quiet=%d "
        L"popupRect=%ld,%ld,%ld,%ld centerRect=%ld,%ld,%ld,%ld fgPopup=%d fgCenter=%d first='%s' firstCount=%d",
        notices_.size(), Unread(), popup_.surface->Visible() ? 1 : 0, popupIds_.size(), center_.surface->Visible() ? 1 : 0, expanded_ ? 1 : 0,
        BodyHidden() ? 1 : 0, QuietState(), p.left, p.top, p.right, p.bottom, c.left, c.top, c.right, c.bottom, fg == popup_.surface->Hwnd() ? 1 : 0,
        fg == center_.surface->Hwnd() ? 1 : 0, notices_.empty() ? L"" : notices_[0].title.c_str(), notices_.empty() ? 0 : notices_[0].count);
}

} // namespace atrium
