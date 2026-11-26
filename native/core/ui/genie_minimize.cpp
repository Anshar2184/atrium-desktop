#include "ui/genie_minimize.h"

#include "clock.h"
#include "log.h"
#include "shell/games.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace atrium {

namespace {

constexpr UINT WM_GENIE_PICTURE = WM_APP + 0x5E, WM_GENIE_RESTORE = WM_APP + 0x5F;
constexpr UINT_PTR kWatchTimer = 0x4751, kIdleTimer = 0x4752, kFreshTimer = 0x4753;
constexpr UINT kWatchMs = 300;
constexpr ULONGLONG kDrawnMs = 700;      // a window this long in front has drawn itself
constexpr ULONGLONG kNearMs = 1500;      // the picture's age while the pointer is near the caption buttons
constexpr ULONGLONG kFarMs = 20000;      // elsewhere: PrintWindow makes the app draw itself whole
constexpr int kLargeMax = 1600;          // the picture, at most this many px either way
constexpr UINT kIdleMs = 4000;          // the layer stays this long after it was last wanted
constexpr UINT kFreshMs = 180;           // the bar's minimize waits at most this long for a fresh picture
constexpr double kFlightMs = 560;
constexpr double kHoldMs = 90, kDissolveMs = 160; // out of the icon: the picture over the window, then fading
constexpr int kRows = 64;                // the picture poured as this many slices

GenieMinimize* g_genie = nullptr; // the one effect, for its hook

void CALLBACK OnMinimizeStart(HWINEVENTHOOK, DWORD, HWND h, LONG object, LONG, DWORD, DWORD)
{
    if (object == OBJID_WINDOW && g_genie)
        g_genie->Minimizing(h);
}

void Transitions(HWND h, bool on)
{
    const BOOL off = on ? FALSE : TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}

RECT Frame(HWND h)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    return r;
}

RECT MonitorRect(const RECT& r)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

// A window worth the effect: a visible, top-level, captioned one that minimizes.
bool Minimizable(HWND h)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    return h && IsWindowVisible(h) && !IsIconic(h) && (style & WS_MINIMIZEBOX) && (style & WS_CAPTION) == WS_CAPTION && !GetWindow(h, GW_OWNER);
}

float SmoothStep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

GenieMinimize::GenieMinimize(HINSTANCE instance, std::function<std::optional<RECT>(HWND)> target, std::function<bool(HWND)> skip)
    : surface_(instance, L"Atrium Genie", false, 0, false, true),
      snapshots_(surface_.Hwnd(), WM_GENIE_PICTURE),
      target_(std::move(target)),
      skip_(std::move(skip))
{
    surface_.SetGlassless(true);
    surface_.SetRegionNone(); // over everything and out of the way until something pours
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m == WM_TIMER && w == kWatchTimer) {
            Watch();
        } else if (m == WM_TIMER && w == kIdleTimer) {
            KillTimer(surface_.Hwnd(), kIdleTimer);
            if (flights_.empty())
                surface_.Hide(false); // idle for a while: its buffers go
        } else if (m == WM_TIMER && w == kFreshTimer) {
            RunPending();
        } else if (m == WM_GENIE_RESTORE) {
            RunRestores();
        } else if (m == WM_GENIE_PICTURE) {
            for (WindowSnapshots::Result& r : snapshots_.Take())
                if (r.large.ok && pictures_.contains(r.window)) {
                    pictures_[r.window].large = std::make_shared<const WindowPicture>(std::move(r.large));
                    if (r.window == pending_)
                        RunPending();
                }
        } else {
            return false;
        }
        return true;
    };
}

GenieMinimize::~GenieMinimize()
{
    SetEnabled(false);
    Shutdown();
}

void GenieMinimize::Shutdown()
{
    if (g_genie == this)
        g_genie = nullptr;
    if (hook_)
        UnhookWinEvent(hook_);
    hook_ = nullptr;
    KillTimer(surface_.Hwnd(), kWatchTimer);
    KillTimer(surface_.Hwnd(), kIdleTimer);
    RunPending();
    Release(held_, !(skip_ && skip_(held_)));
    for (const Flight& f : flights_) {
        surface_.ForgetImage(f.key);
        if (f.window && f.window != held_ && !(skip_ && skip_(f.window)) && IsWindow(f.window))
            Transitions(f.window, true);
    }
    flights_.clear();
    pictures_.clear();
    front_ = nullptr;
    surface_.SetRegionNone();
    surface_.Hide(false);
}

