// Stage Manager's strip and flight rendering (perspective cards, icons, fade-out blur, and window flights).
#include "ui/stage_manager.h"

#include "clock.h"
#include "ui/stage_fade.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr float kHoverTilt = 0.7f, kDepth = 340, kComeTilt = 34, kKeystone = 0.45f;
constexpr float kStackDx = 9, kStackDy = 8, kStackZ = 26;
constexpr float kShadowDrop = 6, kShadowAlpha = 0.5f;
constexpr float kIcon = 34;
constexpr double kFlightHoldMs = 150, kFlightFadeMs = 150, kFlightStepMs = 34;
constexpr float kRestoreAt = 0.86f;
constexpr UINT kFlightIdleMs = 60000;

D2D1::Matrix4x4F Tilt(float cx, float cy, float ox, float oy, float angle, float depth, float z)
{
    using M = D2D1::Matrix4x4F;
    return M::Translation(-cx, -cy, 0) * M::RotationY(angle) * M::Translation(cx - ox, cy - oy, z) * M::PerspectiveProjection(depth) *
           M::Translation(ox, oy, 0);
}

float EaseInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

D2D1_POINT_2F Project(const D2D1_MATRIX_4X4_F& m, float x, float y)
{
    const float px = x * m._11 + y * m._21 + m._41, py = x * m._12 + y * m._22 + m._42, pw = x * m._14 + y * m._24 + m._44;
    return D2D1::Point2F(px / pw, py / pw);
}

float SmoothStep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

} // namespace

StageManager::Pose StageManager::PoseOf(const Entry& e, size_t j, const WindowPicture& p, float angle) const
{
    const float dir = onRight_ ? -1.f : 1.f;
    const D2D1_RECT_F box = Box(e);
    Pose pose;
    pose.cx = pose.ox = (box.left + box.right) / 2;
    pose.cy = (box.top + box.bottom) / 2;
    pose.oy = pose.cy + kKeystone * (size_.height / 2 - pose.cy);
    pose.angle = angle;
    pose.z = -kStackZ * float(j);
    const float f = std::min(kBoxW / float(p.width), kBoxH / float(p.height)) * std::pow(0.93f, float(j)); // DIP per picture px
    const float w = float(p.width) * f, h = float(p.height) * f;
    const float x = pose.cx - w / 2 + dir * kStackDx * float(j), y = pose.cy - h / 2 - kStackDy * float(j);
    pose.rect = D2D1::RectF(x, y, x + w, y + h);
    return pose;
}

D2D1_POINT_2F StageManager::StripOffset() const
{
    const MONITORINFO mi = MonitorInfo();
    const float s = PopupScale(mi.rcWork);
    return D2D1::Point2F(float(rect_.left - mi.rcMonitor.left) / s, float(rect_.top - mi.rcMonitor.top) / s);
}

bool StageManager::StripPose(HWND window, Pose& pose)
{
    if (!shown_)
        return false;
    for (const Entry& e : entries_) {
        auto at = std::ranges::find(e.windows, window);
        if (at == e.windows.end())
            continue;
        auto parked = parked_.find(window);
        std::wstring key;
        auto p = PictureOf(window, parked != parked_.end() ? parked->second.key : e.keys.front(), key);
        if (!p || !p->ok)
            return false;
        Entry rest = e;
        rest.y = rest.targetY;
        pose = PoseOf(rest, size_t(at - e.windows.begin()), *p, (onRight_ ? -1.f : 1.f) * kTilt);
        const D2D1_POINT_2F o = StripOffset();
        pose.rect = D2D1::RectF(pose.rect.left + o.x, pose.rect.top + o.y, pose.rect.right + o.x, pose.rect.bottom + o.y);
        pose.cx += o.x, pose.ox += o.x, pose.cy += o.y, pose.oy += o.y;
        return true;
    }
    return false;
}

StageManager::Pose StageManager::PlacePose(const RECT& frame) const
{
    const MONITORINFO mi = MonitorInfo();
    const float s = PopupScale(mi.rcWork);
    Pose pose;
    pose.rect = D2D1::RectF(float(frame.left - mi.rcMonitor.left) / s, float(frame.top - mi.rcMonitor.top) / s, float(frame.right - mi.rcMonitor.left) / s,
                            float(frame.bottom - mi.rcMonitor.top) / s);
    pose.cx = pose.ox = (pose.rect.left + pose.rect.right) / 2;
    pose.cy = pose.oy = (pose.rect.top + pose.rect.bottom) / 2;
    return pose;
}

