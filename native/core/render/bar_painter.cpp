#include "bar_painter.h"

#include "../shell/wifi.h"

#include <algorithm>
#include <cmath>
#include <set>

#pragma comment(lib, "dxguid.lib") // CLSID_D2D1Shadow

namespace atrium {

namespace {

// Positions (items, the bar's edges) ease towards new places at this rate per second (about 0.3 s).
constexpr float kSlideRate = 14;

// Critically damped approach towards `target`; `rate` in 1/s.
float Approach(float v, float target, double dtMs, float rate)
{
    float k = 1.0f - std::exp(-float(dtMs) / 1000.0f * rate);
    v += (target - v) * k;
    return std::fabs(target - v) < 0.002f ? target : v;
}

// The same for positions in px.
float ApproachPx(float v, float target, double dtMs, float rate)
{
    float k = 1.0f - std::exp(-float(dtMs) / 1000.0f * rate);
    v += (target - v) * k;
    return std::fabs(target - v) < 0.25f ? target : v;
}

float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

} // namespace

BarPainter::BarPainter(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite)
{
    dwrite_.copy_from(dwrite);
    winrt::check_hresult(d2d->CreateEffect(CLSID_D2D1Shadow, shadow_.put()));
}

IDWriteTextFormat* BarPainter::Format(float px, DWRITE_FONT_WEIGHT weight, const wchar_t* family)
{
    auto key = std::make_tuple(int(std::lround(px * 4)), int(weight), std::wstring(family));
    auto& f = formats_[key];
    if (!f) {
        dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", f.put());
        if (f) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    return f.get();
}

ID2D1Bitmap1* BarPainter::BitmapFor(ID2D1DeviceContext2* d2d, const SceneItem& item)
{
    if (!item.image || item.image->empty())
        return nullptr;
    auto& b = bitmaps_[item.imageId];
    if (!b) {
        const IconImage& img = *item.image;
        D2D1_BITMAP_PROPERTIES1 bp =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        d2d->CreateBitmap(D2D1::SizeU(UINT32(img.width), UINT32(img.height)), img.bgra.data(), UINT32(img.width * 4), &bp, b.put());
    }
    return b.get();
}

namespace {

// Integral of the magnification wave (a raised cosine over [-1, 1]) from 0 to x.
float WaveArea(float x)
{
    constexpr float pi = 3.14159265f;
    if (x >= 1)
        return 0.5f;
    if (x <= -1)
        return -0.5f;
    return x / 2 + std::sin(pi * x) / (2 * pi);
}

} // namespace

// Displayed position of base position u. The local scale is 1 + (mag-1) *
// amount * wave((t - centre) / range); integrating it from the pointer keeps
// the point under the pointer fixed, so the icon under the pointer in the base
// layout is the one shown under it, and the neighbours spread out like a wave.
float BarPainter::MapX(float u) const
{
    float k = (mag_ - 1) * amount_;
    if (k <= 0)
        return u;
    return u + k * range_ * WaveArea((u - centre_) / range_);
}

bool BarPainter::Animate(const BarScene& scene, double nowMs)
{
    double dt = lastMs_ == 0 ? 16 : std::clamp(nowMs - lastMs_, 0.0, 100.0);
    lastMs_ = nowMs;
    bool moving = false;
    // A new window geometry: items jump to their places instead of sliding
    // across from coordinates of the old window. (The window keeps its width
    // when apps come and go; then the bar's edges and the items slide.)
    bool snap = scene.windowW != lastWindowW_ || scene.bar.top != lastTop_ || scene.bar.bottom != lastBottom_;
    lastWindowW_ = scene.windowW;
    lastTop_ = scene.bar.top;
    lastBottom_ = scene.bar.bottom;
    auto slide = [&](float& v, float target) {
        if (snap) {
            v = target;
            return;
        }
        float n = ApproachPx(v, target, dt, kSlideRate);
        moving |= n != v;
        v = n;
    };
    slide(barL_, scene.bar.left);
    slide(barR_, scene.bar.right);
    if (scene.separatorX < 0 || sepX_ < 0)
        sepX_ = scene.separatorX;
    else
        slide(sepX_, scene.separatorX);
    slide(clockX_, (scene.clock.left + scene.clock.right) / 2);

    // Magnification: follows the pointer at once, grows and settles smoothly.
    bool hovering = scene.pointerX >= 0 && scene.dragKey.empty() && scene.magnify > 1.001f;
    mag_ = std::max(1.f, scene.magnify);
    range_ = std::max(1.f, scene.magnifyRange);
    if (hovering)
        centre_ = scene.pointerX;
    float amount = Approach(amount_, hovering ? 1.f : 0.f, dt, 13);
    moving |= amount != amount_;
    amount_ = amount;

    std::set<std::wstring> alive;
    auto step = [&](const std::wstring& key, const SceneItem* item) {
        alive.insert(key);
        Motion& m = motion_[key];
        float h = Approach(m.hover, scene.hoverKey == key || scene.dragKey == key ? 1.f : 0.f, dt, 18);
        float p = Approach(m.press, scene.pressedKey == key && scene.dragKey != key ? 1.f : 0.f, dt, 30);
        float a = Approach(m.appear, 1.f, dt, 10);
        float l = Approach(m.lift, scene.dragKey == key ? 1.f : 0.f, dt, 22);
        float d = Approach(m.dot, item && item->running ? 1.f : 0.f, dt, 12);
        moving |= h != m.hover || p != m.press || a != m.appear || l != m.lift || d != m.dot;
        m.hover = h;
        m.press = p;
        m.appear = a;
        m.lift = l;
        m.dot = d;
        m.leaving = false; // back again
        if (!item)
            return;
        float x = (item->plate.left + item->plate.right) / 2;
        if (!m.placed || snap || scene.dragKey == key) {
            m.x = x;
            m.placed = true;
        } else {
            float nx = ApproachPx(m.x, x, dt, kSlideRate);
            moving |= nx != m.x;
            m.x = nx;
        }
        // Launch bounce: hops of decreasing height; stops after the hop in
        // which the app started running.
        m.hop = 0;
        if (item->bounceMs > 0) {
            if (m.bounceMs != item->bounceMs) {
                m.bounceMs = item->bounceMs;
                m.bounceEnd = std::max(1, item->bounces);
            }
            constexpr double kHopMs = 560;
            double t = nowMs - m.bounceMs;
            int n = int(t / kHopMs);
            if (item->running && m.bounceEnd > n + 1)
                m.bounceEnd = n + 1;
            if (t >= 0 && n < m.bounceEnd) {
                float ph = float(std::fmod(t, kHopMs) / kHopMs);
                m.hop = 4 * ph * (1 - ph) * std::pow(0.72f, float(n));
                moving = true;
            }
        }
    };
    for (const auto& it : scene.items)
        step(it.key, &it);
    for (const auto& it : scene.extras)
        step(it.key, &it);
    step(L"#clock", nullptr);
    // Apps that went away shrink and fade out where they were while the
    // others close up; other entries just go.
    for (auto it = motion_.begin(); it != motion_.end();) {
        Motion& m = it->second;
        if (alive.count(it->first)) {
            ++it;
            continue;
        }
        auto last = lastApps_.find(it->first);
        if (!m.leaving && last != lastApps_.end() && !snap) {
            m.leaving = true;
            m.ghost = last->second;
            m.hover = m.press = m.lift = 0;
            m.hop = 0;
        }
        if (m.leaving) {
            m.appear = Approach(m.appear, 0.f, dt, 11);
            m.dot = Approach(m.dot, 0.f, dt, 14);
        }
        if (!m.leaving || m.appear < 0.02f || snap) {
            it = motion_.erase(it);
        } else {
            moving = true;
            ++it;
        }
    }
    lastApps_.clear();
    for (const auto& it : scene.items)
        lastApps_[it.key] = it;

    // What the glass shows this frame: the stretched shape and the ripple.
    frame_.magnified = amount_;
    frame_.hop = 0;
    for (const auto& [key, m] : motion_)
        frame_.hop = std::max(frame_.hop, m.hop);
    frame_.shape = D2D1::RectF(MapX(barL_), scene.bar.top, MapX(barR_), scene.bar.bottom);
    frame_.rippleA = 0;
    if (scene.rippleMs > 0) {
        constexpr double kRippleMs = 720;
        double t = nowMs - scene.rippleMs;
        if (t >= 0 && t < kRippleMs) {
            float u = float(t / kRippleMs);
            frame_.rippleX = MapX(scene.rippleX);
            frame_.rippleY = scene.rippleY;
            frame_.rippleR = (scene.bar.bottom - scene.bar.top) * 1.5f * EaseOut(u);
            frame_.rippleA = 0.9f * (1 - u) * (1 - u);
            moving = true;
        }
    }
    return moving;
}

void BarPainter::Draw(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date,
                      const std::wstring& contentKey, bool animating)
{
    winrt::com_ptr<ID2D1Image> target;
    d2d->GetTarget(target.put());
    auto size = d2d->GetPixelSize();
    if (layer_ && (layer_->GetPixelSize().width != size.width || layer_->GetPixelSize().height != size.height))
        layer_ = nullptr;
    if (!layer_) {
        D2D1_BITMAP_PROPERTIES1 bp =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d->CreateBitmap(size, nullptr, 0, &bp, layer_.put()))) {
            DrawContent(d2d, scene, ink, time, date); // no layer: draw directly
            return;
        }
        layerKey_.clear();
    }
    if (animating || contentKey != layerKey_) {
        d2d->SetTarget(layer_.get());
        d2d->Clear(D2D1::ColorF(0, 0, 0, 0));
        DrawContent(d2d, scene, ink, time, date);
        d2d->SetTarget(target.get());
        layerKey_ = animating ? std::wstring() : contentKey;
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    d2d->DrawImage(layer_.get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
}

void BarPainter::DrawContent(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink inks, const std::wstring& time, const std::wstring& date)
{
    const float s = scene.scale;
    // Ink colours: white on dark glass, near-black on light glass, per region.
    auto inkFor = [](bool dark) { return dark ? D2D1::ColorF(0.07f, 0.07f, 0.09f, 0.92f) : D2D1::ColorF(1, 1, 1, 0.96f); };
    auto haloFor = [](bool dark) { return dark ? D2D1::ColorF(1, 1, 1, 0.35f) : D2D1::ColorF(0, 0, 0, 0.32f); };
    auto washFor = [](bool dark) { return dark ? D2D1::ColorF(0, 0, 0, 0.10f) : D2D1::ColorF(1, 1, 1, 0.20f); };
    bool darkText = inks.darkClock;
    D2D1_COLOR_F ink = inkFor(inks.darkApps);
    D2D1_COLOR_F halo = haloFor(darkText);
    D2D1_COLOR_F wash = washFor(inks.darkApps);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    d2d->CreateSolidColorBrush(ink, brush.put());
    d2d->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // premultiplied target: no ClearType

    usedBitmaps_.clear();
    for (const auto& it : scene.extras) { // the right group follows the clock's ink, the others the apps'
        bool right = it.key == L"#notify" || it.key == L"#tray" || it.key == L"#status";
        DrawItem(d2d, scene, it, brush.get(), right ? inkFor(darkText) : ink, right ? washFor(darkText) : wash);
    }
    for (const auto& [key, m] : motion_)
        if (m.leaving)
            DrawItem(d2d, scene, m.ghost, brush.get(), ink, wash);
    for (const auto& it : scene.items)
        if (it.key != scene.dragKey)
            DrawItem(d2d, scene, it, brush.get(), ink, wash);
    for (const auto& it : scene.items)
        if (it.key == scene.dragKey)
            DrawItem(d2d, scene, it, brush.get(), ink, wash); // on top of the others
    // Drop bitmaps of icons that are gone or were replaced.
    for (auto it = bitmaps_.begin(); it != bitmaps_.end();)
        it = usedBitmaps_.count(it->first) ? std::next(it) : bitmaps_.erase(it);

    if (sepX_ >= 0) {
        D2D1_COLOR_F c = inkFor(darkText);
        c.a = darkText ? 0.18f : 0.30f;
        brush->SetColor(c);
        float h = (scene.bar.bottom - scene.bar.top) * 0.46f, mid = (scene.bar.top + scene.bar.bottom) / 2, x = MapX(sepX_);
        d2d->FillRectangle(D2D1::RectF(x - 0.5f * s, mid - h / 2, x + 0.5f * s, mid + h / 2), brush.get());
    }

    if (scene.clock.right > scene.clock.left) {
        const Motion& m = motion_[L"#clock"];
        ink = inkFor(darkText);
        // The clock does not grow; it moves with the stretched bar.
        float shift = MapX(clockX_) - (scene.clock.left + scene.clock.right) / 2;
        d2d->SetTransform(D2D1::Matrix3x2F::Translation(shift, 0));
        if (m.hover > 0.001f) {
            D2D1_COLOR_F c = washFor(darkText);
            c.a *= EaseOut(m.hover);
            brush->SetColor(c);
            float r = scene.plateRadius;
            d2d->FillRoundedRectangle(D2D1::RoundedRect(scene.clock, r, r), brush.get());
        }
        float lineT = scene.timePx * 1.22f, lineD = scene.datePx * 1.25f;
        float top = (scene.clock.top + scene.clock.bottom) / 2 - (lineT + lineD) / 2;
        auto text = [&](const std::wstring& str, IDWriteTextFormat* f, float y0, float y1) {
            if (!f)
                return;
            float o = std::max(1.0f, s * 0.6f);
            brush->SetColor(halo);
            d2d->DrawText(str.c_str(), UINT32(str.size()), f, D2D1::RectF(scene.clock.left, y0 + o, scene.clock.right, y1 + o), brush.get());
            brush->SetColor(ink);
            d2d->DrawText(str.c_str(), UINT32(str.size()), f, D2D1::RectF(scene.clock.left, y0, scene.clock.right, y1), brush.get());
        };
        text(time, Format(scene.timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD), top, top + lineT);
        text(date, Format(scene.datePx, DWRITE_FONT_WEIGHT_NORMAL), top + lineT, top + lineT + lineD);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

void BarPainter::DrawItem(ID2D1DeviceContext2* d2d, const BarScene& scene, const SceneItem& it, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink,
                          D2D1_COLOR_F wash)
{
    const float s = scene.scale;
    const Motion& m = motion_[it.key];
    const bool magnifying = mag_ > 1.001f;
    // Base position: everything of the item slides with its plate (reordering,
    // drag and drop). Displayed position: the base one under the magnification.
    float dx = m.placed ? m.x - (it.plate.left + it.plate.right) / 2 : 0;
    D2D1_RECT_F plate = D2D1::RectF(it.plate.left + dx, it.plate.top, it.plate.right + dx, it.plate.bottom);
    D2D1_RECT_F icon = D2D1::RectF(it.icon.left + dx, it.icon.top, it.icon.right + dx, it.icon.bottom);
    float size = icon.right - icon.left;
    float bx = (icon.left + icon.right) / 2, bottom = icon.bottom;
    float pw = std::max(1.f, plate.right - plate.left);
    float grow = (MapX(bx + pw / 2) - MapX(bx - pw / 2)) / pw; // local magnification
    float cx = MapX(bx);
    float hover = EaseOut(m.hover), lift = EaseOut(m.lift);

    // Plate (only without magnification): hover, press and drag feedback.
    // Its corners are concentric with the bar's.
    float plateA = (magnifying ? 0.f : hover) + 0.35f * m.press + 0.4f * lift;
    if (plateA > 0.001f) {
        D2D1_COLOR_F c = wash;
        c.a *= std::min(plateA, 1.6f);
        brush->SetColor(c);
        float r = scene.plateRadius;
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(grow, grow, D2D1::Point2F(bx, plate.bottom)) * D2D1::Matrix3x2F::Translation(cx - bx, 0));
        d2d->FillRoundedRectangle(D2D1::RoundedRect(plate, r, r), brush);
    }

    // Icon: grows from its bottom edge with the wave, hops when launching,
    // dips on press, fades in when new; lifts while dragged.
    float k = grow * (magnifying ? 1.f : 1 + 0.06f * hover) * (1 + 0.08f * lift) * (1 - 0.08f * m.press) * (0.85f + 0.15f * m.appear) *
              (m.leaving ? 0.45f + 0.55f * m.appear : 1.f);
    float up = m.hop * 0.55f * size + (magnifying ? 0.f : 1.5f * s * hover) + 2.0f * s * lift;
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(bx, bottom)) * D2D1::Matrix3x2F::Translation(cx - bx, -up));
    if (it.key == L"#status") {
        bool darkInk = ink.r < 0.5f;
        DrawStatus(d2d, scene.status, icon, brush, ink, darkInk);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }
    if (it.key == L"#start") {
        DrawLogo(d2d, icon, m.appear);
    } else if (!it.glyph.empty()) {
        if (IDWriteTextFormat* f = Format(size * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
            brush->SetColor(ink);
            d2d->DrawText(it.glyph.c_str(), UINT32(it.glyph.size()), f, icon, brush);
        }
    } else if (ID2D1Bitmap1* bmp = BitmapFor(d2d, it)) {
        usedBitmaps_.insert(it.imageId);
        shadow_->SetInput(0, bmp);
        shadow_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, (1.6f + 1.6f * lift + 1.2f * m.hop) * s);
        shadow_->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, (0.38f + 0.12f * lift) * m.appear));
        auto px = bmp->GetPixelSize();
        float sx = size / float(px.width), sy = size / float(px.height);
        D2D1_MATRIX_3X2_F t;
        d2d->GetTransform(&t);
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(icon.left, icon.top + (1.2f + 1.5f * lift) * s) * t);
        d2d->DrawImage(shadow_.get(), D2D1_INTERPOLATION_MODE_LINEAR);
        d2d->SetTransform(t);
        d2d->DrawBitmap(bmp, icon, m.appear, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else {
        // Placeholder while the icon loads: a quiet rounded square.
        D2D1_COLOR_F c = wash;
        c.a *= 1.4f * m.appear;
        brush->SetColor(c);
        d2d->FillRoundedRectangle(D2D1::RoundedRect(icon, size * 0.22f, size * 0.22f), brush);
    }
    // Count badge (unread notifications): a small accent disc on the icon's corner.
    if (it.badge > 0) {
        float d = std::max(9.f * s, size * 0.46f);
        float bxr = icon.right - d * 0.35f, by = icon.top + d * 0.15f;
        brush->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1.0f));
        d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bxr, by), d / 2, d / 2), brush);
        std::wstring n = it.badge > 9 ? L"9+" : std::to_wstring(it.badge);
        if (IDWriteTextFormat* f = Format(d * 0.62f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) {
            brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
            d2d->DrawText(n.c_str(), UINT32(n.size()), f, D2D1::RectF(bxr - d, by - d / 2, bxr + d, by + d / 2), brush);
        }
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());

    // Running indicator: a small dot under the icon, centred in the gap to the
    // bar's lower edge; full ink for the foreground app, amber when the app
    // asks for attention. It stays on the bar while the icon hops or grows.
    if (m.dot > 0.01f && lift < 0.99f) {
        float r = (it.active ? 2.3f : 1.9f) * s;
        float y = (it.icon.bottom + scene.bar.bottom) / 2;
        D2D1_COLOR_F c = it.attention ? D2D1::ColorF(1.0f, 0.70f, 0.18f, 0.98f) : ink;
        if (!it.active && !it.attention)
            c.a *= 0.45f;
        c.a *= (1 - lift) * m.appear * m.dot;
        brush->SetColor(c);
        d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y), r, r), brush);
    }
}

