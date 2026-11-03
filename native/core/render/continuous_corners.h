// Round G3 corners: f(x) + f(y) = 1, f(t) = t^4 / (0.02 + 0.98 t^2).
// Sixteen fitted cubics per quarter: radial error < 0.000010 * R.
// Capsules keep circular ends. Cache local paths, translate on movement.
#pragma once

#include <d2d1_1.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace atrium {

inline D2D1_POINT_2F ContinuousCornerPoint(double theta)
{
    const double x = std::sin(theta), y = -std::cos(theta);
    double n = 1;
    for (int i = 0; i < 6; ++i) {
        const double vx = x / n, vy = y / n, xx = vx * vx, yy = vy * vy;
        const double dx = 0.02 + 0.98 * xx, dy = 0.02 + 0.98 * yy;
        const double fx = 2 * vx * xx * (0.04 + 0.98 * xx) / (dx * dx);
        const double fy = 2 * vy * yy * (0.04 + 0.98 * yy) / (dy * dy);
        n *= 1 + (xx * xx / dx + yy * yy / dy - 1) / (vx * fx + vy * fy);
    }
    return D2D1::Point2F(float(x / n), float(y / n));
}

inline ID2D1PathGeometry* ContinuousGeometry(ID2D1RenderTarget* target, float width, float height, float radius)
{
    struct Shape {
        float width = 0, height = 0, radius = 0;
        winrt::com_ptr<ID2D1PathGeometry> path;
    };
    thread_local winrt::com_ptr<ID2D1Factory> made;
    thread_local std::array<Shape, 64> cache;
    thread_local size_t next = 0;
    winrt::com_ptr<ID2D1Factory> factory;
    target->GetFactory(factory.put());
    if (factory != made) {
        cache = {};
        next = 0;
        made = factory;
    }
    radius = std::clamp(radius, 0.f, std::max(0.f, std::min(width, height) / 2));
    for (const auto& entry : cache)
        if (entry.path && entry.width == width && entry.height == height && entry.radius == radius)
            return entry.path.get();

    constexpr float curve[16][6] = {
        {0.032498308f, -1.000000000f, 0.065710536f, -1.000178492f, 0.098338114f, -0.998443629f},
        {0.131553444f, -0.996677515f, 0.163540056f, -0.993057048f, 0.196386850f, -0.987303366f},
        {0.228803327f, -0.981625061f, 0.261034933f, -0.974022988f, 0.292662745f, -0.964779776f},
        {0.324313483f, -0.955529865f, 0.355545339f, -0.944582888f, 0.386091437f, -0.932107184f},
        {0.416654437f, -0.919624577f, 0.446598774f, -0.905584742f, 0.475764260f, -0.890092326f},
        {0.504935401f, -0.874596906f, 0.533328642f, -0.857648022f, 0.560822831f, -0.839330681f},
        {0.588318859f, -0.821012115f, 0.614896140f, -0.801338079f, 0.640450527f, -0.780391226f},
        {0.666005469f, -0.759443918f, 0.690512781f, -0.737244022f, 0.713878402f, -0.713878402f},
        {0.737244022f, -0.690512781f, 0.759443918f, -0.666005469f, 0.780391226f, -0.640450527f},
        {0.801338079f, -0.614896140f, 0.821012115f, -0.588318859f, 0.839330681f, -0.560822831f},
        {0.857648022f, -0.533328642f, 0.874596906f, -0.504935401f, 0.890092326f, -0.475764260f},
        {0.905584742f, -0.446598774f, 0.919624577f, -0.416654437f, 0.932107184f, -0.386091437f},
        {0.944582888f, -0.355545339f, 0.955529865f, -0.324313483f, 0.964779776f, -0.292662745f},
        {0.974022988f, -0.261034933f, 0.981625061f, -0.228803327f, 0.987303366f, -0.196386850f},
        {0.993057048f, -0.163540056f, 0.996677515f, -0.131553444f, 0.998443629f, -0.098338114f},
        {1.000178492f, -0.065710536f, 1.000000000f, -0.032498308f, 1.000000000f, 0.000000000f}
    };
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put())))
        return nullptr;
    sink->BeginFigure(D2D1::Point2F(radius, 0), D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F centres[] = {{width - radius, radius}, {width - radius, height - radius}, {radius, height - radius}, {radius, radius}};
    const D2D1_POINT_2F starts[] = {{width - radius, 0}, {width, height - radius}, {radius, height}, {0, radius}};
    for (int corner = 0; corner < 4; ++corner) {
        sink->AddLine(starts[corner]);
        auto point = [&](float x, float y) {
            float rx = corner == 0 ? x : corner == 1 ? -y : corner == 2 ? -x : y;
            float ry = corner == 0 ? y : corner == 1 ? x : corner == 2 ? -y : -x;
            return D2D1::Point2F(centres[corner].x + radius * rx, centres[corner].y + radius * ry);
        };
        if (radius >= std::min(width, height) / 2)
            sink->AddArc(D2D1::ArcSegment(point(1, 0), D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        else
            for (const auto& segment : curve)
                sink->AddBezier(D2D1::BezierSegment(point(segment[0], segment[1]), point(segment[2], segment[3]), point(segment[4], segment[5])));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return nullptr;
    auto& entry = cache[next++ % cache.size()];
    entry = {width, height, radius, std::move(path)};
    return entry.path.get();
}

inline void FillContinuousRect(ID2D1RenderTarget* target, D2D1_RECT_F rect, float radius, ID2D1Brush* brush)
{
    if (auto* path = ContinuousGeometry(target, rect.right - rect.left, rect.bottom - rect.top, radius)) {
        D2D1_MATRIX_3X2_F saved;
        target->GetTransform(&saved);
        target->SetTransform(D2D1::Matrix3x2F::Translation(rect.left, rect.top) * saved);
        D2D1_MATRIX_3X2_F brushSaved;
        brush->GetTransform(&brushSaved);
        brush->SetTransform(brushSaved * D2D1::Matrix3x2F::Translation(-rect.left, -rect.top));
        target->FillGeometry(path, brush);
        brush->SetTransform(brushSaved);
        target->SetTransform(saved);
    }
}

inline void StrokeContinuousRect(ID2D1RenderTarget* target, D2D1_RECT_F rect, float radius, ID2D1Brush* brush, float width)
{
    if (auto* path = ContinuousGeometry(target, rect.right - rect.left, rect.bottom - rect.top, radius)) {
        D2D1_MATRIX_3X2_F saved;
        target->GetTransform(&saved);
        target->SetTransform(D2D1::Matrix3x2F::Translation(rect.left, rect.top) * saved);
        target->DrawGeometry(path, brush, width);
        target->SetTransform(saved);
    }
}

// The input region follows the same silhouette as the glass, including its fuller corners.
inline HRGN ContinuousRegion(RECT rect, int radius)
{
    radius = std::clamp(radius, 0, int(std::min(rect.right - rect.left, rect.bottom - rect.top) / 2));
    if (radius >= std::min(rect.right - rect.left, rect.bottom - rect.top) / 2)
        return CreateRoundRectRgn(rect.left, rect.top, rect.right, rect.bottom, 2 * radius, 2 * radius);
    std::vector<POINT> points;
    points.reserve(260);
    const POINT centres[] = {{rect.right - radius, rect.top + radius}, {rect.right - radius, rect.bottom - radius},
                             {rect.left + radius, rect.bottom - radius}, {rect.left + radius, rect.top + radius}};
    for (int corner = 0; corner < 4; ++corner)
        for (int i = 0; i <= 64; ++i) {
            const double theta = double(i) * 1.5707963267948966 / 64;
            const auto p = ContinuousCornerPoint(theta);
            const double x = p.x, y = p.y;
            const double rx = corner == 0 ? x : corner == 1 ? -y : corner == 2 ? -x : y;
            const double ry = corner == 0 ? y : corner == 1 ? x : corner == 2 ? -y : -x;
            points.push_back({centres[corner].x + LONG(std::lround(radius * rx)), centres[corner].y + LONG(std::lround(radius * ry))});
        }
    return CreatePolygonRgn(points.data(), int(points.size()), WINDING);
}

} // namespace atrium
