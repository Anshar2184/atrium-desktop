using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Vortice.Direct3D11;
using Vortice.DXGI;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P01e (option C end to end): rebuild the backdrop of a region from the shell
// desktop layer plus per-window captures, compare it with the real screen, then
// draw glass from that rebuilt backdrop WITHOUT capture exclusion and check that
// an ordinary screenshot now contains the glass.
internal static class OptionCProbe
{
    public static void Run()
    {
        var rep = new Report("p01e-option-c");
        var mon = MonitorFromPoint(new POINT { X = 0, Y = 0 }, MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        using var gpu = Gpu.ForMonitor(mon);

        // Find a region of the desktop not covered by any visible app window.
        int rw = 760, rh = 230;
        var occupied = new List<RECT>();
        var overlays = new List<string>();
        EnumWindows((h, _) =>
        {
            const int GWL_EXSTYLE = -20;
            int exs = GetWindowLong(h, GWL_EXSTYLE);
            // Full-screen click-through overlays (handwriting canvas, GPU overlays)
            // draw nothing most of the time; a real implementation must alpha-blend them.
            bool overlay = (exs & (int)WS_EX_LAYERED) != 0 && (exs & (int)(WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW)) != 0;
            if (overlay && IsWindowVisible(h)) overlays.Add(ClassOf(h));
            if (!overlay && IsWindowVisible(h) && !IsCloaked(h) && !IsIconic(h) && h != GetShellWindow() && ClassOf(h) is not "Progman" and not "WorkerW")
            {
                const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
                if (DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, out RECT r, Marshal.SizeOf<RECT>()) != 0) GetWindowRect(h, out r);
                if (r.Width > 0 && r.Height > 0) occupied.Add(r);
            }
            return true;
        }, IntPtr.Zero);
        RECT? free = null;
        for (int yy = mi.rcWork.Top + 40; yy + rh < mi.rcWork.Bottom - 200 && free == null; yy += 40)
            for (int xx = mi.rcWork.Left + 40; xx + rw < mi.rcWork.Right - 40 && free == null; xx += 40)
            {
                var c = new RECT { Left = xx, Top = yy, Right = xx + rw, Bottom = yy + rh };
                if (!occupied.Any(o => o.Left < c.Right && o.Right > c.Left && o.Top < c.Bottom && o.Bottom > c.Top)) free = c;
            }
        if (free == null)
        {
            rep.Verdict("option-c-e2e", "Inconclusive", "no uncovered desktop region found; rerun with fewer windows on screen");
            rep.Save();
            return;
        }
        rep.Set("skippedOverlays", new JsonArray(overlays.Select(o => (JsonNode)o).ToArray()));
        var R = free.Value;
        rep.Set("region", R.ToString());

        // Our app window T inside the region (stands in for any app window).
        int tx = R.Left + 60, ty = R.Top + 20, tw = 300, th = 150;
        using var t = new TestWindow(tx, ty, tw, th, WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT, gdi: true, color: 0x002288EE, title: "Atrium option C app");
        t.Show();
        PumpFor(400);

        using var monFeed = new CaptureFeed(gpu, Wgc.ForMonitor(mon), tryBorderless: true);
        using var deskFeed = new CaptureFeed(gpu, Wgc.ForWindow(GetShellWindow()), tryBorderless: true);
        using var appFeed = new CaptureFeed(gpu, Wgc.ForWindow(t.Hwnd), tryBorderless: true);
        monFeed.WaitFrames(1, 2000); deskFeed.WaitFrames(1, 2000); appFeed.WaitFrames(1, 2000);
        int ox = R.Left - mi.rcMonitor.Left, oy = R.Top - mi.rcMonitor.Top;

        byte[]? truth, desk, app;
        lock (gpu.Context)
        {
            truth = monFeed.WithLatest(x => gpu.ReadBack(x, ox, oy, rw, rh, out _));
            desk = deskFeed.WithLatest(x => gpu.ReadBack(x, ox, oy, rw, rh, out _));
            app = appFeed.WithLatest(x => gpu.ReadBack(x, 0, 0, tw, th, out _));
        }
        if (truth == null || desk == null || app == null)
        {
            rep.Verdict("option-c-e2e", "Failed", "a capture produced no frame");
            rep.Save();
            return;
        }
        // Composite: desktop layer, then the app window at its screen position.
        var rebuilt = (byte[])desk.Clone();
        for (int j = 0; j < th; j++)
            Buffer.BlockCopy(app, j * tw * 4, rebuilt, ((ty - R.Top + j) * rw + (tx - R.Left)) * 4, tw * 4);
        double diffAll = MeanDiff(rebuilt, truth), diffDeskOnly = MeanDiff(desk, truth);
        rep.Set("rebuiltVsScreen.meanAbsDiff", Math.Round(diffAll, 2));
        rep.Set("desktopLayerOnlyVsScreen.meanAbsDiff", Math.Round(diffDeskOnly, 2));
        Png.Save(Path.Combine(Report.ResultsDir, "p01e", "rebuilt.png"), rebuilt, rw, rh);
        Png.Save(Path.Combine(Report.ResultsDir, "p01e", "screen.png"), truth, rw, rh);

        // Glass over part of T and the desktop, fed only by the rebuilt backdrop.
        int gw = 400, gh = 120, gx = R.Left + 230, gy = R.Top + 80;
        using var g = new TestWindow(gx, gy, gw, gh, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, gdi: false, title: "Atrium option C glass");
        using var backdrop = gpu.Device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)rw, Height = (uint)rh, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
        }, new SubresourceData(Marshal.UnsafeAddrOfPinnedArrayElement(rebuilt, 0), (uint)(rw * 4)));
        using var glass = new GlassRenderer(gpu, g.Hwnd, gw, gh);
        var p = GlassParams.Liquid(gw, gh, 18);
        lock (gpu.Context) { glass.UpdateBackground(backdrop, gx - R.Left, gy - R.Top); glass.RenderAndPresent(p); }
        g.Show();
        for (int i = 0; i < 20; i++) { lock (gpu.Context) glass.RenderAndPresent(p); Pump(); }
        PumpFor(300);
        DwmFlush();
        var shot = WdaProbe.Gdi(R.Left, R.Top, rw, rh);
        Png.Save(Path.Combine(Report.ResultsDir, "p01e", "system-screenshot-with-glass.png"), shot, rw, rh);
        // Inside the glass shape the screenshot must differ from the plain screen.
        int sx = gx - R.Left + 60, sy = gy - R.Top + 35, sw = gw - 120, sh = gh - 70;
        double glassDiff = MeanDiffRegion(shot, truth, rw, sx, sy, sw, sh);
        double outsideDiff = MeanDiffRegion(shot, truth, rw, rw - 110, 10, 100, 40);
        rep.Set("screenshot.insideGlass.diffFromScreen", Math.Round(glassDiff, 2));
        rep.Set("screenshot.outsideGlass.diffFromScreen", Math.Round(outsideDiff, 2));

        rep.Verdict("rebuilt-backdrop-matches", diffAll < 6 ? "Passed" : "Failed", $"desktop layer + window layer vs real screen: mean abs diff {Math.Round(diffAll, 2)} (desktop layer alone {Math.Round(diffDeskOnly, 2)})");
        rep.Verdict("glass-visible-in-screenshot", glassDiff > 4 && outsideDiff < 3 ? "Passed" : "Failed", $"GDI screenshot differs inside the glass by {Math.Round(glassDiff, 2)} and outside by {Math.Round(outsideDiff, 2)}");
        rep.Save();
    }

    private static double MeanDiff(byte[] a, byte[] b)
    {
        long s = 0; int n = Math.Min(a.Length, b.Length);
        for (int i = 0; i < n; i += 4) s += Math.Abs(a[i] - b[i]) + Math.Abs(a[i + 1] - b[i + 1]) + Math.Abs(a[i + 2] - b[i + 2]);
        return s / (n / 4.0 * 3);
    }

    private static double MeanDiffRegion(byte[] a, byte[] b, int stride, int x, int y, int w, int h)
    {
        long s = 0;
        for (int j = y; j < y + h; j++)
            for (int i = x; i < x + w; i++)
            {
                int k = (j * stride + i) * 4;
                s += Math.Abs(a[k] - b[k]) + Math.Abs(a[k + 1] - b[k + 1]) + Math.Abs(a[k + 2] - b[k + 2]);
            }
        return s / (w * h * 3.0);
    }
}