void BarPainter::DrawStatus(ID2D1DeviceContext2* d2d, const SystemStatus& st, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink,
                            bool darkInk)
{
    const float h = r.bottom - r.top, cy = (r.top + r.bottom) / 2;
    const float wifiW = h * 0.66f, gap = h * 0.20f, batW = h * 0.92f, nub = h * 0.07f;
    const bool second = st.battery || st.audio;
    const float total = wifiW + (second ? gap + (st.battery ? batW + nub : h * 0.62f) : 0);
    const float x0 = (r.left + r.right) / 2 - total / 2;
    auto dim = [&](float a) {
        D2D1_COLOR_F c = ink;
        c.a *= a;
        return c;
    };

    // Network: Wi-Fi arcs lit by strength; a plug glyph when wired.
    if (st.connected && !st.wifi) {
        if (IDWriteTextFormat* f = Format(h * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
            brush->SetColor(ink);
            d2d->DrawText(L"\uE839", 1, f, D2D1::RectF(x0, r.top, x0 + wifiW, r.bottom), brush);
        }
    } else {
        winrt::com_ptr<ID2D1Factory> factory;
        d2d->GetFactory(factory.put());
        const int lit = st.connected ? WifiBars(st.signal) : 0;
        const float cx = x0 + wifiW / 2, base = cy + h * 0.27f, stroke = h * 0.085f;
        brush->SetColor(lit >= 1 ? ink : dim(0.32f));
        d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, base - stroke * 0.4f), stroke * 0.85f, stroke * 0.85f), brush);
        winrt::com_ptr<ID2D1StrokeStyle> round;
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, round.put());
        for (int k = 1; k <= 3; ++k) {
            float rad = h * (0.10f + 0.115f * float(k));
            const float a0 = 3.14159f * 1.25f, a1 = 3.14159f * 1.75f; // a quarter circle, opening upwards
            winrt::com_ptr<ID2D1PathGeometry> arc;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(arc.put())) || FAILED(arc->Open(sink.put())))
                continue;
            sink->BeginFigure(D2D1::Point2F(cx + rad * std::cos(a0), base + rad * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + rad * std::cos(a1), base + rad * std::sin(a1)), D2D1::SizeF(rad, rad), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            brush->SetColor(lit > k ? ink : dim(0.32f));
            d2d->DrawGeometry(arc.get(), brush, stroke, round.get());
        }
    }
    if (!second)
        return;
    float x = x0 + wifiW + gap;
    if (!st.battery) { // a desktop: the volume instead
        const wchar_t* g = st.muted ? L"\uE74F" : st.volume < 34 ? L"\uE993" : st.volume < 67 ? L"\uE994" : L"\uE995";
        if (IDWriteTextFormat* f = Format(h * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
            brush->SetColor(st.muted ? dim(0.5f) : ink);
            d2d->DrawText(g, 1, f, D2D1::RectF(x, r.top, x + h * 0.62f, r.bottom), brush);
        }
        return;
    }
    // Battery: a capsule filled to the level, the level written inside it.
    const float bh = h * 0.50f, br = bh * 0.34f, line = std::max(1.f, h * 0.045f), pad = h * 0.055f;
    D2D1_RECT_F body = D2D1::RectF(x, cy - bh / 2, x + batW, cy + bh / 2);
    brush->SetColor(dim(0.45f));
    d2d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left + line / 2, body.top + line / 2, body.right - line / 2, body.bottom - line / 2),
                                                br, br),
                              brush, line);
    d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.right + h * 0.02f, cy - bh * 0.18f, body.right + h * 0.02f + nub, cy + bh * 0.18f),
                                                nub / 2, nub / 2),
                              brush);
    const int pct = std::clamp(st.batteryPercent, 0, 100);
    D2D1_RECT_F inner = D2D1::RectF(body.left + pad, body.top + pad, body.right - pad, body.bottom - pad);
    D2D1_RECT_F fill = inner;
    fill.right = inner.left + (inner.right - inner.left) * float(pct) / 100.f;
    D2D1_COLOR_F level = st.charging ? D2D1::ColorF(0x30D158) : pct <= 20 ? D2D1::ColorF(0xFF453A) : ink;
    brush->SetColor(level);
    float ir = std::max(0.f, br - pad);
    d2d->FillRoundedRectangle(D2D1::RoundedRect(fill, ir, ir), brush);
    if (IDWriteTextFormat* f = Format(bh * 0.66f, DWRITE_FONT_WEIGHT_BOLD)) {
        std::wstring n = std::to_wstring(pct);
        // On the fill the number takes the glass's side of the ink, elsewhere the ink.
        bool onFill = pct >= 55 || st.charging;
        brush->SetColor(onFill ? (st.charging || darkInk ? D2D1::ColorF(1, 1, 1, 0.98f) : D2D1::ColorF(0.05f, 0.05f, 0.07f, 0.95f)) : ink);
        d2d->DrawText(n.c_str(), UINT32(n.size()), f, D2D1::RectF(body.left, body.top - h * 0.02f, body.right, body.bottom - h * 0.02f), brush);
    }
}

