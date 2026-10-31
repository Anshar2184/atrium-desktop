using System.Numerics;

namespace Atrium.Settings;

// The three procedural wallpapers behind the specimen. Everything is a pure function of the
// position in DIP of the artwork, so the loupe can repaint a magnified crop at any resolution.
internal static class Specimens
{
    public static void Paint(Specimen kind, float[] rgba, int pw, int ph, float artW, float artH, float x0, float y0, float ppd)
    {
        float aa = 1f / ppd; // one pixel in DIP
        Parallel.For(0, ph, j =>
        {
            float y = y0 + (j + 0.5f) / ppd;
            for (int i = 0; i < pw; i++)
            {
                float x = x0 + (i + 0.5f) / ppd;
                Vector3 c = kind switch
                {
                    Specimen.Sunset => Sunset(x, y, artW, artH, aa),
                    Specimen.Night => Night(x, y, artW, artH, aa),
                    _ => Stripes(x, y, artW, artH, aa),
                };
                int o = (j * pw + i) * 4;
                rgba[o] = c.X; rgba[o + 1] = c.Y; rgba[o + 2] = c.Z; rgba[o + 3] = 1;
            }
        });
    }

    private static float Sat(float v) => v < 0 ? 0 : v > 1 ? 1 : v;
    private static float Smooth(float a, float b, float x) { float t = Sat((x - a) / (b - a)); return t * t * (3 - 2 * t); }
    private static Vector3 Mix(Vector3 a, Vector3 b, float t) => a + (b - a) * t;
    private static Vector3 Rgb(int hex) => new(((hex >> 16) & 255) / 255f, ((hex >> 8) & 255) / 255f, (hex & 255) / 255f);

    private static Vector3 Ramp(float v, (float At, Vector3 C)[] stops)
    {
        if (v <= stops[0].At) return stops[0].C;
        for (int i = 1; i < stops.Length; i++)
            if (v <= stops[i].At)
                return Mix(stops[i - 1].C, stops[i].C, Smooth(0, 1, (v - stops[i - 1].At) / (stops[i].At - stops[i - 1].At)) );
        return stops[^1].C;
    }

    private static float Hash(int x, int y)
    {
        uint h = unchecked((uint)(x * 374761393 + y * 668265263));
        h = unchecked((h ^ (h >> 13)) * 1274126177u);
        h ^= h >> 16;
        return (h & 0xFFFFFF) / 16777216f;
    }

    private static float Blob(float x, float y, float cx, float cy, float rx, float ry)
    {
        float dx = (x - cx) / rx, dy = (y - cy) / ry;
        return MathF.Exp(-(dx * dx + dy * dy));
    }

    // ---- Sunset: a low sun over layered dunes ----

    private static readonly (float, Vector3)[] SunsetSky =
    {
        (0.00f, Rgb(0x1B1442)), (0.22f, Rgb(0x4A1D6B)), (0.40f, Rgb(0xA8366F)),
        (0.54f, Rgb(0xF26B5B)), (0.64f, Rgb(0xFFA463)), (0.74f, Rgb(0xFFD08A)),
    };