void GenieMinimize::SetEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (on) {
        g_genie = this;
        hook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnMinimizeStart, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(surface_.Hwnd(), kWatchTimer, kWatchMs, nullptr);
    } else {
        Shutdown();
    }
    Log(L"genie: %s", on ? L"on" : L"off");
}

void GenieMinimize::Release(HWND window, bool restoreTransitions)
{
    if (window && window == held_) {
        if (restoreTransitions && IsWindow(window))
            Transitions(window, true);
        held_ = nullptr;
    }
}

void GenieMinimize::Hold(HWND window)
{
    if (window == held_)
        return;
    Release(held_);
    Transitions(window, false);
    held_ = window;
}

RECT GenieMinimize::FlightBounds(const RECT& fallback) const
{
    RECT bounds{};
    for (const Flight& g : flights_) {
        const RECT a = MonitorRect(g.from), b = MonitorRect(g.to);
        RECT both{};
        UnionRect(&both, &a, &b);
        if (IsRectEmpty(&bounds))
            bounds = both;
        else
            UnionRect(&bounds, &bounds, &both);
    }
    return IsRectEmpty(&bounds) ? MonitorRect(fallback) : bounds;
}

void GenieMinimize::Arm(const RECT& frame)
{
    const RECT bounds = FlightBounds(frame);
    if (!surface_.Visible()) {
        surface_.Show(bounds, false, POINT{bounds.left, bounds.top}, false);
    } else {
        const RECT now = surface_.Rect();
        if (!EqualRect(&now, &bounds))
            surface_.Move(bounds);
    }
    SetTimer(surface_.Hwnd(), kIdleTimer, kIdleMs, nullptr);
}

// The window in front: its picture kept fresh, and its own minimize animation off once there is one.
void GenieMinimize::Watch()
{
    if (pending_)
        return;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    DWORD pid = 0;
    if (root)
        GetWindowThreadProcessId(root, &pid);
    if (pid == GetCurrentProcessId() || !Minimizable(root) || (skip_ && skip_(root)) || IsGameWindow(root))
        root = nullptr;
    const ULONGLONG now = GetTickCount64();
    if (root != front_) {
        Release(held_, !(skip_ && skip_(held_)));
        front_ = root;
        frontSince_ = now;
    }
    const auto inFlight = [&](HWND h) { return std::ranges::any_of(flights_, [h](const Flight& f) { return f.window == h; }); };
    std::erase_if(pictures_, [&](auto& kv) {
        const HWND h = kv.first;
        if (h != root && !IsWindow(h))
            return true;
        if (h != root && h != pending_ && !IsIconic(h) && !inFlight(h)) {
            kv.second.large.reset();
            return true;
        }
        return false;
    });
    if (!root)
        return;
    Picture& p = pictures_[root];
    p.frame = Frame(root);
    // Only a window with its icon on a bar to pour into: any other keeps Windows' own animation.
    if (p.large && target_ && target_(root))
        Hold(root);
    POINT pt{};
    GetCursorPos(&pt);
    const float s = float(GetDpiForWindow(root)) / 96.f;
    const RECT& r = p.frame;
    const bool atButtons = PtInRect(&r, pt) && pt.y < r.top + LONG(56 * s) && pt.x > r.right - LONG(200 * s);
    if (atButtons && held_ == root)
        Arm(r); // a minimize may come: the first frame should not wait for the layer
    if (now - frontSince_ < kDrawnMs || now - p.taken < (atButtons ? kNearMs : kFarMs))
        return;
    p.taken = now;
    snapshots_.Request(root, 64, 64, 0, kLargeMax);
}