// The start entry: the Windows logo, redrawn as four panes with Windows 11's
// light-to-deep blue, slightly smaller than an app icon's square.
void BarPainter::DrawLogo(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, float opacity)
{
    float w = std::min(r.right - r.left, r.bottom - r.top) * 0.80f;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float left = cx - w / 2, top = cy - w / 2;
    float gap = w * 0.08f, pane = (w - gap) / 2, corner = pane * 0.10f;
    D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0x5CCBFF, opacity)}, {0.5f, D2D1::ColorF(0x1A9BF0, opacity)}, {1.f, D2D1::ColorF(0x0060C8, opacity)}};
    winrt::com_ptr<ID2D1GradientStopCollection> coll;
    winrt::com_ptr<ID2D1LinearGradientBrush> grad;
    if (FAILED(d2d->CreateGradientStopCollection(stops, 3, coll.put())) ||
        FAILED(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(left, top), D2D1::Point2F(left + w, top + w)),
                                              coll.get(), grad.put())))
        return;
    winrt::com_ptr<ID2D1SolidColorBrush> sheen;
    d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f * opacity), sheen.put());
    for (int i = 0; i < 4; ++i) {
        float x = left + float(i % 2) * (pane + gap), y = top + float(i / 2) * (pane + gap);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + pane, y + pane), corner, corner);
        d2d->FillRoundedRectangle(rr, grad.get());
        // A faint lit edge on top of each pane.
        d2d->FillRectangle(D2D1::RectF(x + corner, y, x + pane - corner, y + std::max(0.6f, pane * 0.06f)), sheen.get());
    }
}

} // namespace atrium
