using System.Runtime.InteropServices;
using Vortice.Direct3D11;
using Vortice.DXGI;

namespace Atrium.Probe;

// Renders the probe glass over a synthetic background (no screen content) to
// produce shareable images for the README.
internal static class DemoProbe
{
    public static void Run(string outDir)
    {
        const int W = 1600, H = 520;
        var bg = Background(W, H);
        using var gpu = Gpu.ForMonitor(Native.MonitorFromPoint(new Native.POINT(), Native.MONITOR_DEFAULTTOPRIMARY));
        var handle = GCHandle.Alloc(bg, GCHandleType.Pinned);
        try
        {
            using var tex = gpu.Device.CreateTexture2D(new Texture2DDescription
            {
                Width = W, Height = H, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
                SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
            }, new SubresourceData(handle.AddrOfPinnedObject(), W * 4));
            int gw = 1180, gh = 196, gx = (W - gw) / 2, gy = 250;
            using var glass = new GlassRenderer(gpu, IntPtr.Zero, gw, gh);
            foreach (var (name, p) in new[] { ("liquid", GlassParams.Liquid(gw, gh, 26)), ("frosted", GlassParams.Frosted(gw, gh, 26)) })
            {
                glass.UpdateBackground(tex, gx, gy);
                var g = glass.RenderOffscreen(p);
                var comp = (byte[])bg.Clone();
                for (int j = 0; j < gh; j++)
                    for (int i = 0; i < gw; i++)
                    {
                        int s = (j * gw + i) * 4, d = ((j + gy) * W + i + gx) * 4, a = g[s + 3];
                        for (int k = 0; k < 3; k++) comp[d + k] = (byte)Math.Min(255, g[s + k] + comp[d + k] * (255 - a) / 255);
                    }
                var path = Path.Combine(outDir, $"glass-{name}.png");
                Png.Save(path, comp, W, H);
                Console.WriteLine("saved " + path);
            }
        }
        finally { handle.Free(); }
    }

    // Soft gradient, colour discs and stripes so refraction and blur are visible.
    private static byte[] Background(int w, int h)
    {
        var px = new byte[w * h * 4];
        var discs = new (float X, float Y, float R, float Cr, float Cg, float Cb)[]
        {
            (260, 330, 150, 1.00f, 0.55f, 0.35f), (640, 250, 120, 0.35f, 0.75f, 1.00f),
            (980, 380, 170, 0.95f, 0.80f, 0.30f), (1330, 260, 130, 0.55f, 0.90f, 0.55f),
        };
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float t = (float)y / h, u = (float)x / w;
                float r = 0.20f + 0.35f * u, g = 0.30f + 0.25f * (1 - t), b = 0.55f + 0.35f * t;
                foreach (var d in discs)
                {
                    float dist = MathF.Sqrt((x - d.X) * (x - d.X) + (y - d.Y) * (y - d.Y));
                    float a = Math.Clamp((d.R - dist) / 6f, 0, 1) * 0.9f;
                    r += (d.Cr - r) * a; g += (d.Cg - g) * a; b += (d.Cb - b) * a;
                }
                if (((x / 24) % 2 == 0) && y > 120 && y < 470 && (x % 24) < 3) { r *= 0.75f; g *= 0.75f; b *= 0.75f; }
                int k = (y * w + x) * 4;
                px[k] = (byte)(Math.Clamp(b, 0, 1) * 255); px[k + 1] = (byte)(Math.Clamp(g, 0, 1) * 255);
                px[k + 2] = (byte)(Math.Clamp(r, 0, 1) * 255); px[k + 3] = 255;
            }
        return px;
    }
}