    private static Vector3 Sunset(float x, float y, float W, float H, float aa)
    {
        float u = x / W, v = y / H;
        var c = Ramp(v, SunsetSky);

        // Soft clouds catching the light.
        c = Mix(c, Rgb(0xFFB3A0), 0.40f * Blob(x, y, 0.22f * W, 0.34f * H, 0.20f * W, 0.045f * H));
        c = Mix(c, Rgb(0xFFC9A8), 0.32f * Blob(x, y, 0.40f * W, 0.43f * H, 0.16f * W, 0.030f * H));
        c = Mix(c, Rgb(0xFF9FB0), 0.34f * Blob(x, y, 0.86f * W, 0.27f * H, 0.17f * W, 0.040f * H));

        // Sun: disc plus a warm halo.
        float sx = 0.68f * W, sy = 0.60f * H;
        float dx = x - sx, dy = y - sy;
        float dist = MathF.Sqrt(dx * dx + dy * dy);
        float R = 0.13f * H;
        c += Rgb(0xFF8A4A) * (0.50f * MathF.Exp(-dist * dist / (2 * R * R * 9f)));
        c += Rgb(0xFFC27A) * (0.45f * MathF.Exp(-dist * dist / (2 * R * R * 2.2f)));
        float disc = 1 - Smooth(R - aa, R + aa, dist);
        c = Mix(c, Rgb(0xFFF4D6), disc);

        // Dunes, back to front.
        float[] bases = { 0.690f, 0.790f, 0.905f };
        Vector3[] top = { Rgb(0xE5675B), Rgb(0xA83A63), Rgb(0x5E2358) };
        Vector3[] bottom = { Rgb(0x92315F), Rgb(0x5A2160), Rgb(0x2A1240) };
        float[] amp1 = { 0.034f, 0.046f, 0.032f }, f1 = { 5.1f, 3.3f, 4.4f }, p1 = { 0.6f, 2.0f, 4.1f };
        float[] amp2 = { 0.012f, 0.014f, 0.011f }, f2 = { 13.0f, 9.1f, 11.0f }, p2 = { 1.7f, 0.4f, 2.3f };
        for (int k = 0; k < 3; k++)
        {
            float ridge = (bases[k] + amp1[k] * MathF.Sin(u * f1[k] + p1[k]) + amp2[k] * MathF.Sin(u * f2[k] + p2[k])) * H;
            float m = Smooth(ridge - aa, ridge + aa, y);
            float depth = Sat((y - ridge) / (0.20f * H));
            var layer = Mix(top[k], bottom[k], depth);
            layer += Rgb(0xFFB070) * (0.30f * MathF.Exp(-(y - ridge) / (0.012f * H)) * (k == 0 ? 1f : 0.55f));
            c = Mix(c, layer, m);
        }

        float vx = (u - 0.5f), vy = (v - 0.5f) * 0.8f;
        c *= 1 - 0.28f * Sat(MathF.Sqrt(vx * vx + vy * vy) * 1.5f - 0.2f);
        return c;
    }

    // ---- Night: aurora, stars and a lit skyline ----

    private static readonly (float, Vector3)[] NightSky =
    {
        (0.00f, Rgb(0x050718)), (0.35f, Rgb(0x0E1040)), (0.70f, Rgb(0x2A1A63)), (1.00f, Rgb(0x4B2478)),
    };

