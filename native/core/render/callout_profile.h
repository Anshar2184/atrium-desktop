// A Dock label's analytic rounded body plus its deliberate pointed tail.
// The body retains CornerProfile; the pointer is a triangle, not a rounded corner.
#pragma once
#include "corner_profile.h"

namespace atrium {
inline float CalloutTailEdge(float x, float y, float width, float bottom, float tipX, float halfWidth, float height, float overlap)
{
    const float margin = std::max(0.5f, overlap * 0.25f);
    halfWidth = std::min(halfWidth, std::max(0.5f, width * 0.5f - margin));
    const float centre = std::clamp(tipX, halfWidth + margin, std::max(halfWidth + margin, width - halfWidth - margin));
    const float ax = centre - halfWidth, ay = bottom - overlap, bx = centre + halfWidth, by = ay, cx = tipX, cy = bottom + height;
    auto edge = [&](float x0, float y0, float x1, float y1) {
        const float ex = x1 - x0, ey = y1 - y0, vx = x - x0, vy = y - y0;
        const float t = std::clamp((vx * ex + vy * ey) / std::max(ex * ex + ey * ey, 1e-12f), 0.f, 1.f);
        return std::hypot(vx - t * ex, vy - t * ey);
    };
    const float distance = std::min({edge(ax, ay, bx, by), edge(bx, by, cx, cy), edge(cx, cy, ax, ay)});
    const bool inside = (bx - ax) * (y - ay) - (by - ay) * (x - ax) >= 0 &&
                        (cx - bx) * (y - by) - (cy - by) * (x - bx) >= 0 &&
                        (ax - cx) * (y - cy) - (ay - cy) * (x - cx) >= 0;
    return inside ? -distance : distance;
}

inline float CalloutEdge(float x, float y, float width, float height, float radius, float tipX, float halfWidth, float tailHeight, float overlap)
{
    const auto corner = CornerFor(width, height, radius);
    const float body = CornerEdge(std::abs(x - width * 0.5f) - width * 0.5f + corner.reach,
                                  std::abs(y - height * 0.5f) - height * 0.5f + corner.reach, corner);
    return tailHeight > 0 ? std::min(body, CalloutTailEdge(x, y, width, height, tipX, halfWidth, tailHeight, overlap)) : body;
}
} // namespace atrium
