// Canvas: what a popup's paint callback draws with, in DIP, on its surface's device context.
#include "ui/surface.h"

#include "render/callout_profile.h"
#include "render/continuous_corners.h"
#include "render/round_stroke.h"
#include "render/ui_font.h"
#include "render/wifi_glyph.h"

#include <d2d1effects.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace atrium {

namespace {

// Small text over glass is a step heavier: below 12.5 DIP a regular reads as a medium (D40).
DWRITE_FONT_WEIGHT GlassWeight(float size, DWRITE_FONT_WEIGHT w) { return size <= 12.5f && w <= DWRITE_FONT_WEIGHT_NORMAL ? DWRITE_FONT_WEIGHT_MEDIUM : w; }

D2D1_RECT_F TransformRect(D2D1_RECT_F r, const D2D1_MATRIX_3X2_F& m)
{
    auto pt = [&](float x, float y) -> D2D1_POINT_2F { return {x * m._11 + y * m._21 + m._31, x * m._12 + y * m._22 + m._32}; };
    const D2D1_POINT_2F c0 = pt(r.left, r.top), c1 = pt(r.right, r.top), c2 = pt(r.left, r.bottom), c3 = pt(r.right, r.bottom);
    return D2D1::RectF(std::min({c0.x, c1.x, c2.x, c3.x}), std::min({c0.y, c1.y, c2.y, c3.y}), std::max({c0.x, c1.x, c2.x, c3.x}),
                       std::max({c0.y, c1.y, c2.y, c3.y}));
}

} // namespace

D2D1_RECT_F Canvas::Px(D2D1_RECT_F r) const { return D2D1::RectF(r.left * scale, r.top * scale, r.right * scale, r.bottom * scale); }

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F c)
{
    c.a *= alpha_; // the fade of the enclosing groups
    if (!s_.brush_)
        d2d_->CreateSolidColorBrush(c, s_.brush_.put());
    else
        s_.brush_->SetColor(c);
    return s_.brush_.get();
}

void Canvas::Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c)
{
    D2D1_COLOR_F unused;
    c = InkAt(r, c, unused);
    if (radius > 0)
        FillContinuousRect(d2d_, Px(r), radius * scale, Brush(c));
    else
        d2d_->FillRectangle(Px(r), Brush(c));
}

void Canvas::Callout(D2D1_RECT_F body, float radius, float tipX, float halfWidth, float tailHeight, float tailInset, D2D1_COLOR_F color)
{
    if (tailHeight <= 0) {
        Fill(body, radius, color);
        return;
    }
    auto rect = Px(body);
    rect = D2D1::RectF(std::round(rect.left), std::round(rect.top), std::round(rect.right), std::round(rect.bottom));
    const float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return;
    const std::array<float, 7> key{
        width, height, radius * scale, std::round(tipX * scale) - rect.left, halfWidth * scale, tailHeight * scale, tailInset * scale};
    if (!s_.calloutMask_ || key != s_.calloutMaskKey_) {
        const UINT32 w = UINT32(std::ceil(width)), h = UINT32(std::ceil(height + key[5]));
        std::vector<BYTE> pixels(size_t(w) * h);
        for (UINT32 y = 0; y < h; ++y)
            for (UINT32 x = 0; x < w; ++x)
                pixels[size_t(y) * w + x] = BYTE(std::lround(
                    255 * std::clamp(0.5f - CalloutEdge(float(x) + 0.5f, float(y) + 0.5f, key[0], key[1], key[2], key[3], key[4], key[5], key[6]), 0.f, 1.f)));
        s_.calloutMask_ = nullptr;
        const auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(d2d_->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w, properties, s_.calloutMask_.put())))
            return;
        s_.calloutMaskKey_ = key;
    }
    const auto antialias = d2d_->GetAntialiasMode();
    d2d_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    const auto size = s_.calloutMask_->GetSize();
    d2d_->FillOpacityMask(s_.calloutMask_.get(), Brush(color), D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
                          D2D1::RectF(rect.left, rect.top, rect.left + size.width, rect.top + size.height), D2D1::RectF(0, 0, size.width, size.height));
    d2d_->SetAntialiasMode(antialias);
}