void GenieMinimize::Minimizing(HWND window)
{
    if (!enabled_ || window != held_)
        return; // not prepared for: Windows' own animation played
    if (skip_ && skip_(window)) {
        Release(window, false);
        front_ = nullptr;
        return;
    }
    eventMs_ = ClockMs();
    const auto it = pictures_.find(window);
    const std::optional<RECT> to = target_ ? target_(window) : std::nullopt;
    Release(window); // down now: restoring it is Windows' own unless the bar restores it
    front_ = nullptr;
    if (it == pictures_.end() || !it->second.large || !to) {
        Log(L"genie: %p went down without it (picture %d, icon %d)", window, it != pictures_.end() && it->second.large, to.has_value());
        return;
    }
    Flight f;
    f.picture = it->second.large;
    f.from = it->second.frame;
    f.to = *to;
    f.window = window;
    Launch(std::move(f));
}

bool GenieMinimize::Minimize(HWND window, std::function<void()> minimize)
{
    if (!enabled_ || pending_ || !Minimizable(window) || (skip_ && skip_(window)) || !target_ || !target_(window))
        return false;
    Picture& p = pictures_[window];
    p.frame = Frame(window);
    p.taken = GetTickCount64();
    Hold(window);
    front_ = window;
    Arm(p.frame);
    pending_ = window;
    pendingMinimize_ = std::move(minimize);
    snapshots_.Request(window, 64, 64, 0, kLargeMax);
    SetTimer(surface_.Hwnd(), kFreshTimer, kFreshMs, nullptr);
    return true;
}

void GenieMinimize::RunPending()
{
    KillTimer(surface_.Hwnd(), kFreshTimer);
    const HWND window = std::exchange(pending_, nullptr);
    std::function<void()> minimize = std::exchange(pendingMinimize_, nullptr);
    if (!window || !minimize)
        return;
    const auto it = pictures_.find(window);
    if (it == pictures_.end() || !it->second.large)
        Release(window); // nothing to pour: Windows' own animation
    minimize();
}

bool GenieMinimize::Restore(HWND window, std::function<void()> restore)
{
    if (!enabled_ || (skip_ && skip_(window)))
        return false;
    const auto it = pictures_.find(window);
    const std::optional<RECT> to = target_ ? target_(window) : std::nullopt;
    if (it == pictures_.end() || !it->second.large || !to)
        return false;
    Flight f;
    f.picture = it->second.large;
    f.from = it->second.frame;
    f.to = *to;
    f.window = window;
    f.restore = std::move(restore);
    eventMs_ = ClockMs();
    Launch(std::move(f));
    return true;
}

void GenieMinimize::Launch(Flight f)
{
    f.key = L"genie|" + std::to_wstring(++flightGen_);
    flights_.push_back(std::move(f));
    Arm(flights_.back().from);
    // On screen (and in the way of clicks) only where the flights go, while they do.
    const RECT origin = surface_.Rect();
    const float s = surface_.Scale();
    const float maxW = float(origin.right - origin.left) / s, maxH = float(origin.bottom - origin.top) / s;
    D2D1_RECT_F area = D2D1::RectF(1e9f, 1e9f, -1e9f, -1e9f);
    for (const Flight& g : flights_)
        for (const RECT& r : {g.from, g.to})
            area = D2D1::RectF(std::min(area.left, float(r.left - origin.left) / s), std::min(area.top, float(r.top - origin.top) / s),
                               std::max(area.right, float(r.right - origin.left) / s), std::max(area.bottom, float(r.bottom - origin.top) / s));
    area =
        D2D1::RectF(std::clamp(area.left, 0.f, maxW), std::clamp(area.top, 0.f, maxH), std::clamp(area.right, 0.f, maxW), std::clamp(area.bottom, 0.f, maxH));
    if (area.right > area.left && area.bottom > area.top)
        surface_.SetRegion(area);
    else
        surface_.SetRegionNone();
    surface_.Invalidate();
}

// Out of the icon and landed: the window comes back under its picture, without an animation of its own.
void GenieMinimize::RunRestores()
{
    struct Due {
        HWND window = nullptr;
        std::function<void()> restore;
    };
    std::vector<Due> due;
    for (Flight& f : flights_)
        if (f.restoreDue && f.restore) {
            f.restoreTransitions = true;
            due.push_back({f.window, std::exchange(f.restore, nullptr)});
        }
    for (Due& d : due) {
        if (IsWindow(d.window))
            Transitions(d.window, false);
        d.restore();
    }
}

