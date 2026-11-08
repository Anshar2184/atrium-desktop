using System.Numerics;

namespace Atrium.Settings;

// Same analytic boundary as native/core/render/corner_profile.h and glass.hlsl.
internal readonly record struct CornerProfile(float Reach, float A)
{
    internal static CornerProfile For(float width, float height, float radius)
    {
        float half = Math.Max(0, Math.Min(width, height) * 0.5f);
        radius = Math.Clamp(radius, 0, half);
        float reach = Math.Min(radius * 1.6f, half);
        if (reach <= 0) return new(0, 0.02f);
        if (width == height && radius == half) return new(reach, 0); // circle: curvature is constant everywhere
        float v = 1 - 0.2928932188134525f * radius / reach, v2 = v * v;
        return new(reach, Math.Max(0.02f, (2 * v2 * v2 - v2) / (1 - v2)));
    }

    internal float Edge(float qx, float qy)
    {
        if (Reach <= 0 || Math.Min(qx, qy) <= 0) return Math.Max(qx, qy) - Reach;
        float x = qx / Reach, y = qy / Reach, xx = x * x, yy = y * y;
        float dx = A + (1 - A) * xx, dy = A + (1 - A) * yy;
        float fx = 2 * x * xx * (2 * A + (1 - A) * xx) / (dx * dx);
        float fy = 2 * y * yy * (2 * A + (1 - A) * yy) / (dy * dy);
        return (xx * xx / dx + yy * yy / dy - 1) * Reach / Math.Max(MathF.Sqrt(fx * fx + fy * fy), 1e-12f);
    }

    internal float Coverage(float x, float y, float width, float height) => Math.Clamp(0.5f - Edge(
        MathF.Abs(x - width * 0.5f) - width * 0.5f + Reach,
        MathF.Abs(y - height * 0.5f) - height * 0.5f + Reach), 0, 1);

    internal Vector2 Point(double theta)
    {
        if (theta <= 0) return new(0, -1);
        if (theta >= Math.PI / 2) return new(1, 0);
        double x = Math.Sin(theta), y = -Math.Cos(theta), n = 1;
        for (int i = 0; i < 8; ++i)
        {
            double vx = x / n, vy = y / n, xx = vx * vx, yy = vy * vy;
            double dx = A + (1 - A) * xx, dy = A + (1 - A) * yy;
            double fx = 2 * vx * xx * (2 * A + (1 - A) * xx) / (dx * dx);
            double fy = 2 * vy * yy * (2 * A + (1 - A) * yy) / (dy * dy);
            n *= 1 + (xx * xx / dx + yy * yy / dy - 1) / (vx * fx + vy * fy);
        }
        return new((float)(x / n), (float)(y / n));
    }

}
