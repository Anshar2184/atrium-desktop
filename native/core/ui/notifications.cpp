#include "ui/notifications.h"

#include "clock.h"
#include "log.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr float kW = 384, kGap = 10, kPeek = 10, kPill = 44;
constexpr int kMaxNotices = 50;
constexpr ULONGLONG kMergeMs = 120000;
constexpr UINT_PTR kPopupTimer = 1, kSweepTimer = 2, kHoldTimer = 3;
constexpr UINT kHoldMs = 250;
constexpr double kLeaveMs = 340, kRippleMs = 520;

float Smooth(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
// Moves v towards target at `rate` per second; true while still moving.
bool Glide(float& v, float target, float dt, float rate, float eps)
{
    v += (target - v) * (1 - std::exp(-dt / 1000.f * rate));
    if (std::fabs(target - v) < eps) {
        v = target;
        return false;
    }
    return true;
}

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
    // Expanding the stack: the popup stays until the list's glass is there (no empty frame).
    center_.surface->onReady = [this] {
        if (handoffPopup_) {
            handoffPopup_ = false;
            popup_.surface->Hide(false);
        }
    };
    center_.surface->onMargin = [this] { // a press next to the list closes it
        center_.surface->Hide();
        centerClosed_ = GetTickCount64();
    };
}

void Notifications::SetOptions(double timeoutSeconds, bool hideBody)
{
    timeout_ = std::clamp(timeoutSeconds, 3.0, 30.0);
    hideBody_ = hideBody;
    // With their text hidden, the cards stay out of screenshots too.
    popup_.surface->SetPrivate(hideBody);
    center_.surface->SetPrivate(hideBody);
    popup_.surface->Invalidate();
    center_.surface->Invalidate();
}

