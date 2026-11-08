// Analytic G3 corner family. See docs/DESIGN.md §2 for the endpoint proof.
// F(x,y) = f(x) + f(y) - 1; f(t) = t^4 / (a + (1-a)t^2).
// a is positive whenever straight joins exist: departure there is O(t^4).
#pragma once

#include <algorithm>
#include <cmath>

namespace atrium {
struct CornerProfile {
    float reach = 0, a = 0.02f;
};

inline CornerProfile CornerFor(float width, float height, float radius)
{
    const float half = std::max(0.f, std::min(width, height) * 0.5f);
    radius = std::clamp(radius, 0.f, half);
    const float reach = std::min(radius * 1.6f, half);
    if (reach <= 0) return {};
    // Figma's iOS reference extends the transition by 60%. Keep the same
    // 45-degree inset as R, tapering the extension when the sides run out.
    const float v = 1 - 0.2928932188134525f * radius / reach;
    const float v2 = v * v;
    if (width == height && radius == half) return {reach, 0}; // a whole circle has no straight joins
    return {reach, std::max(0.02f, (2 * v2 * v2 - v2) / (1 - v2))};
}

inline float CornerF(float t, float a)
{
    const float t2 = t * t;
    return t2 * t2 / (a + (1 - a) * t2);
}
inline float CornerDF(float t, float a)
{
    const float t2 = t * t, den = a + (1 - a) * t2;
    return 2 * t * t2 * (2 * a + (1 - a) * t2) / (den * den);
}

// First-order pixel distance to the EXACT implicit contour, for antialiasing.
// No fitted path, circle fallback, or iterated radial solution defines the edge.
inline float CornerEdge(float qx, float qy, CornerProfile p)
{
    if (p.reach <= 0 || std::min(qx, qy) <= 0) return std::max(qx, qy) - p.reach;
    const float x = qx / p.reach, y = qy / p.reach;
    const float f = CornerF(x, p.a) + CornerF(y, p.a) - 1;
    return f * p.reach / std::max(std::hypot(CornerDF(x, p.a), CornerDF(y, p.a)), 1e-12f);
}

inline float CornerCoverage(float x, float y, float width, float height, CornerProfile p)
{
    const float qx = std::abs(x - width * 0.5f) - width * 0.5f + p.reach;
    const float qy = std::abs(y - height * 0.5f) - height * 0.5f + p.reach;
    return std::clamp(0.5f - CornerEdge(qx, qy, p), 0.f, 1.f);
}
} // namespace atrium