void GenieMinimize::Paint(Canvas& c)
{
    for (auto it = flights_.begin(); it != flights_.end();) {
        if (PaintOne(c, *it)) {
            if (it->restoreTransitions && it->window != held_ && !(skip_ && skip_(it->window)) && IsWindow(it->window))
                Transitions(it->window, true);
            surface_.ForgetImage(it->key);
            it = flights_.erase(it);
        } else {
            ++it;
        }
    }
    if (!flights_.empty()) {
        c.Continue();
    } else {
        surface_.SetRegionNone();
        SetTimer(surface_.Hwnd(), kIdleTimer, kIdleMs, nullptr);
    }
}

// One flight's frame: into the icon over kFlightMs; out of it the same way backwards, then the picture held
// over the restored window while it draws, then fading into it.
bool GenieMinimize::PaintOne(Canvas& c, Flight& f)
{
    if (f.startMs <= 0) {
        f.startMs = c.now; // time runs from the first frame on screen: no jump at the start
        Log(L"genie: %s, first frame %.0f ms after it began", f.restore ? L"restore" : L"minimize", c.now - eventMs_);
    }
    const double ms = c.now - f.startMs;
    const float t = float(std::clamp(ms / kFlightMs, 0.0, 1.0));
    if (!f.restore && !f.restoreDue) {
        PaintFlight(c, f, t, 1);
        return t >= 1;
    }
    if (t >= 1 && !f.restoreDue) {
        f.restoreDue = true;
        PostMessageW(surface_.Hwnd(), WM_GENIE_RESTORE, 0, 0);
    }
    const float fade = 1 - SmoothStep(float(kFlightMs + kHoldMs), float(kFlightMs + kHoldMs + kDissolveMs), float(ms));
    PaintFlight(c, f, 1 - t, fade);
    return ms >= kFlightMs + kHoldMs + kDissolveMs && !f.restore;
}