void Canvas::Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width)
{
    D2D1_COLOR_F unused;
    c = InkAt(r, c, unused);
    float h = width * scale / 2;
    D2D1_RECT_F p = Px(r);
    p = D2D1::RectF(p.left + h, p.top + h, p.right - h, p.bottom - h);
    StrokeContinuousRect(d2d_, p, std::max(0.f, radius * scale - h), Brush(c), width * scale);
}

void Canvas::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1) + 1, std::max(y0, y1) + 1), c, unused);
    d2d_->DrawLine(D2D1::Point2F(x0 * scale, y0 * scale), D2D1::Point2F(x1 * scale, y1 * scale), Brush(c), width * scale);
}

void Canvas::Capsule(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1) + 1, std::max(y0, y1) + 1), c, unused);
    d2d_->DrawLine(D2D1::Point2F(x0 * scale, y0 * scale), D2D1::Point2F(x1 * scale, y1 * scale), Brush(c), width * scale, RoundStroke(d2d_));
}

void Canvas::Circle(float cx, float cy, float radius, D2D1_COLOR_F c)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(cx - radius, cy - radius, cx + radius, cy + radius), c, unused);
    d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx * scale, cy * scale), radius * scale, radius * scale), Brush(c));
}

void Canvas::Gradient(D2D1_RECT_F r, float radius, D2D1_COLOR_F a, D2D1_COLOR_F b, bool vertical)
{
    a.a *= alpha_;
    b.a *= alpha_;
    const D2D1_RECT_F p = Px(r);
    D2D1_GRADIENT_STOP stops[] = {{0, a}, {1, b}};
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1LinearGradientBrush> brush;
    if (FAILED(d2d_->CreateGradientStopCollection(stops, 2, collection.put())) ||
        FAILED(d2d_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(p.left, p.top), vertical ? D2D1::Point2F(p.left, p.bottom) : D2D1::Point2F(p.right, p.top)),
            collection.get(), brush.put())))
        return;
    if (radius > 0)
        FillContinuousRect(d2d_, p, radius * scale, brush.get());
    else
        d2d_->FillRectangle(p, brush.get());
}

void Canvas::Crescent(float cx, float cy, float radius, float dx, float dy, float cut, D2D1_COLOR_F c)
{
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1EllipseGeometry> disc, bite;
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx * scale, cy * scale), radius * scale, radius * scale), disc.put())) ||
        FAILED(f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F((cx + dx) * scale, (cy + dy) * scale), cut * scale, cut * scale), bite.put())) ||
        FAILED(f->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put())))
        return;
    disc->CombineWithGeometry(bite.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get());
    sink->Close();
    d2d_->FillGeometry(path.get(), Brush(c));
}

void Canvas::Polygon(std::span<const D2D1_POINT_2F> points, D2D1_COLOR_F c, float round)
{
    if (points.size() < 3)
        return;
    D2D1_RECT_F bounds = D2D1::RectF(points[0].x, points[0].y, points[0].x, points[0].y);
    for (const auto& p : points.subspan(1)) {
        bounds.left = std::min(bounds.left, p.x);
        bounds.top = std::min(bounds.top, p.y);
        bounds.right = std::max(bounds.right, p.x);
        bounds.bottom = std::max(bounds.bottom, p.y);
    }
    D2D1_COLOR_F unused;
    c = InkAt(bounds, c, unused);
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put())))
        return;
    sink->BeginFigure(D2D1::Point2F(points[0].x * scale, points[0].y * scale), D2D1_FIGURE_BEGIN_FILLED);
    for (const auto& p : points.subspan(1))
        sink->AddLine(D2D1::Point2F(p.x * scale, p.y * scale));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;
    ID2D1SolidColorBrush* brush = Brush(c);
    d2d_->FillGeometry(path.get(), brush);
    if (round <= 0)
        return;
    // Round corners: the outline stroked as wide as the rounding, with round joins.
    winrt::com_ptr<ID2D1StrokeStyle> joins;
    const auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    if (SUCCEEDED(f->CreateStrokeStyle(props, nullptr, 0, joins.put())))
        d2d_->DrawGeometry(path.get(), brush, 2 * round * scale, joins.get());
}

