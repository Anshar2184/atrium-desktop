// Rasterize the analytic G3 boundary, never a fitted cubic path.
#pragma once

#include "corner_profile.h"
#include <d2d1_1.h>
#include <winrt/base.h>
#include <array>
#include <vector>

namespace atrium {
struct CornerMask {
    CornerProfile outer, inner;
    float stroke = 0;
    size_t bytes = 0;
    winrt::com_ptr<IUnknown> domain;
    winrt::com_ptr<ID2D1Bitmap> bitmap;
};

struct CornerMaskCache {
    std::vector<CornerMask> entries;
    size_t bytes = 0;
};
inline CornerMaskCache& ContinuousMasks()
{
    thread_local CornerMaskCache cache;
    return cache;
}
inline winrt::com_ptr<IUnknown> CornerResourceDomain(ID2D1RenderTarget* target)
{
    winrt::com_ptr<IUnknown> domain;
    winrt::com_ptr<ID2D1DeviceContext> context;
    if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(context.put())))) {
        winrt::com_ptr<ID2D1Device> device;
        context->GetDevice(device.put());
        domain = device.as<IUnknown>();
    } else winrt::check_hresult(target->QueryInterface(IID_PPV_ARGS(domain.put())));
    return domain;
}
inline void ForgetContinuousMaskDomain(IUnknown* domain)
{
    if (!domain) return;
    auto& cache = ContinuousMasks();
    std::erase_if(cache.entries, [&](const auto& entry) {
        if (entry.domain.get() != domain) return false;
        cache.bytes -= entry.bytes;
        return true;
    });
}
inline void ForgetContinuousMasks(ID2D1RenderTarget* target)
{
    if (target) ForgetContinuousMaskDomain(CornerResourceDomain(target).get());
}
inline ID2D1Bitmap* ContinuousMask(ID2D1RenderTarget* target, float width, float height, float radius, float stroke = 0)
{
    auto& storage = ContinuousMasks();
    auto& cache = storage.entries;
    auto& bytes = storage.bytes;
    if (width <= 0 || height <= 0) return nullptr;
    stroke = std::clamp(stroke, 0.f, std::min(width, height) * 0.5f);
    auto domain = CornerResourceDomain(target);
    const auto outer = CornerFor(width, height, radius);
    const auto inner = CornerFor(std::max(0.f, width - 2 * stroke), std::max(0.f, height - 2 * stroke), std::max(0.f, radius - stroke));
    for (auto& entry : cache)
        if (entry.domain == domain && entry.outer.reach == outer.reach && entry.outer.a == outer.a &&
            entry.inner.reach == inner.reach && entry.inner.a == inner.a && entry.stroke == stroke)
            return entry.bitmap.get();
    // A corner stamp has a small stretchable centre. Animated lengths reuse it
    // without allocating or rasterizing a full-size bitmap every frame.
    const UINT32 w = UINT32(2 * (std::ceil(std::max(outer.reach, stroke)) + 1) + 1), h = w;
    std::vector<BYTE> pixels(size_t(w) * h);
    for (UINT32 y = 0; y < h; ++y)
        for (UINT32 x = 0; x < w; ++x) {
            float alpha = CornerCoverage(float(x) + 0.5f, float(y) + 0.5f, float(w), float(h), outer);
            if (stroke > 0 && width > 2 * stroke && height > 2 * stroke)
                alpha = std::max(0.f, alpha - CornerCoverage(float(x) + 0.5f - stroke, float(y) + 0.5f - stroke,
                    float(w) - 2 * stroke, float(h) - 2 * stroke, inner));
            pixels[size_t(y) * w + x] = BYTE(std::lround(alpha * 255));
        }
    winrt::com_ptr<ID2D1Bitmap> bitmap;
    auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(target->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w, &properties, bitmap.put()))) return nullptr;
    constexpr size_t budget = 4 * 1024 * 1024;
    while (!cache.empty() && (bytes + pixels.size() > budget || cache.size() >= 64)) {
        bytes -= cache.front().bytes;
        cache.erase(cache.begin());
    }
    bytes += pixels.size();
    cache.push_back({outer, inner, stroke, pixels.size(), std::move(domain), std::move(bitmap)});
    return cache.back().bitmap.get();
}

