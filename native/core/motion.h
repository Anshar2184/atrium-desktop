// Atrium's motion vocabulary (docs/DESIGN.md, Motion): one ease-out curve for
// fades and short slides, an exponential approach for states that follow the
// pointer (hover, press), and springs for whatever moves into place. Springs
// are given the way Apple gives them: `response`, the period in seconds of the
// undamped oscillation, and `damping`, 1 for none past the target, less for a
// small overshoot. Used by the UI thread and the render thread alike.
#pragma once

#include <algorithm>
#include <cmath>

namespace atrium::motion {

// Cubic ease-out over t in 0..1: fast start, soft landing.
inline float EaseOut(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    const float u = 1 - t;
    return 1 - u * u * u;
}

// Ease-out that passes the end by a little before settling (`overshoot` 1.70158: 10 %).
inline float EaseOutBack(float t, float overshoot)
{
    t = std::clamp(t, 0.f, 1.f) - 1;
    return 1 + (overshoot + 1) * t * t * t + overshoot * t * t;
}

// Moves v towards target by the share `rate` per second closes, whatever the frame time.
inline float Approach(float v, float target, double dtMs, float rate)
{
    return v + (target - v) * (1 - std::exp(-float(dtMs) / 1000.f * rate));
}

// Hover and press washes: in quickly, out a little slower, as on macOS (per second).
constexpr float kHoverIn = 20, kHoverOut = 11;

// A 0..1 state that follows a flag from frame to frame (a hover wash);
// `moving` until it has landed.
struct Follow {
    float value = 0;
    double lastMs = 0;
    bool moving = false;
    float Step(bool on, double nowMs)
    {
        const double dt = lastMs > 0 ? std::clamp(nowMs - lastMs, 0.0, 100.0) : 0;
        lastMs = nowMs;
        const float target = on ? 1.f : 0.f;
        value = Approach(value, target, dt, on ? kHoverIn : kHoverOut);
        moving = std::fabs(target - value) >= 0.01f;
        if (!moving)
            value = target;
        return value;
    }
};

struct Spring {
    float response = 0.4f; // s
    float damping = 1.f;   // damping ratio

    // Advances position x and velocity v towards target over dt seconds, in
    // closed form: a late frame lands exactly where a punctual one would.
    void Step(float& x, float& v, float target, float dt) const
    {
        const float w0 = 6.2831853f / std::max(response, 0.01f), z = std::max(damping, 0.f);
        const float y0 = x - target, v0 = v;
        float y, vy;
        if (std::fabs(z - 1) < 1e-3f) {
            const float e = std::exp(-w0 * dt), b = v0 + w0 * y0;
            y = e * (y0 + b * dt);
            vy = e * (v0 - w0 * dt * b);
        } else if (z < 1) {
            const float a = z * w0, wd = w0 * std::sqrt(1 - z * z), e = std::exp(-a * dt), c = std::cos(wd * dt), s = std::sin(wd * dt);
            y = e * (y0 * c + (v0 + a * y0) / wd * s);
            vy = e * (v0 * c - (a * v0 + w0 * w0 * y0) / wd * s);
        } else {
            const float q = w0 * std::sqrt(z * z - 1), r1 = -z * w0 + q, r2 = -z * w0 - q;
            const float c2 = (v0 - r1 * y0) / (r2 - r1), c1 = y0 - c2, e1 = std::exp(r1 * dt), e2 = std::exp(r2 * dt);
            y = c1 * e1 + c2 * e2;
            vy = c1 * r1 * e1 + c2 * r2 * e2;
        }
        x = target + y;
        v = vy;
    }

    // Close enough to rest at target that a frame would not show the difference.
    static bool AtRest(float x, float v, float target, float eps = 0.002f) { return std::fabs(x - target) < eps && std::fabs(v) < eps * 5; }
};

// The springs everything uses (DESIGN.md, Motion).
constexpr Spring kPop{0.36f, 0.62f};    // popups opening: quick, a hint of overshoot
constexpr Spring kSmooth{0.48f, 0.77f}; // levels and indicators (the status ring)

} // namespace atrium::motion