void Canvas::Arc(float cx, float cy, float radius, float a0, float a1, D2D1_COLOR_F c, float width)
{
    if (a1 - a0 < 0.5f)
        return;
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(cx - radius, cy - radius, cx + radius, cy + radius), c, unused);
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1PathGeometry> g;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(g.put())) || FAILED(g->Open(sink.put())))
        return;
    const float k = std::numbers::pi_v<float> / 180.f, r = radius * scale;
    a1 = std::min(a1, a0 + 359.9f);
    sink->BeginFigure(D2D1::Point2F(cx * scale + r * std::cos(a0 * k), cy * scale + r * std::sin(a0 * k)), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx * scale + r * std::cos(a1 * k), cy * scale + r * std::sin(a1 * k)), D2D1::SizeF(r, r), 0,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, a1 - a0 > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    d2d_->DrawGeometry(g.get(), Brush(c), width * scale, RoundStroke(d2d_));
}

IDWriteTextFormat* Canvas::Format(float px, DWRITE_FONT_WEIGHT w, bool glyph, int align, bool wrap)
{
    auto key = std::make_tuple(int(std::lround(px * 8)), int(w), glyph, align, wrap);
    auto& f = s_.formats_[key];
    if (!f) {
        if (glyph)
            s_.dwrite_->CreateTextFormat(IconFontFamily(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", f.put());
        else
            UiTextFormat(s_.dwrite_.get(), px, w, f.put());
        if (!f)
            return nullptr;
        f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!glyph) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            winrt::com_ptr<IDWriteInlineObject> ellipsis;
            s_.dwrite_->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
    }
    return f.get();
}

float Canvas::DarkAt(D2D1_RECT_F r) const
{
    if (ink.dark.empty())
        return inkDark;
    D2D1_MATRIX_3X2_F m;
    d2d_->GetTransform(&m);
    const D2D1_RECT_F p = TransformRect(Px(r), m);
    return ink.DarkAt(p.left, p.top, p.right, p.bottom, inkDark);
}

D2D1_COLOR_F Canvas::InkAt(D2D1_RECT_F r, D2D1_COLOR_F c, D2D1_COLOR_F& haloInk) const
{
    haloInk = theme.halo;
    if (ink.dark.empty())
        return c;
    // Only the theme's own ink and washes switch (accents and colours stay).
    auto same = [](const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
        return std::fabs(a.r - b.r) < 0.01f && std::fabs(a.g - b.g) < 0.01f && std::fabs(a.b - b.b) < 0.01f;
    };
    // Opaque white (a slider's knob, text on an accent) is not ink: the
    // theme's inks are at most 0.96 opaque.
    if (c.a > 0.965f)
        return c;
    const bool sharedRgb = same(theme.text, theme.hover);
    const bool isText = same(c, theme.text) && (!sharedRgb || c.a >= 0.40f);
    const bool isWash = !isText && same(c, theme.hover);
    if (!isText && !isWash)
        return c;
    const D2D1_COLOR_F& from = isText ? theme.text : theme.hover;
    D2D1_MATRIX_3X2_F m;
    d2d_->GetTransform(&m); // groups move and scale what they draw
    const D2D1_RECT_F p = TransformRect(Px(inkArea_ ? *inkArea_ : r), m);
    float dark = ink.DarkAt(p.left, p.top, p.right, p.bottom, inkDark);
    if (std::fabs(dark - inkDark) < 0.002f)
        return c;
    Theme t = Theme::Mix(dark);
    haloInk = t.halo;
    const D2D1_COLOR_F& to = isText ? t.text : t.hover;
    return D2D1::ColorF(to.r, to.g, to.b, std::min(1.f, c.a * to.a / std::max(0.01f, from.a)));
}

void Canvas::Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w, int align, bool wrap)
{
    if (s.empty())
        return;
    D2D1_COLOR_F haloInk;
    c = InkAt(r, c, haloInk);
    if (IDWriteTextFormat* f = Format(size * scale, GlassWeight(size, w), false, align, wrap)) {
        D2D1_RECT_F p = Px(r);
        if (halo && haloInk.a > 0) { // the bar's halo: a soft copy just below the ink
            float o = std::max(1.0f, scale * 0.6f);
            D2D1_COLOR_F hc = haloInk;
            hc.a *= c.a;
            DrawUiText(d2d_, s_.dwrite_.get(), s, f, D2D1::RectF(p.left, p.top + o, p.right, p.bottom + o), Brush(hc), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        DrawUiText(d2d_, s_.dwrite_.get(), s, f, p, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Canvas::WifiFan(float cx, float cy, float radius, int lit, D2D1_COLOR_F c)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(cx - radius, cy - radius / 2, cx + radius, cy + radius / 2), c, unused);
    D2D1_COLOR_F off = c;
    off.a *= 0.28f;
    c.a *= alpha_;
    off.a *= alpha_;
    DrawWifiFan(d2d_, Brush(c), cx * scale, cy * scale, radius * scale, lit, c, off);
}

void Canvas::Glyph(const std::wstring& g, D2D1_RECT_F r, float size, D2D1_COLOR_F c)
{
    D2D1_COLOR_F haloInk;
    c = InkAt(r, c, haloInk);
    if (IDWriteTextFormat* f = Format(size * scale, DWRITE_FONT_WEIGHT_NORMAL, true, 1, false)) {
        D2D1_RECT_F p = Px(r);
        if (halo && haloInk.a > 0) {
            float o = std::max(1.0f, scale * 0.6f);
            D2D1_COLOR_F hc = haloInk;
            hc.a *= c.a;
            d2d_->DrawText(g.c_str(), UINT32(g.size()), f, D2D1::RectF(p.left, p.top + o, p.right, p.bottom + o), Brush(hc));
        }
        d2d_->DrawText(g.c_str(), UINT32(g.size()), f, p, Brush(c));
    }
}

float Canvas::TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w)
{
    IDWriteTextFormat* f = Format(size * scale, GlassWeight(size, w), false, 0, false);
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!f || FAILED(UiTextLayout(s_.dwrite_.get(), s, f, 100000, 1000, layout.put())))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace / scale;
}

ID2D1Bitmap* Canvas::Bitmap(const std::wstring& key, const IconImage& image)
{
    auto& b = s_.bitmaps_[key];
    if (!b) {
        if (image.Empty())
            return nullptr;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d_->CreateBitmap(D2D1::SizeU(UINT32(image.width), UINT32(image.height)), image.Data(), UINT32(image.width * 4), &bp, b.put())))
            return nullptr;
    }
    return b.get();
}