void StageManager::DrawPicture(Canvas& c, const WindowPicture& p, const std::wstring& key, const Pose& pose, float opacity, float shadow, bool image, int fade)
{
    if (!p.ok || p.width <= 0 || p.height <= 0 || opacity < 0.004f)
        return;
    const float s = c.scale;
    const D2D1::Matrix4x4F m = Tilt(pose.cx * s, pose.cy * s, pose.ox * s, pose.oy * s, pose.angle, kDepth * s, pose.z * s);
    if (fade != 0) {
        DrawFaded(c, p, key, pose, m, opacity, shadow, fade);
        return;
    }
    const D2D1_RECT_F& r = pose.rect;
    const float f = (r.right - r.left) / float(p.width), sp = float(kShadowPad) * f, pp = float(kPicturePad) * f;
    if (shadow > 0.004f && !p.shadow.Empty())
        c.ImagePerspective(key + L"|s", p.shadow, D2D1::RectF(r.left - sp, r.top - sp, r.right + sp, r.bottom + sp),
                           m * D2D1::Matrix4x4F::Translation(0, kShadowDrop * s, 0), kShadowAlpha * opacity * shadow);
    if (image)
        c.ImagePerspective(key, p.image, D2D1::RectF(r.left - pp, r.top - pp, r.right + pp, r.bottom + pp), m, opacity);
}

// An end card's picture and shadow faded away (stage_fade.h): made once a picture, in its own px,
// so they turn with the card. `fade` 1: away down (the last card), -1: away up (the first).
void StageManager::DrawFaded(Canvas& c, const WindowPicture& p, const std::wstring& key, const Pose& pose, const D2D1::Matrix4x4F& m, float opacity,
                             float shadow, int fade)
{
    const D2D1_RECT_F& r = pose.rect;
    const float f = (r.right - r.left) / float(p.width), sp = float(kShadowPad) * f;
    const bool up = fade < 0;
    const std::wstring faded = key + (up ? L"|fu" : L"|fd");
    if (shadow > 0.004f && !p.shadow.Empty()) {
        const float k = float(p.shadow.height) / float(p.height + 2 * kShadowPad); // shadow px a picture px
        const StageFade plain{p.shadow.width, 0, 0};
        const IconImage made = c.Cached(faded + L"|s") ? IconImage{} : FadeAway(p.shadow, float(kShadowPad) * k, float(kShadowPad + p.height) * k, plain, up);
        c.ImagePerspective(faded + L"|s", made, D2D1::RectF(r.left - sp, r.top - sp, r.right + sp, r.bottom + sp),
                           m * D2D1::Matrix4x4F::Translation(0, kShadowDrop * c.scale, 0), kShadowAlpha * opacity * shadow);
    }
    const StageFade sf = StageFadeFor(p.image.width, kBoxW * c.scale);
    const IconImage made = c.Cached(faded) ? IconImage{} : FadeAway(p.image, float(kPicturePad), float(kPicturePad + p.height), sf, up);
    const float pp = (float(kPicturePad) + sf.margin) * f;
    c.ImagePerspective(faded, made, D2D1::RectF(r.left - pp, r.top - pp, r.right + pp, r.bottom + pp), m, opacity);
}

void StageManager::PaintEntryIcons(Canvas& c, Entry& e, const Pose& front, D2D1_RECT_F box, float slide, float k, float hv, int index, bool live, bool icons)
{
    const float s = c.scale, dir = onRight_ ? -1.f : 1.f;
    const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    const D2D1::Matrix4x4F m = Tilt(front.cx * s, front.cy * s, front.ox * s, front.oy * s, front.angle, kDepth * s, front.z * s);
    const float outer = onRight_ ? front.rect.right : front.rect.left;
    const float bottom = Project(m, outer * s, front.rect.bottom * s).y / s;
    const int iconPx = int(std::lround(kIcon * s));
    static const IconImage kShadow = SoftShadow(96, 96);
    std::vector<std::wstring> apps; // one icon an app, though several of its windows are in the set
    for (const std::wstring& key : e.keys)
        if (icons && !HasKey(apps, AppKeyOf(key)) && apps.size() < 3)
            apps.push_back(AppKeyOf(key));
    for (size_t j = apps.size(); j-- > 0;) {
        IconImage icon = tasks_.Icon(apps[j], iconPx);
        if (icon.Empty())
            continue;
        const float ix = (onRight_ ? box.right : box.left) + dir * (10 + 24 * float(j)), iy = bottom - 6;
        const D2D1_RECT_F ir = D2D1::RectF(ix - kIcon / 2, iy - kIcon / 2, ix + kIcon / 2, iy + kIcon / 2);
        const float sp = kIcon * float(kShadowPad) / 96;
        c.Image(L"stage|shadow", kShadow, D2D1::RectF(ir.left - sp, ir.top - sp + 2, ir.right + sp, ir.bottom + sp + 2), 0.45f);
        c.Image(L"stage|" + apps[j] + L"|" + std::to_wstring(iconPx), icon, ir);
    }
    const D2D1_POINT_2F corner = Project(m, front.rect.right * s, front.rect.top * s);
    const D2D1_POINT_2F at{corner.x / s, corner.y / s};
    PaintClose(c, e, index, at, D2D1::Point2F(cx + (at.x - cx) * k + slide, cy + (at.y - cy) * k), live ? hv : 0.f);
}