// The picture in kRows slices across the way to the icon ("along"). In the frame where the icon lies
// ahead, a slice at v (0 the far side, 1 the near) slides from its place to the icon's line, the near
// ones first; at any point of the way the two sides bend from the window's towards the icon's, more as
// the pour goes on, along an S curve.
void GenieMinimize::PaintFlight(Canvas& c, const Flight& f, float t, float alpha)
{
    const WindowPicture& pic = *f.picture;
    const RECT origin = surface_.Rect();
    auto dip = [&](LONG v, LONG o) { return float(v - o) / c.scale; };
    const D2D1_RECT_F w =
        D2D1::RectF(dip(f.from.left, origin.left), dip(f.from.top, origin.top), dip(f.from.right, origin.left), dip(f.from.bottom, origin.top));
    const D2D1_RECT_F ic = D2D1::RectF(dip(f.to.left, origin.left), dip(f.to.top, origin.top), dip(f.to.right, origin.left), dip(f.to.bottom, origin.top));
    const float wcx = (w.left + w.right) / 2, wcy = (w.top + w.bottom) / 2, icx = (ic.left + ic.right) / 2, icy = (ic.top + ic.bottom) / 2;
    // Which way: towards the bar's side the icon lies on.
    const bool vertical = std::abs(icy - wcy) * (w.right - w.left) >= std::abs(icx - wcx) * (w.bottom - w.top);
    const float sign = vertical ? (icy >= wcy ? 1.f : -1.f) : (icx >= wcx ? 1.f : -1.f);
    // The frame: a along the way (growing towards the icon), b across it.
    const float a0 = vertical ? (sign > 0 ? w.top : -w.bottom) : (sign > 0 ? w.left : -w.right);
    const float a1 = vertical ? (sign > 0 ? w.bottom : -w.top) : (sign > 0 ? w.right : -w.left);
    const float b0 = vertical ? w.left : w.top, b1 = vertical ? w.right : w.bottom;
    const float aEnd = vertical ? sign * icy : sign * icx;
    const float iconHalf = 0.35f * (vertical ? ic.right - ic.left : ic.bottom - ic.top), iconMid = vertical ? icx : icy;
    const float bend = SmoothStep(0.f, 0.42f, t), slide = SmoothStep(0.12f, 1.f, t);
    auto along = [&](float v) {
        constexpr float kLead = 0.9f; // how far ahead the near rows go
        const float s = std::clamp(slide * (1 + kLead) - (1 - v) * kLead, 0.f, 1.f);
        return Lerp(a0 + v * (a1 - a0), aEnd, s * s * (3 - 2 * s));
    };
    auto sides = [&](float a, float& lo, float& hi) {
        const float u = std::clamp((a - a0) / std::max(1.f, aEnd - a0), 0.f, 1.f), k = bend * SmoothStep(0.f, 1.f, u);
        lo = Lerp(b0, iconMid - iconHalf, k);
        hi = Lerp(b1, iconMid + iconHalf, k);
    };
    const float opacity = (1 - SmoothStep(0.86f, 1.f, t)) * alpha;
    if (opacity <= 0.002f)
        return;
    const IconImage& image = pic.image;
    const float pw = float(pic.width), ph = float(pic.height), pad = float(kPicturePad);
    // While the picture is still whole (the first frames of a pour, the last of a restore) slices
    // only put their seams back: one quad carries it. Slices start once it is being compressed.
    float wl0 = 0, wh0 = 0, wl1 = 0, wh1 = 0;
    sides(along(0.f), wl0, wh0);
    sides(along(1.f), wl1, wh1);
    const bool whole = std::abs(wl0 - b0) < 0.75f && std::abs(wh0 - b1) < 0.75f && std::abs(wl1 - b0) < 0.75f &&
                       std::abs(wh1 - b1) < 0.75f && along(1.f) - along(0.f) > 0.97f * (a1 - a0);
    const int rows = whole ? 1 : kRows;
    const float hair = rows > 1 ? 0.5f : 0.f; // a hair over the next: no seams
    for (int i = 0; i < rows; ++i) {
        const float v0 = float(i) / rows, v1 = float(i + 1) / rows;
        const float p0 = along(v0), p1 = along(v1) + hair;
        if (p1 - p0 < 0.55f)
            continue;
        float lo0 = 0, hi0 = 0, lo1 = 0, hi1 = 0;
        sides(p0, lo0, hi0);
        sides(p1, lo1, hi1);
        // Back to the screen: the slice's corners for its source's top left, top right, bottom right, bottom
        // left; its source rows (far side v = 0) in the picture.
        const float s0 = sign > 0 ? v0 : 1 - v1, s1 = sign > 0 ? v1 : 1 - v0;
        const float y0 = sign * p0, y1 = sign * p1; // the far end and the near end, on the screen's axis
        std::array<D2D1_POINT_2F, 4> quad{};
        D2D1_RECT_F src{};
        if (vertical) {
            // The source's top is the slice's far end when the icon is below, its near end when above.
            quad = sign > 0 ? std::array{D2D1::Point2F(lo0, y0), D2D1::Point2F(hi0, y0), D2D1::Point2F(hi1, y1), D2D1::Point2F(lo1, y1)} :
                              std::array{D2D1::Point2F(lo1, y1), D2D1::Point2F(hi1, y1), D2D1::Point2F(hi0, y0), D2D1::Point2F(lo0, y0)};
            src = D2D1::RectF(pad, pad + s0 * ph, pad + pw, pad + s1 * ph);
        } else {
            quad = sign > 0 ? std::array{D2D1::Point2F(y0, lo0), D2D1::Point2F(y1, lo1), D2D1::Point2F(y1, hi1), D2D1::Point2F(y0, hi0)} :
                              std::array{D2D1::Point2F(y1, lo1), D2D1::Point2F(y0, lo0), D2D1::Point2F(y0, hi0), D2D1::Point2F(y1, hi1)};
            src = D2D1::RectF(pad + s0 * pw, pad, pad + s1 * pw, pad + ph);
        }
        c.ImageQuad(f.key, image, src, quad, opacity);
    }
}

} // namespace atrium
