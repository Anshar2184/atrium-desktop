#include "bar_painter.h"

#include <algorithm>
#include <cmath>
#include <set>

#pragma comment(lib, "dxguid.lib") // CLSID_D2D1Shadow

namespace atrium {

namespace {

// Critically damped approach towards `target`; `rate` in 1/s.
float Approach(float v, float target, double dtMs, float rate)
{
    float k = 1.0f - std::exp(-float(dtMs) / 1000.0f * rate);
    v += (target - v) * k;
    return std::fabs(target - v) < 0.002f ? target : v;
}

float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

} // namespace

BarPainter::BarPainter(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite)
{
    dwrite_.copy_from(dwrite);
    winrt::check_hresult(d2d->CreateEffect(CLSID_D2D1Shadow, shadow_.put()));
}

IDWriteTextFormat* BarPainter::Format(float px, DWRITE_FONT_WEIGHT weight)
{
    auto key = std::make_pair(int(std::lround(px * 4)), int(weight));
    auto& f = formats_[key];
    if (!f) {
        dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"",
                                  f.put());
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

bool BarPainter::Animate(const BarScene& scene, double nowMs)
{
    double dt = lastMs_ == 0 ? 16 : std::clamp(nowMs - lastMs_, 0.0, 100.0);
    lastMs_ = nowMs;
    bool moving = false;
    std::set<std::wstring> alive;
    auto step = [&](const std::wstring& key) {
        alive.insert(key);
        Motion& m = motion_[key];
        float h = Approach(m.hover, scene.hoverKey == key ? 1.f : 0.f, dt, 18);
        float p = Approach(m.press, scene.pressedKey == key ? 1.f : 0.f, dt, 30);
        float a = Approach(m.appear, 1.f, dt, 10);
        moving |= h != m.hover || p != m.press || a != m.appear;
        m = {h, p, a};
    };
    for (const auto& it : scene.items)
        step(it.key);
    step(L"#clock");
    for (auto it = motion_.begin(); it != motion_.end();)
        it = alive.count(it->first) ? std::next(it) : motion_.erase(it);
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

    std::set<uint64_t> used;
    for (const auto& it : scene.items) {
        const Motion& m = motion_[it.key];
        float size = it.icon.right - it.icon.left;
        float cx = (it.icon.left + it.icon.right) / 2, cy = (it.icon.top + it.icon.bottom) / 2;
        float hover = EaseOut(m.hover);

        // Hover: a soft glass plate behind the icon.
        if (hover > 0.001f) {
            float plate = size * 1.30f;
            D2D1_COLOR_F c = wash;
            c.a *= hover;
            brush->SetColor(c);
            d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - plate / 2, cy - plate / 2, cx + plate / 2, cy + plate / 2), plate * 0.26f,
                                                        plate * 0.26f),
                                      brush.get());
        }

        // Icon: lifts and grows slightly on hover, dips on press, fades in when new.
        float k = (1 + 0.07f * hover) * (1 - 0.08f * m.press) * (0.85f + 0.15f * m.appear);
        float lift = -1.5f * s * hover;
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(cx, cy)) * D2D1::Matrix3x2F::Translation(0, lift));
        if (ID2D1Bitmap1* bmp = BitmapFor(d2d, it)) {
            used.insert(it.imageId);
            D2D1_RECT_F dst = it.icon;
            shadow_->SetInput(0, bmp);
            shadow_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 1.6f * s);
            shadow_->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.38f * m.appear));
            auto size2 = bmp->GetPixelSize();
            float sx = size / float(size2.width), sy = size / float(size2.height);
            D2D1_MATRIX_3X2_F t;
            d2d->GetTransform(&t);
            d2d->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(dst.left, dst.top + 1.2f * s) * t);
            d2d->DrawImage(shadow_.get(), D2D1_INTERPOLATION_MODE_LINEAR);
            d2d->SetTransform(t);
            d2d->DrawBitmap(bmp, dst, m.appear, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {
            // Placeholder while the icon loads: a quiet rounded square.
            D2D1_COLOR_F c = wash;
            c.a *= 1.4f * m.appear;
            brush->SetColor(c);
            d2d->FillRoundedRectangle(D2D1::RoundedRect(it.icon, size * 0.22f, size * 0.22f), brush.get());
        }
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());

        // Running indicator: a dot; a short pill for the active app; amber when
        // the app asks for attention.
        if (it.running) {
            float w = it.active ? 14 * s : 4.5f * s, h = 4 * s;
            float y = std::min(it.icon.bottom + 5 * s, scene.bar.bottom - 3.5f * s) - h / 2;
            D2D1_COLOR_F c = it.attention ? D2D1::ColorF(1.0f, 0.70f, 0.18f, 0.98f) : ink;
            if (!it.active && !it.attention)
                c.a *= 0.65f;
            brush->SetColor(c);
            d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - w / 2, y, cx + w / 2, y + h), h / 2, h / 2), brush.get());
        }
    }
    // Drop bitmaps of icons that are gone or were replaced.
    for (auto it = bitmaps_.begin(); it != bitmaps_.end();)
        it = used.count(it->first) ? std::next(it) : bitmaps_.erase(it);

    if (scene.separatorX >= 0) {
        D2D1_COLOR_F c = inkFor(darkText);
        c.a = darkText ? 0.18f : 0.30f;
        brush->SetColor(c);
        float h = (scene.bar.bottom - scene.bar.top) * 0.46f, mid = (scene.bar.top + scene.bar.bottom) / 2;
        d2d->FillRectangle(D2D1::RectF(scene.separatorX - 0.5f * s, mid - h / 2, scene.separatorX + 0.5f * s, mid + h / 2), brush.get());
    }

    if (scene.clock.right > scene.clock.left) {
        const Motion& m = motion_[L"#clock"];
        ink = inkFor(darkText);
        if (m.hover > 0.001f) {
            D2D1_COLOR_F c = washFor(darkText);
            c.a *= EaseOut(m.hover);
            brush->SetColor(c);
            float r = 10 * s;
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
    }
}

} // namespace atrium