void Notifications::SetRadius(float dip)
{
    radius_ = dip;
    popup_.surface->SetRadius(dip);
    center_.surface->SetRadius(dip);
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

float Notifications::CardHeight() const { return 16 + 16 + 6 + 20 + 4 + 36 + 14; }

void Notifications::Post(Notice n)
{
    n.time = GetTickCount64();
    // Repeats of the listener's notices, and bursts of the same title from one
    // app, become one card with a count.
    for (auto it = notices_.begin(); it != notices_.end(); ++it) {
        bool sameExternal = n.externalId && it->externalId == n.externalId && it->source == n.source;
        bool burst = it->appId == n.appId && it->app == n.app && it->title == n.title && n.time - it->time < kMergeMs;
        if (sameExternal && it->title == n.title && it->body == n.body)
            return; // seen already
        if (sameExternal) { // the sender changed its notification: the card follows, as new
            Notice updated = *it;
            updated.title = n.title;
            updated.body = n.body;
            updated.time = n.time;
            updated.unread = true;
            notices_.erase(it);
            n = updated;
            Log(L"notify: %llu updated by its sender", n.id);
            break;
        }
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
    leaving_.erase(n.id); // a card closing just now that the new message continues stays
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
    if (popup_.surface->Visible())
        SetTimer(popup_.surface->Hwnd(), kPopupTimer, UINT(timeout_ * 1000), nullptr);
    Changed();
}

void Notifications::PanelsChanged()
{
    if (!popupIds_.empty())
        Layout(Mode::Popup);
}

void Notifications::HidePopup(const wchar_t* why)
{
    if (!popup_.surface->Visible()) {
        // Cards held back by an open panel are dropped too (they stay in the list).
        KillTimer(popup_.surface->Hwnd(), kHoldTimer);
        popupIds_.clear();
        return;
    }
    KillTimer(popup_.surface->Hwnd(), kPopupTimer);
    popup_.surface->Hide();
    popupIds_.clear();
    Log(L"notify: popup hidden (%s)", why);
}

void Notifications::ToggleCenter(bool expand)
{
    if (center_.surface->Visible()) {
        center_.surface->Hide();
        centerClosed_ = GetTickCount64();
        Log(L"notify: center closed");
        return;
    }
    if (!expand && GetTickCount64() - centerClosed_ < 400)
        return; // the click on the bell that just closed it
    if (!handoffPending_)
        HidePopup(L"center opened");
    expanded_ = expand || notices_.size() <= 1;
    center_.scroll = center_.scrollTarget = 0;
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
    HidePopup(L"cleared");
    popupIds_.clear();
    if (center_.surface->Visible() && n) {
        // The cards fly out one after another, then go.
        double now = ClockMs();
        int i = 0;
        for (const auto& notice : notices_)
            leaving_.try_emplace(notice.id, now + 30.0 * std::min(i++, 8));
        SetTimer(center_.surface->Hwnd(), kSweepTimer, UINT(kLeaveMs + 30 * 8 + 20), nullptr);
        center_.surface->Invalidate();
    } else {
        notices_.clear();
        leaving_.clear();
    }
    Log(L"notify: cleared %zu", n);
    Changed();
}

void Notifications::Remove(uint64_t id)
{
    bool inPopup = std::find(popupIds_.begin(), popupIds_.end(), id) != popupIds_.end();
    if (popup_.surface->Visible() && inPopup) {
        // Fades and drifts away; the cards behind move up once it is gone.
        KillTimer(popup_.surface->Hwnd(), kPopupTimer);
        leaving_.try_emplace(id, ClockMs());
        SetTimer(popup_.surface->Hwnd(), kSweepTimer, UINT(kLeaveMs + 20), nullptr);
        popup_.surface->Invalidate();
        Changed();
        return;
    }
    std::erase(popupIds_, id);
    if (popupIds_.empty())
        HidePopup(L"last card closed");
    if (center_.surface->Visible() && std::any_of(notices_.begin(), notices_.end(), [&](const Notice& n) { return n.id == id; })) {
        leaving_.try_emplace(id, ClockMs()); // slides out; the others close up
        SetTimer(center_.surface->Hwnd(), kSweepTimer, UINT(kLeaveMs + 20), nullptr);
        center_.surface->Invalidate();
    } else {
        std::erase_if(notices_, [&](const Notice& n) { return n.id == id; });
    }
    Changed();
}

void Notifications::Sweep()
{
    double now = ClockMs();
    bool swept = false;
    for (auto it = leaving_.begin(); it != leaving_.end();) {
        if (now - it->second >= kLeaveMs || (!center_.surface->Visible() && !popup_.surface->Visible())) {
            uint64_t id = it->first;
            std::erase_if(notices_, [&](const Notice& n) { return n.id == id; });
            std::erase(popupIds_, id);
            it = leaving_.erase(it);
            swept = true;
        } else {
            ++it;
        }
    }
    if (!leaving_.empty())
        SetTimer((center_.surface->Visible() ? center_ : popup_).surface->Hwnd(), kSweepTimer, 60, nullptr);
    if (swept && popup_.surface->Visible()) {
        if (popupIds_.empty()) {
            // Already faded out: the window just goes.
            KillTimer(popup_.surface->Hwnd(), kPopupTimer);
            popup_.surface->Hide(false);
            Log(L"notify: popup hidden (last card closed)");
        } else if (!popup_.tracking) {
            SetTimer(popup_.surface->Hwnd(), kPopupTimer, UINT(timeout_ * 1000), nullptr);
        }
    }
    if (swept)
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
    Sweep();
}

void Notifications::Changed()
{
    if (onUnread)
        onUnread(Unread());
    // While the stack turns into the list the popup waits for the list's glass (onReady).
    if (!handoffPopup_ && (popup_.surface->Visible() || !popupIds_.empty()))
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
    if (mode == Mode::Popup && held && held()) {
        // Another panel is open: the cards wait (their timeout too) until it closes.
        if (v.surface->Visible()) {
            KillTimer(v.surface->Hwnd(), kPopupTimer);
            v.surface->Hide();
            Log(L"notify: popup held (a panel is open)");
        }
        SetTimer(v.surface->Hwnd(), kHoldTimer, kHoldMs, nullptr);
        return;
    }
    RECT bell = bellRect ? bellRect() : RECT{};
    RECT bar = barRect ? barRect() : bell;
    float s = MonitorScale(bar);
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;

    // The cards stand on the bar: the newest at the bottom, older ones above
    // it (peeking out over its top edge in the stack, in a column when
    // expanded), so the list unfolds upwards. The list's controls are a
    // capsule below the cards.
    const size_t n = shown.size();
    const float card = CardHeight();
    float content = 0;
    if (n && (mode == Mode::Popup || (!expanded_ && n > 1)))
        content = card + kPeek * float(std::min<size_t>(2, n - 1));
    else if (n)
        content = float(n) * card + float(n - 1) * kGap;
    float pill = mode == Mode::Center ? kPill + (n ? kGap : 0) : 0;
    int gap = int(std::lround(kPopupGapDip * s)), margin = int(std::lround(8 * s));
    int bottom = bar.top - gap;
    float maxContent = float(bottom - wa.top - margin) / s - pill;
    v.contentH = content;
    v.viewH = n ? std::min(content, std::max(card, maxContent)) : 0;
    v.scrollTarget = std::clamp(v.scrollTarget, 0.f, std::max(0.f, v.contentH - v.viewH));
    v.scroll = std::clamp(v.scroll, 0.f, std::max(0.f, v.contentH - v.viewH));
    int w = int(std::lround(kW * s)), h = int(std::lround((v.viewH + pill) * s));
    // Right edge on the bar's right edge (D32), inside the work area.
    int right = std::clamp(int(bar.right), int(wa.left) + margin + w, int(wa.right) - margin);
    RECT r{right - w, std::max(int(wa.top) + margin, bottom - h), right, bottom};
    if (!v.surface->Visible()) {
        v.cards.clear(); // the cards arrive afresh
        v.glow.clear();
        bool handoff = mode == Mode::Center && handoffPending_;
        if (handoff) {
            // From the popup's stack: each card starts where it was on screen.
            float dy = float(handoffRect_.top - r.top) / s;
            for (auto [id, a] : handoff_) {
                a.y += dy;
                a.content = id == handoffFront_ ? 1.f : 0.f; // their text appears as they move apart
                v.cards[id] = a;
            }
            handoff_.clear();
            handoffPending_ = false;
        }
        // Pops out of the bell (unless the stack turns into the list in place).
        v.surface->Show(r, mode == Mode::Center, POINT{(bell.left + bell.right) / 2, bottom}, !handoff);
    } else {
        // The window's top moves when the list grows or shrinks: the cards
        // keep their place on screen and glide from there.
        RECT old = v.surface->Rect();
        float dy = float(old.top - r.top) / s;
        if (dy != 0)
            for (auto& [id, a] : v.cards)
                a.y += dy;
        v.surface->Move(r);
    }
}

float Notifications::Ease(View& v, Canvas& c, const std::wstring& id, bool on, float dt)
{
    float& g = v.glow[id];
    if (Glide(g, on ? 1.f : 0.f, dt, on ? 20.f : 11.f, 0.01f))
        c.Continue();
    return g;
}

float Notifications::CardRadius() const { return std::min(radius_, CardHeight() / 2); }

// A card's content over its own piece of glass. Cards behind the newest one
// in the stack show only their glass's edge.
void Notifications::PaintCard(Canvas& c, const Notice& n, D2D1_RECT_F r, int index, View& v, bool full, float alpha)
{
    if (!full)
        return;
    const Theme& t = c.theme;
    const float cardR = CardRadius();
    std::wstring cardId = L"card:" + std::to_wstring(index), closeId = L"close:" + std::to_wstring(index);
    bool hot = v.hover == cardId || v.hover == closeId;
    bool pressed = v.pressed == cardId || (index == 0 && v.pressed == L"stack");
    float glow = Ease(v, c, cardId, hot, float(std::clamp(c.now - v.lastPaint, 0.0, 100.0)));
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    // A pressed card gives way a little, like the glass under it.
    c.BeginGroup(alpha, pressed ? 0.975f : 1 + 0.008f * glow, cx, cy);
    if (c.panel)
        c.Fill(r, cardR, t.panel);
    if (glow > 0)
        c.Fill(r, cardR, D2D1::ColorF(t.hover.r, t.hover.g, t.hover.b, t.hover.a * 0.6f * glow));
    if (v.rippleId == cardId && c.now - v.rippleMs < kRippleMs) {
        c.Ripple(r, cardR, v.rippleX, v.rippleY, float((c.now - v.rippleMs) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
        c.Continue();
    }
    const float x = r.left, y = r.top, w = r.right - r.left;
    float tx = x + 20, ty = y + 16;
    if (n.unread)
        c.Circle(tx + 3, ty + 8, 3.5f, t.accent);
    float nameX = tx + (n.unread ? 12 : 0);
    std::wstring app = n.app.empty() ? L"Atrium" : n.app;
    if (n.count > 1)
        app += L"  ·  " + std::to_wstring(n.count) + L" 条";
    c.Text(app, D2D1::RectF(nameX, ty, x + w - 170, ty + 16), 12, t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::wstring when = Ago(n.time);
    if (index == 0 && &v == &popup_ && popupIds_.size() > 1) {
        // Which card of the stack is in front (the wheel turns the stack).
        size_t newer = size_t(std::count_if(popupIds_.begin(), popupIds_.end(), [&](uint64_t id) { return id > n.id; }));
        when = std::to_wstring(newer + 1) + L"/" + std::to_wstring(popupIds_.size()) + L"  ·  " + when;
    }
    c.Text(when, D2D1::RectF(x + w - 170, ty, x + w - 50, ty + 16), 12, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
    c.Text(n.title, D2D1::RectF(tx, ty + 22, x + w - 20, ty + 42), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::wstring body = BodyHidden() ? L"内容已隐藏" : n.body;
    c.Text(body, D2D1::RectF(tx, ty + 46, x + w - 20, ty + 46 + 36), 13, BodyHidden() ? t.faint : t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    // Dismiss: a round button in the top-right corner, shown with the pointer on the card.
    D2D1_RECT_F close = D2D1::RectF(x + w - 44, y + 10, x + w - 16, y + 38);
    if (glow > 0.01f) {
        c.Fill(close, 14, D2D1::ColorF(t.hover.r, t.hover.g, t.hover.b, (v.hover == closeId ? t.press.a * 1.6f : t.hover.a * 1.2f) * glow));
        c.Glyph(L"\uE711", close, 10, D2D1::ColorF(t.text.r, t.text.g, t.text.b, t.text.a * glow));
    }
    c.EndGroup();
    v.hits.push_back({close, closeId});
}

void Notifications::Paint(Mode mode, Canvas& c)
{
    View& v = ViewOf(mode);
    const Theme& t = c.theme;
    RECT rr = v.surface->Rect();
    const float w = float(rr.right - rr.left) / c.scale;
    const float dt = v.lastPaint > 0 ? float(std::clamp(c.now - v.lastPaint, 0.0, 100.0)) : 16.f;
    v.hits.clear();
    auto shown = Shown(mode);
    std::vector<ShapeDip> shapes;
    const float listH = v.viewH; // the cards: [0, listH]; the list's capsule below

    const bool stack = mode == Mode::Popup || (!expanded_ && shown.size() > 1);
    if (Glide(v.scroll, v.scrollTarget, dt, 18, 0.2f))
        c.Continue();
    const float card = CardHeight(), cardR = CardRadius();
    // Where the layout wants each card; the displayed place glides there.
    std::vector<D2D1_RECT_F> rects(shown.size());
    std::vector<float> alphas(shown.size());
    for (size_t i = 0; i < shown.size(); ++i) {
        const Notice& n = *shown[i];
        float ty, tinset, talpha;
        if (stack) {
            float k = float(std::min<size_t>(i, 2));
            ty = listH - card - kPeek * k;
            tinset = 12.f * k;
            talpha = i <= 2 ? 1.f : 0.f;
        } else {
            ty = listH - card - float(i) * (card + kGap) + v.scroll;
            tinset = 0;
            talpha = 1;
        }
        CardAnim& a = v.cards[n.id];
        if (!a.placed) // a new card rises from the bar
            a = {ty + 18, tinset, 0, 1, true};
        bool moving = Glide(a.y, ty, dt, 16, 0.2f);
        moving |= Glide(a.inset, tinset, dt, 16, 0.1f);
        moving |= Glide(a.alpha, talpha, dt, 12, 0.01f);
        moving |= Glide(a.content, stack && i > 0 ? 0.f : 1.f, dt, 9, 0.01f);
        float alpha = a.alpha, dx = 0, shrink = 0;
        if (auto l = leaving_.find(n.id); l != leaving_.end()) {
            // Closing: it fades out while drifting right and settling back a little.
            float e = Smooth(float(std::max(0.0, c.now - l->second) / kLeaveMs));
            dx = 36 * e;
            shrink = 0.07f * e;
            alpha *= (1 - e) * (1 - e);
            moving = true;
        }
        if (moving)
            c.Continue();
        float sx = (w - 2 * a.inset) * shrink / 2, sy = card * shrink / 2;
        rects[i] = D2D1::RectF(a.inset + dx + sx, a.y + sy, w - a.inset + dx - sx, a.y + card - sy);
        alphas[i] = alpha;
    }
    for (auto it = v.cards.begin(); it != v.cards.end();) // forget cards that are gone
        it = std::any_of(shown.begin(), shown.end(), [&](const Notice* n) { return n->id == it->first; }) ? std::next(it) : v.cards.erase(it);

    // Back to front: the newest card is drawn last, on top. Each card is a
    // piece of glass; a fading card's glass shrinks away with it, and glass
    // scrolled out of the list is cut at the list's edge.
    c.Clip(D2D1::RectF(-1, 0, w + 1, listH));
    for (size_t i = shown.size(); i-- > 0;) {
        bool full = !stack || i == 0;
        D2D1_RECT_F r = rects[i];
        if (alphas[i] <= 0.02f || r.bottom < 0 || r.top > listH)
            continue;
        // The glass fades with the card (and settles back a hair).
        float k = 0.96f + 0.04f * alphas[i], hw = (r.right - r.left) / 2 * k, hh = (r.bottom - r.top) / 2 * k;
        float mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
        D2D1_RECT_F g = D2D1::RectF(mx - hw, std::max(0.f, my - hh), mx + hw, std::min(listH, my + hh));
        if (g.bottom - g.top > 8)
            shapes.push_back({g, cardR, 1 - std::clamp(alphas[i], 0.f, 1.f)});
        size_t hitsBefore = v.hits.size();
        PaintCard(c, *shown[i], r, int(i), v, full, alphas[i] * v.cards[shown[i]->id].content);
        if (!stack)
            v.hits.push_back({r, L"card:" + std::to_wstring(i)});
        // Only the visible part of a card is clickable (a card scrolled below
        // the list must not catch presses meant for the capsule).
        for (size_t h = hitsBefore; h < v.hits.size(); ++h) {
            v.hits[h].rect.top = std::max(v.hits[h].rect.top, 0.f);
            v.hits[h].rect.bottom = std::min(v.hits[h].rect.bottom, listH);
        }
    }
    c.Unclip();
    if (stack && !shown.empty()) {
        // One card: clicking it runs its action. Several: clicking expands the list.
        D2D1_RECT_F all = D2D1::RectF(0, 0, w, listH);
        v.hits.push_back({all, shown.size() > 1 ? L"stack" : L"card:0"});
    }

    if (mode == Mode::Center) {
        // The list's capsule, shaped like the bar.
        D2D1_RECT_F pill = D2D1::RectF(0, shown.empty() ? 0 : listH + kGap, w, (shown.empty() ? 0 : listH + kGap) + kPill);
        const float pr = kPill / 2;
        shapes.push_back({pill, pr});
        if (c.panel)
            c.Fill(pill, pr, t.panel);
        size_t live = shown.size() - std::min(shown.size(), leaving_.size());
        std::wstring label = live ? L"通知  ·  " + std::to_wstring(live) : L"没有新通知";
        c.Text(label, D2D1::RectF(pill.left + 20, pill.top, w / 2 + 40, pill.bottom), 13.5f, live ? t.text : t.subtext,
               live ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        float x = pill.right - 6;
        auto button = [&](const std::wstring& id, const std::wstring& text) {
            float bw = c.TextWidth(text, 12) + 28;
            D2D1_RECT_F b = D2D1::RectF(x - bw, pill.top + 6, x, pill.bottom - 6);
            float g = Ease(v, c, id, v.hover == id, dt);
            c.Fill(b, (b.bottom - b.top) / 2, Alpha(v.pressed == id ? t.press : t.hover, 0.45f + 0.55f * g));
            c.Text(text, b, 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
            v.hits.push_back({b, id});
            x = b.left - 6;
        };
        if (live)
            button(L"clear", L"全部清除");
        if (expanded_ && shown.size() > 1)
            button(L"collapse", L"收起");
    }
    v.surface->SetShapes(shapes);
    v.lastPaint = c.now;
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
            // The stack becomes the list in place: the popup goes at once and
            // its cards carry on in the list from where they are.
            handoff_ = popup_.cards;
            handoffFront_ = popupIds_.empty() ? 0 : popupIds_.front();
            handoffRect_ = popup_.surface->Rect();
            handoffPending_ = true;
            KillTimer(popup_.surface->Hwnd(), kPopupTimer);
            popupIds_.clear();
            handoffPopup_ = true; // hidden once the list's glass shows (onReady)
            ToggleCenter(true); // the full list, expanded
            handoffPending_ = false;
            handoff_.clear();
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
        if (w == kHoldTimer && mode == Mode::Popup) {
            if (!held || !held()) {
                KillTimer(v.surface->Hwnd(), kHoldTimer);
                if (!popupIds_.empty()) {
                    Layout(Mode::Popup);
                    SetTimer(v.surface->Hwnd(), kPopupTimer, UINT(timeout_ * 1000), nullptr);
                    Log(L"notify: popup shown after the panel closed");
                }
            }
            return true;
        }
        if (w == kSweepTimer) {
            KillTimer(v.surface->Hwnd(), kSweepTimer);
            Sweep();
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
    case WM_LBUTTONDOWN: {
        v.pressed = hitAt(pt);
        if (!v.pressed.empty()) {
            // The glass under the card gives way, and a soft light spreads from the press.
            float s = v.surface->Scale();
            v.rippleId = v.pressed == L"stack" ? L"card:0" : v.pressed;
            v.rippleX = float(pt.x) / s;
            v.rippleY = float(pt.y) / s;
            v.rippleMs = ClockMs();
            v.surface->Ripple(v.rippleX, v.rippleY);
        }
        v.surface->Invalidate();
        return true;
    }
    case WM_LBUTTONUP: {
        std::wstring h = hitAt(pt);
        bool same = !h.empty() && h == v.pressed;
        v.pressed.clear();
        if (same)
            Click(mode, h);
        else
            v.surface->Invalidate();
        return true;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (mode == Mode::Popup) {
            // Over the stack: the wheel turns it, one card per notch, to read
            // the ones behind (down: the next older comes to the front).
            v.wheel += delta;
            int steps = v.wheel / WHEEL_DELTA;
            v.wheel -= steps * WHEEL_DELTA;
            if (popupIds_.size() > 1 && steps) {
                for (int i = 0; i < std::abs(steps); ++i) {
                    if (steps < 0)
                        std::rotate(popupIds_.begin(), popupIds_.begin() + 1, popupIds_.end());
                    else
                        std::rotate(popupIds_.rbegin(), popupIds_.rbegin() + 1, popupIds_.rend());
                }
                Log(L"notify: stack turned (%llu in front)", popupIds_.front());
                v.surface->Invalidate();
            }
            return true;
        }
        v.scrollTarget += float(delta) / WHEEL_DELTA * 80; // older cards are above: the wheel up brings them down
        v.scrollTarget = std::clamp(v.scrollTarget, 0.f, std::max(0.f, v.contentH - v.viewH));
        v.surface->Invalidate();
        return true;
    }
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
        // Screen px, for tests.
        const RECT r = v.surface->Rect();
        const float s = v.surface->Scale();
        std::wstring spots;
        for (const Hit& h : v.hits)
            spots += L" " + h.id + L"@" + std::to_wstring(int(float(r.left) + (h.rect.left + h.rect.right) / 2 * s)) + L"," +
                     std::to_wstring(int(float(r.top) + (h.rect.top + h.rect.bottom) / 2 * s));
        Log(L"notify: spots %s%s", mode == Mode::Popup ? L"popup" : L"center", spots.c_str());
    }
    // Cards that are flying out are already gone for the user.
    std::vector<const Notice*> live;
    for (const auto& n : notices_)
        if (!leaving_.count(n.id))
            live.push_back(&n);
    RECT p = popup_.surface->Rect(), c = center_.surface->Rect();
    HWND fg = GetForegroundWindow();
    Log(L"notify: state count=%zu unread=%d popup=%d popupCards=%zu center=%d expanded=%d bodyHidden=%d quiet=%d "
        L"popupRect=%ld,%ld,%ld,%ld centerRect=%ld,%ld,%ld,%ld fgPopup=%d fgCenter=%d first='%s' firstCount=%d radius=%.0f popupShapes=%zu centerShapes=%zu",
        live.size(), Unread(), popup_.surface->Visible() ? 1 : 0, popupIds_.size(), center_.surface->Visible() ? 1 : 0, expanded_ ? 1 : 0,
        BodyHidden() ? 1 : 0, QuietState(), p.left, p.top, p.right, p.bottom, c.left, c.top, c.right, c.bottom, fg == popup_.surface->Hwnd() ? 1 : 0,
        fg == center_.surface->Hwnd() ? 1 : 0, live.empty() ? L"" : live[0]->title.c_str(), live.empty() ? 0 : live[0]->count, radius_, popup_.surface->Shapes(),
        center_.surface->Shapes());
}

} // namespace atrium