void Canvas::Image(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, float opacity)
{
    if (ID2D1Bitmap* b = Bitmap(key, image))
        d2d_->DrawBitmap(b, Px(r), opacity * alpha_, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Canvas::ImagePerspective(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, const D2D1_MATRIX_4X4_F& perspective, float opacity)
{
    if (ID2D1Bitmap* b = Bitmap(key, image)) {
        const D2D1_RECT_F px = Px(r);
        d2d_->DrawBitmap(b, &px, opacity * alpha_, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, &perspective);
    }
}

void Canvas::ImageQuad(const std::wstring& key, const IconImage& image, D2D1_RECT_F source, const std::array<D2D1_POINT_2F, 4>& quad, float opacity)
{
    ID2D1Bitmap* b = Bitmap(key, image);
    if (!b)
        return;
    // The projective map of the unit square onto the quad (Heckbert), its corners in px.
    std::array<D2D1_POINT_2F, 4> p{};
    for (size_t i = 0; i < 4; ++i)
        p[i] = D2D1::Point2F(quad[i].x * scale, quad[i].y * scale);
    const float dx1 = p[1].x - p[2].x, dx2 = p[3].x - p[2].x, dx3 = p[0].x - p[1].x + p[2].x - p[3].x;
    const float dy1 = p[1].y - p[2].y, dy2 = p[3].y - p[2].y, dy3 = p[0].y - p[1].y + p[2].y - p[3].y;
    const float det = dx1 * dy2 - dx2 * dy1;
    const float g = std::abs(det) < 1e-6f ? 0.f : (dx3 * dy2 - dx2 * dy3) / det, h = std::abs(det) < 1e-6f ? 0.f : (dx1 * dy3 - dx3 * dy1) / det;
    const float a = p[1].x - p[0].x + g * p[1].x, bx = p[3].x - p[0].x + h * p[3].x, d = p[1].y - p[0].y + g * p[1].y, e = p[3].y - p[0].y + h * p[3].y;
    // The source drawn at its own size at the origin, then mapped (row vectors: x' = x m11 + y m21 + m41).
    const float w = source.right - source.left, ht = source.bottom - source.top;
    if (w <= 0 || ht <= 0)
        return;
    const D2D1::Matrix4x4F m(a / w, d / w, 0, g / w, bx / ht, e / ht, 0, h / ht, 0, 0, 1, 0, p[0].x, p[0].y, 0, 1);
    const D2D1_RECT_F dest = D2D1::RectF(0, 0, w, ht);
    // A slice pours into a few pixels near the icon: bilinear alone picks single source rows and
    // the picture breaks into stripes. Multi-sample averages four, which the fade then hides.
    d2d_->DrawBitmap(b, &dest, opacity * alpha_, D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR, &source, &m);
}

bool Canvas::Cached(const std::wstring& key) const
{
    auto it = s_.bitmaps_.find(key);
    return it != s_.bitmaps_.end() && it->second;
}

D2D1_RECT_F Canvas::Caret(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT w, size_t pos)
{
    D2D1_RECT_F fallback = D2D1::RectF(r.left, r.top, r.left + 1.5f, r.top + size * 1.33f);
    IDWriteTextFormat* f = Format(size * scale, GlassWeight(size, w), false, 0, true);
    // A zero-width space after the text: a caret after a final line break sits on the new line.
    std::wstring text = s + L"\u200B";
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!f || FAILED(UiTextLayout(s_.dwrite_.get(), text, f, (r.right - r.left) * scale, 1e6f, layout.put())))
        return fallback;
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(layout->HitTestTextPosition(UINT32(std::min(pos, s.size())), FALSE, &x, &y, &m)))
        return fallback;
    return D2D1::RectF(r.left + x / scale, r.top + y / scale, r.left + x / scale + 1.5f, r.top + (y + m.height) / scale);
}