inline void PaintContinuousRect(ID2D1RenderTarget* target, D2D1_RECT_F rect, float radius, ID2D1Brush* brush, float stroke = 0)
{
    const float width = rect.right - rect.left, height = rect.bottom - rect.top;
    stroke = std::clamp(stroke, 0.f, std::max(0.f, std::min(width, height) * 0.5f));
    if (auto* mask = ContinuousMask(target, width, height, radius, stroke)) {
        const auto antialias = target->GetAntialiasMode();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        const auto size = mask->GetSize();
        const float cap = std::max(CornerFor(width, height, radius).reach, stroke);
        const float x[] = {rect.left, rect.left + cap, rect.right - cap, rect.right};
        const float y[] = {rect.top, rect.top + cap, rect.bottom - cap, rect.bottom};
        const float sx[] = {0, cap, size.width - cap, size.width};
        const float sy[] = {0, cap, size.height - cap, size.height};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                if (x[col + 1] > x[col] && y[row + 1] > y[row]) {
                    const auto destination = D2D1::RectF(x[col], y[row], x[col + 1], y[row + 1]);
                    const auto source = D2D1::RectF(sx[col], sy[row], sx[col + 1], sy[row + 1]);
                    target->FillOpacityMask(mask, brush, D2D1_OPACITY_MASK_CONTENT_GRAPHICS, destination, source);
                }
        target->SetAntialiasMode(antialias);
    }
}
inline void FillContinuousRect(ID2D1RenderTarget* target, D2D1_RECT_F rect, float radius, ID2D1Brush* brush)
{
    PaintContinuousRect(target, rect, radius, brush);
}
inline void StrokeContinuousRect(ID2D1RenderTarget* target, D2D1_RECT_F rect, float radius, ID2D1Brush* brush, float width)
{
    const float half = width * 0.5f;
    PaintContinuousRect(target, D2D1::RectF(rect.left - half, rect.top - half, rect.right + half, rect.bottom + half), radius + half, brush, width);
}

inline D2D1_POINT_2F ContinuousCornerPoint(double theta, double a)
{
    const double x = std::sin(theta), y = -std::cos(theta);
    if (a == 0) return D2D1::Point2F(float(x), float(y));
    double n = 1;
    for (int i = 0; i < 8; ++i) {
        const double vx = x / n, vy = y / n, xx = vx * vx, yy = vy * vy;
        const double dx = a + (1 - a) * xx, dy = a + (1 - a) * yy;
        const double fx = 2 * vx * xx * (2 * a + (1 - a) * xx) / (dx * dx);
        const double fy = 2 * vy * yy * (2 * a + (1 - a) * yy) / (dy * dy);
        n *= 1 + (xx * xx / dx + yy * yy / dy - 1) / (vx * fx + vy * fy);
    }
    return D2D1::Point2F(float(x / n), float(y / n));
}

// HRGN is an integer-pixel input region sampled from the same analytic boundary.
inline HRGN ContinuousRegion(RECT rect, int radius)
{
    const auto profile = CornerFor(float(rect.right - rect.left), float(rect.bottom - rect.top), float(radius));
    const double r = profile.reach;
    std::vector<POINT> points;
    points.reserve(260);
    const D2D1_POINT_2F centres[] = {{float(rect.right - r), float(rect.top + r)}, {float(rect.right - r), float(rect.bottom - r)},
                                   {float(rect.left + r), float(rect.bottom - r)}, {float(rect.left + r), float(rect.top + r)}};
    for (int corner = 0; corner < 4; ++corner)
        for (int i = 0; i <= 64; ++i) {
            const auto p = ContinuousCornerPoint(double(i) * 1.5707963267948966 / 64, profile.a);
            const double x = p.x, y = p.y;
            const double rx = corner == 0 ? x : corner == 1 ? -y : corner == 2 ? -x : y;
            const double ry = corner == 0 ? y : corner == 1 ? x : corner == 2 ? -y : -x;
            points.push_back({LONG(std::lround(centres[corner].x + r * rx)), LONG(std::lround(centres[corner].y + r * ry))});
        }
    return CreatePolygonRgn(points.data(), int(points.size()), WINDING);
}
} // namespace atrium