void StageManager::PaintEntry(Canvas& c, Entry& e, int index, bool live, int fade)
{
    const float dir = onRight_ ? -1.f : 1.f;
    const float hv = e.hover.Step(live && (index == hover_ || index == dragHover_), c.now);
    const float into = e.drop.Step(live && index == dragHover_, c.now);
    if (e.hover.moving || e.drop.moving)
        c.Continue();
    const float rv = Reveal(std::max(index, 0), c.now);
    const float opacity = e.appear * std::clamp(rv * 1.6f, 0.f, 1.f);
    // A card whose pictures are all on their way in (or out) shows once they are there.
    if (opacity < 0.004f || std::ranges::all_of(e.windows, [&](HWND h) { return Flying(h); }))
        return;
    const D2D1_RECT_F box = Box(e);
    const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    const float k = (0.9f + 0.1f * e.appear) * (1 + 0.06f * hv + 0.04f * into) * (live && index == pressed_ ? 0.96f : 1.f);
    // Out of the screen's edge, turning to the eye as it comes.
    const float slide = onRight_ ? (size_.width - box.left + 24) * (1 - rv) : -(box.right + 24) * (1 - rv);
    const float angle = dir * (kTilt * (1 - kHoverTilt * hv) + kComeTilt * (1 - rv));
    c.BeginGroup(opacity, k, cx, cy, slide, 0);
    if (into > 0.004f) {
        const D2D1_RECT_F ring = D2D1::RectF(box.left - 10, box.top - 10, box.right + 10, box.bottom + 10);
        c.Fill(ring, 14, D2D1::ColorF(c.theme.accent.r, c.theme.accent.g, c.theme.accent.b, 0.14f * into));
        c.Stroke(ring, 14, D2D1::ColorF(c.theme.accent.r, c.theme.accent.g, c.theme.accent.b, 0.85f * into), 2.f);
    }
    // Back to front: the windows behind the most recent one a step inwards, up and away.
    Pose front{};
    bool hasFront = false;
    for (size_t j = e.windows.size(); j-- > 0;) {
        HWND h = e.windows[j];
        if (Flying(h))
            continue;
        auto parked = parked_.find(h);
        std::wstring key;
        auto p = PictureOf(h, parked != parked_.end() ? parked->second.key : e.keys.front(), key);
        if (!p || !p->ok || p->width <= 0 || p->height <= 0)
            continue;
        const Pose pose = PoseOf(e, j, *p, angle);
        DrawPicture(c, *p, key, pose, 1, 1, true, fade);
        if (!hasFront || j == 0)
            front = pose, hasFront = true;
    }
    if (hasFront)
        PaintEntryIcons(c, e, front, box, slide, k, hv, index, live, fade <= 0); // the last card's would sit in its fade
    c.EndGroup();
}