void Canvas::ImageCircle(const std::wstring& key, const IconImage& image, D2D1_RECT_F r)
{
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1EllipseGeometry> g;
    D2D1_RECT_F p = Px(r);
    if (FAILED(f->CreateEllipseGeometry(
            D2D1::Ellipse(D2D1::Point2F((p.left + p.right) / 2, (p.top + p.bottom) / 2), (p.right - p.left) / 2, (p.bottom - p.top) / 2), g.put())))
        return;
    d2d_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    Image(key, image, r);
    d2d_->PopLayer();
}

void Canvas::Ripple(D2D1_RECT_F r, float radius, float x, float y, float t, D2D1_COLOR_F c)
{
    t = std::clamp(t, 0.f, 1.f);
    const D2D1_RECT_F p = Px(r);
    float reach = std::hypot(r.right - r.left, r.bottom - r.top) * scale;
    float rr = std::max(1.f, reach * (1 - (1 - t) * (1 - t) * (1 - t)));
    float a = c.a * alpha_ * (1 - t) * (1 - t);
    D2D1_GRADIENT_STOP stops[] = {
        {0.f, D2D1::ColorF(c.r, c.g, c.b, a * 0.55f)}, {0.75f, D2D1::ColorF(c.r, c.g, c.b, a)}, {1.f, D2D1::ColorF(c.r, c.g, c.b, 0)}};
    winrt::com_ptr<ID2D1GradientStopCollection> coll;
    winrt::com_ptr<ID2D1RadialGradientBrush> brush;
    D2D1_POINT_2F centre = D2D1::Point2F(x * scale, y * scale);
    if (FAILED(d2d_->CreateGradientStopCollection(stops, 3, coll.put())) ||
        FAILED(d2d_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(), rr, rr), coll.get(), brush.put())))
        return;
    FillContinuousRect(d2d_, p, radius * scale, brush.get());
}

void Canvas::BeginBlur()
{
    // Recorded into a command list, drawn through a Gaussian blur at EndBlur.
    blurList_ = nullptr;
    blurTarget_ = nullptr;
    if (FAILED(d2d_->CreateCommandList(blurList_.put())))
        return;
    d2d_->GetTarget(blurTarget_.put());
    d2d_->SetTarget(blurList_.get());
}

void Canvas::EndBlur(float radius)
{
    if (!blurList_ || !blurTarget_)
        return;
    d2d_->SetTarget(blurTarget_.get());
    winrt::com_ptr<ID2D1CommandList> list = std::move(blurList_);
    blurTarget_ = nullptr;
    if (FAILED(list->Close()))
        return;
    D2D1_MATRIX_3X2_F transform;
    d2d_->GetTransform(&transform);
    d2d_->SetTransform(D2D1::Matrix3x2F::Identity()); // the commands carry their own transforms
    if (radius > 0.05f && (s_.blur_ || SUCCEEDED(d2d_->CreateEffect(CLSID_D2D1GaussianBlur, s_.blur_.put())))) {
        s_.blur_->SetInput(0, list.get());
        s_.blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, radius * scale);
        s_.blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        s_.blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED); // downsampled: a frame's budget
        d2d_->DrawImage(s_.blur_.get());
        s_.blur_->SetInput(0, nullptr);
    } else {
        d2d_->DrawImage(list.get());
    }
    d2d_->SetTransform(transform);
}