    private static Vector3 Night(float x, float y, float W, float H, float aa)
    {
        float u = x / W, v = y / H;
        var c = Ramp(v, NightSky);

        c += Rgb(0x16C7B8) * (0.55f * Blob(x, y, 0.20f * W, 0.36f * H, 0.28f * W, 0.20f * H));
        c += Rgb(0x6A5BFF) * (0.50f * Blob(x, y, 0.50f * W, 0.22f * H, 0.30f * W, 0.16f * H));
        c += Rgb(0xE23FA6) * (0.55f * Blob(x, y, 0.82f * W, 0.40f * H, 0.24f * W, 0.22f * H));

        // Moon.
        float mx = 0.91f * W - x, my = 0.17f * H - y;
        float md = MathF.Sqrt(mx * mx + my * my), mr = 0.055f * H;
        c += Rgb(0x9DB4FF) * (0.35f * MathF.Exp(-md * md / (2 * mr * mr * 10f)));
        c = Mix(c, Rgb(0xF4F6FF), 1 - Smooth(mr - aa, mr + aa, md));

        // Stars: at most one per 26 DIP cell, kept clear of the cell border.
        const float cell = 26f;
        int ci = (int)MathF.Floor(x / cell), cj = (int)MathF.Floor(y / cell);
        if (Hash(ci, cj) < 0.42f && v < 0.78f)
        {
            float sx = (ci + 0.25f + 0.5f * Hash(ci + 7, cj)) * cell, sy = (cj + 0.25f + 0.5f * Hash(ci, cj + 11)) * cell;
            float sd2 = (x - sx) * (x - sx) + (y - sy) * (y - sy);
            float size = 0.55f + 1.3f * Hash(ci + 3, cj + 5);
            c += new Vector3(0.9f, 0.93f, 1f) * (MathF.Exp(-sd2 / (2 * size * size)) * (0.55f + 0.45f * Hash(ci + 1, cj + 2)));
        }

        // Skyline with lit windows.
        const float colW = 22f;
        int bi = (int)MathF.Floor(x / colW);
        float bh = 0.64f + 0.22f * Hash(bi, 3) * Hash(bi, 9) + 0.06f * Hash(bi, 5);
        float top = bh * H;
        float lx = x - bi * colW;
        float building = Smooth(top - aa, top + aa, y) * (1 - Smooth(colW - 1.5f - aa, colW - 1.5f + aa, lx));
        if (building > 0)
        {
            var wall = Mix(Rgb(0x0A0B22), Rgb(0x15123A), Sat((y - top) / (0.3f * H)));
            float wx = (lx - 4f) / 7f, wy = (y - top - 6f) / 9f;
            if (wx >= 0 && wy >= 0 && lx < colW - 5.5f)
            {
                int wi = (int)MathF.Floor(wx), wj = (int)MathF.Floor(wy);
                float fx = wx - wi, fy = wy - wj;
                if (fx < 0.5f && fy < 0.55f && Hash(bi * 13 + wi, wj + 31) > 0.5f)
                    wall = Mix(wall, Rgb(0xFFD27A) * (0.65f + 0.35f * Hash(wi, wj + bi)), 0.95f);
            }
            c = Mix(c, wall, building);
        }
        float vx = (u - 0.5f), vy = (v - 0.5f) * 0.8f;
        c *= 1 - 0.30f * Sat(MathF.Sqrt(vx * vx + vy * vy) * 1.5f - 0.25f);
        return c;
    }

    // ---- Stripes: hard edges that make refraction and dispersion obvious ----

    private static readonly float[] BarU = { 0.10f, 0.27f, 0.47f, 0.66f, 0.84f };
    private static readonly float[] BarWd = { 0.050f, 0.030f, 0.070f, 0.040f, 0.055f };
    private static readonly Vector3[] BarC = { Rgb(0xFF3B30), Rgb(0xFFCC00), Rgb(0x0AB4FF), Rgb(0xE040FB), Rgb(0x30D158) };

    private static Vector3 Stripes(float x, float y, float W, float H, float aa)
    {
        const float period = 20f;
        float s = (x + y) / period;
        float f = s - MathF.Floor(s);
        float tri = MathF.Abs(f - 0.5f) * 2f;            // 0..1..0
        float w = aa * 1.4f / period;
        float stripe = Smooth(0.5f - w, 0.5f + w, tri);   // 1 = light
        var c = Mix(new Vector3(0.05f), new Vector3(0.96f), stripe);

        float u = x / W;
        for (int k = 0; k < BarU.Length; k++)
        {
            float half = BarWd[k] * 0.5f * W;
            float dx = MathF.Abs(x - BarU[k] * W);
            float m = 1 - Smooth(half - aa, half + aa, dx);
            if (m > 0)
            {
                // Bars keep a hint of the stripes so the refraction stays readable inside them.
                var bc = BarC[k] * (0.82f + 0.18f * stripe);
                c = Mix(c, bc, m);
            }
        }

        // One ring: curved edges show the lens bending a straight line.
        float cx = 0.37f * W, cy = 0.5f * H, rr = 0.30f * H;
        float rd = MathF.Abs(MathF.Sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - rr);
        c = Mix(c, new Vector3(1f, 0.28f, 0.2f), 1 - Smooth(2.2f - aa, 2.2f + aa, rd));
        return c;
    }
}