bool StageManager::StepFlight(Canvas& c, Flight& f)
{
    const float dir = onRight_ ? -1.f : 1.f;
    // Frame by frame, at most kFlightStepMs a frame: while windows go down or come up the screen
    // may not be composed for a moment; the picture waits rather than jumps.
    if (f.last > 0)
        f.elapsed += std::clamp(c.now - f.last, 0.0, kFlightStepMs);
    f.last = c.now;
    const float t = float(std::clamp(f.elapsed / kFlightMs, 0.0, 1.0)), e = EaseInOut(t);
    Pose a = f.toStrip ? f.place : f.from, b = f.toStrip ? Pose{} : f.place;
    float opacity = 1;
    if (f.toStrip && !StripPose(f.window, b)) {
        // The strip is not out: into the screen's edge.
        const D2D1_SIZE_F screen =
            D2D1::SizeF(float(flight_.Rect().right - flight_.Rect().left) / c.scale, float(flight_.Rect().bottom - flight_.Rect().top) / c.scale);
        const float x = onRight_ ? screen.width + kBoxW / 2 : -kBoxW / 2, y = StripOffset().y + size_.height / 2;
        b.rect = D2D1::RectF(x - kBoxW / 2, y - kBoxH / 2, x + kBoxW / 2, y + kBoxH / 2);
        b.cx = b.ox = x;
        b.cy = b.oy = y;
        b.angle = dir * kTilt;
        opacity = 1 - e * e;
    }
    if (!f.toStrip && t >= kRestoreAt && !f.restored) {
        f.restored = true;
        restoring_[f.window] = GetTickCount64();
        EnqueueAction({f.activate ? Action::Kind::Activate : Action::Kind::ShowQuietly, f.window});
    }
    const double over = f.elapsed - kFlightMs - kFlightHoldMs;
    if (!f.toStrip && over > 0)
        opacity = 1 - float(std::clamp(over / kFlightFadeMs, 0.0, 1.0));
    Pose p;
    p.rect = D2D1::RectF(Lerp(a.rect.left, b.rect.left, e), Lerp(a.rect.top, b.rect.top, e), Lerp(a.rect.right, b.rect.right, e),
                         Lerp(a.rect.bottom, b.rect.bottom, e));
    p.cx = Lerp(a.cx, b.cx, e), p.cy = Lerp(a.cy, b.cy, e), p.ox = Lerp(a.ox, b.ox, e), p.oy = Lerp(a.oy, b.oy, e);
    p.angle = Lerp(a.angle, b.angle, e), p.z = Lerp(a.z, b.z, e);
    const float toPlace = f.toStrip ? 1 - e : e;
    const float large = f.large ? SmoothStep(0.15f, 0.8f, toPlace) : 0.f;
    // Once the window is up under it (kRestoreAt), the picture's shadow fades as the window's own
    // takes over, gone by the landing: cut at once it was a shadow that vanished; kept while the
    // picture fades it would darken the window under it for a moment.
    const float shade = f.toStrip ? 1.f : 1.f - SmoothStep(kRestoreAt - 0.04f, 1.f, t);
    DrawPicture(c, *f.picture, f.key, p, opacity, shade, large < 0.999f);
    if (large > 0)
        DrawPicture(c, *f.large, f.largeKey, p, opacity * large, 0.f);
    return f.toStrip ? t >= 1 : over >= kFlightFadeMs;
}

// The pictures in flight, over everything: into the strip (the card shows them once they land) or
// back to their windows' places (the windows come up under them, then they fade).
void StageManager::PaintFlights(Canvas& c)
{
    const bool drew = !flights_.empty();
    bool ended = false;
    for (auto it = flights_.begin(); it != flights_.end();) {
        if (!StepFlight(c, *it)) {
            ++it;
            continue;
        }
        // Up again: its own animations back, unless minimizing it on the stage sends its picture
        // into the strip (then it stays without, and is looked at as the stage's from now).
        if (!it->toStrip && minimizeToStrip_ && enabled_ && !IsIconic(it->window)) {
            RECT r{};
            if (FAILED(DwmGetWindowAttribute(it->window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
                GetWindowRect(it->window, &r);
            up_[it->window] = r;
        } else if (!it->toStrip) {
            SetTransitions(it->window, true);
        }
        it = flights_.erase(it);
        ended = true;
        surface_.Invalidate(); // the card shows its picture now
    }
    if (ended && flights_.empty() && shrinkAfterFlight_)
        ScheduleOldLargeShrinks();
    if (!flights_.empty() || drew) {
        c.Continue();
    } else {
        // Clicks go through until the next flight; the layer goes if none comes for a while.
        HRGN none = CreateRectRgn(0, 0, 0, 0);
        if (!SetWindowRgn(flight_.Hwnd(), none, FALSE))
            DeleteObject(none);
        SetTimer(flight_.Hwnd(), kFlightTimer, kFlightIdleMs, nullptr);
    }
}

void StageManager::Paint(Canvas& c)
{
    const bool moving = Animate(c.now);
    for (Entry& e : leaving_)
        PaintEntry(c, e, -1, false);
    // More sets than the strip holds (the wheel pages through them, round): the last card blurs more and
    // more down to nothing, the first the same way up, so the strip reads as one that goes on (#112).
    const bool more = scrollCount_ > entries_.size() && entries_.size() > 1;
    for (size_t i = 0; i < entries_.size(); ++i)
        PaintEntry(c, entries_[i], int(i), true, !more ? 0 : i + 1 == entries_.size() ? 1 : i == 0 ? -1 : 0);
    UpdateRegion(moving);
    if (moving)
        c.Continue();
    else if (!shown_ || (entries_.empty() && leaving_.empty()))
        SetTimer(surface_.Hwnd(), kHideTimer, 1, nullptr); // back in the edge, or the last card gone: not from inside a paint
}

} // namespace atrium