void Canvas::BeginFade(D2D1_POINT_2F from, D2D1_POINT_2F to, std::span<const D2D1_GRADIENT_STOP> stops, D2D1_RECT_F contentBounds)
{
    const bool bounded = std::isfinite(contentBounds.left) && std::isfinite(contentBounds.top) && std::isfinite(contentBounds.right) &&
                         std::isfinite(contentBounds.bottom) && contentBounds.right > contentBounds.left && contentBounds.bottom > contentBounds.top;
    const D2D1_RECT_F bounds = bounded ? Px(contentBounds) : D2D1::InfiniteRect();
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1LinearGradientBrush> mask;
    if (FAILED(d2d_->CreateGradientStopCollection(stops.data(), UINT32(stops.size()), collection.put())) ||
        FAILED(d2d_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(from.x * scale, from.y * scale), D2D1::Point2F(to.x * scale, to.y * scale)), collection.get(),
            mask.put()))) {
        d2d_->PushLayer(D2D1::LayerParameters1(bounds), nullptr); // EndFade pops it either way
        return;
    }
    d2d_->PushLayer(D2D1::LayerParameters1(bounds, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.f, mask.get()), nullptr);
}

void Canvas::EndFade() { d2d_->PopLayer(); }

void Canvas::Clip(D2D1_RECT_F r) { d2d_->PushAxisAlignedClip(Px(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::Unclip() { d2d_->PopAxisAlignedClip(); }

void Canvas::MaskRounded(D2D1_RECT_F r, float radius)
{
    const auto p = Px(r);
    const float rp = radius * scale;
    const auto& old = s_.roundedMaskRect_;
    if (!s_.roundedMask_ || old.left != p.left || old.top != p.top || old.right != p.right || old.bottom != p.bottom || s_.roundedMaskRadius_ != rp) {
        winrt::com_ptr<ID2D1CommandList> mask;
        if (FAILED(d2d_->CreateCommandList(mask.put())))
            return;
        winrt::com_ptr<ID2D1Image> target;
        D2D1_MATRIX_3X2_F transform;
        d2d_->GetTarget(target.put());
        d2d_->GetTransform(&transform);
        d2d_->SetTarget(mask.get());
        d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
        FillContinuousRect(d2d_, p, rp, Brush(D2D1::ColorF(1, 1, 1, 1)));
        d2d_->SetTarget(target.get());
        d2d_->SetTransform(transform);
        if (FAILED(mask->Close()))
            return;
        s_.roundedMask_ = std::move(mask);
        s_.roundedMaskRect_ = p;
        s_.roundedMaskRadius_ = rp;
    }
    d2d_->DrawImage(s_.roundedMask_.get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_DESTINATION_IN);
}

void Canvas::Continue() { s_.frameWanted_ = true; }

// Opacity multiplies into every colour drawn inside the group instead of a
// D2D layer: layers allocate intermediate surfaces, which WARP keeps cached
// (tens of MB with a fading tile per row). Overlapping shapes in a group fade
// individually, which these groups never notice.
void Canvas::BeginGroup(float opacity, float k, float cx, float cy, float dx, float dy, float turn)
{
    D2D1_MATRIX_3X2_F current;
    d2d_->GetTransform(&current);
    groups_.emplace_back(current, alpha_);
    const D2D1_POINT_2F pivot = D2D1::Point2F(cx * scale, cy * scale);
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(k, k, pivot) * D2D1::Matrix3x2F::Translation(dx * scale, dy * scale);
    if (turn != 0)
        local = D2D1::Matrix3x2F::Rotation(turn * 180.f / std::numbers::pi_v<float>, pivot) * local;
    d2d_->SetTransform(local * current);
    alpha_ *= std::clamp(opacity, 0.f, 1.f);
}

void Canvas::EndGroup()
{
    if (groups_.empty())
        return;
    d2d_->SetTransform(groups_.back().first);
    alpha_ = groups_.back().second;
    groups_.pop_back();
}

} // namespace atrium
