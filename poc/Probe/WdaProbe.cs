using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P01a: does WDA_EXCLUDEFROMCAPTURE remove our window from WGC, DXGI duplication
// and GDI screen reads, and do those paths then see the real content beneath
// (not black, not a stale copy of the window)?
internal static class WdaProbe
{
    public static void Run()
    {
        var rep = new Report("p01a-wda");
        var mon = MonitorFromPoint(new POINT { X = 0, Y = 0 }, MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        // A small window near the right edge, above the taskbar.
        int w = 260, h = 160;
        int x = mi.rcWork.Right - w - 40, y = mi.rcWork.Bottom - h - 220;
        int sx = x + 40, sy = y + 40, sw = w - 80, sh = h - 80; // sample inset
        rep.Set("monitor", mi.rcMonitor.ToString());
        rep.Set("window", $"{x},{y},{w},{h}");

        using var gpu = Gpu.ForMonitor(mon);
        rep.Set("adapter", gpu.AdapterName);
        using var wgc = new CaptureFeed(gpu, Wgc.ForMonitor(mon), tryBorderless: true);
        rep.Set("wgc.border", wgc.BorderNote);
        DupFeed? dup = null;
        try { dup = new DupFeed(gpu, mon); } catch (Exception e) { rep.Set("dup.error", e.Message); }
        wgc.WaitFrames(1, 2000);
        dup?.Poll(500);

        byte[]? baseWgc = Read(gpu, wgc, sx - mi.rcMonitor.Left, sy - mi.rcMonitor.Top, sw, sh);
        byte[] baseGdi = Gdi(sx, sy, sw, sh);

        var cases = new JsonArray();
        foreach (var (label, ex) in new[]
        {
            ("plain", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST),
            ("layered-transparent", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT),
        })
        {
            foreach (bool exclude in new[] { false, true })
            {
                using var win = new TestWindow(x, y, w, h, ex, gdi: true, color: 0x00FF00FF);
                bool setOk = true; int err = 0;
                if (exclude)
                {
                    setOk = SetWindowDisplayAffinity(win.Hwnd, WDA_EXCLUDEFROMCAPTURE);
                    err = System.Runtime.InteropServices.Marshal.GetLastWin32Error();
                }
                win.Show();
                PumpFor(400);
                DwmFlush();
                wgc.WaitFrames(2, 1500);
                for (int i = 0; i < 5; i++) dup?.Poll(100);
                GetWindowDisplayAffinity(win.Hwnd, out var aff);

                var pw = Read(gpu, wgc, sx - mi.rcMonitor.Left, sy - mi.rcMonitor.Top, sw, sh);
                var pd = dup?.Latest == null ? null : ReadTex(gpu, dup.Latest, sx - mi.rcMonitor.Left, sy - mi.rcMonitor.Top, sw, sh);
                var pg = Gdi(sx, sy, sw, sh);
                var c = new JsonObject
                {
                    ["case"] = label,
                    ["exclude"] = exclude,
                    ["setAffinityOk"] = setOk,
                    ["lastError"] = err,
                    ["affinity"] = $"0x{aff:X}",
                    ["wgc.magenta"] = pw == null ? null : Math.Round(Pixels.Fraction(pw, 255, 0, 255), 3),
                    ["wgc.black"] = pw == null ? null : Math.Round(Pixels.BlackFraction(pw), 3),
                    ["wgc.diffFromBaseline"] = pw == null || baseWgc == null ? null : Math.Round(Diff(pw, baseWgc), 1),
                    ["dxgi.magenta"] = pd == null ? null : Math.Round(Pixels.Fraction(pd, 255, 0, 255), 3),
                    ["dxgi.black"] = pd == null ? null : Math.Round(Pixels.BlackFraction(pd), 3),
                    ["gdi.magenta"] = Math.Round(Pixels.Fraction(pg, 255, 0, 255), 3),
                    ["gdi.black"] = Math.Round(Pixels.BlackFraction(pg), 3),
                    ["gdi.diffFromBaseline"] = Math.Round(Diff(pg, baseGdi), 1),
                };
                cases.Add(c);
                Console.WriteLine(c.ToJsonString());
                if (pw != null) Pixels.SaveBmp(Path.Combine(Report.ResultsDir, "p01a", $"{label}-{(exclude ? "excluded" : "visible")}-wgc.bmp"), pw, sw, sh);
            }
        }
        rep.Set("cases", cases);

        bool visibleSeen = cases.Where(c => !(bool)c!["exclude"]!).All(c => (double)c!["wgc.magenta"]! > 0.9 && (double)c["gdi.magenta"]! > 0.9);
        bool excludedHidden = cases.Where(c => (bool)c!["exclude"]!).All(c =>
            (double)c!["wgc.magenta"]! < 0.02 && (double)c["gdi.magenta"]! < 0.02 && (double)c["wgc.black"]! < 0.5 &&
            (c["dxgi.magenta"] == null || (double)c["dxgi.magenta"]! < 0.02));
        rep.Verdict("control-visible", visibleSeen ? "Passed" : "Failed", "without exclusion the window shows in WGC and GDI");
        rep.Verdict("wda-excludes", excludedHidden ? "Passed" : "Failed", "with WDA_EXCLUDEFROMCAPTURE WGC/DXGI/GDI show the content beneath, not the window and not black");
        dup?.Dispose();
        rep.Save();
    }

    private static byte[]? Read(Gpu gpu, CaptureFeed f, int x, int y, int w, int h) =>
        f.WithLatest(t => { lock (gpu.Context) return gpu.ReadBack(t, x, y, w, h, out _); });

    private static byte[] ReadTex(Gpu gpu, Vortice.Direct3D11.ID3D11Texture2D t, int x, int y, int w, int h)
    {
        lock (gpu.Context) return gpu.ReadBack(t, x, y, w, h, out _);
    }

    internal static byte[] Gdi(int x, int y, int w, int h)
    {
        var screen = GetDC(IntPtr.Zero);
        var mem = CreateCompatibleDC(screen);
        var bmp = CreateCompatibleBitmap(screen, w, h);
        var old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, screen, x, y, 0x00CC0020 | 0x40000000); // SRCCOPY | CAPTUREBLT
        var data = new byte[w * h * 4];
        for (int j = 0; j < h; j += 1)
            for (int i = 0; i < w; i += 1)
            {
                uint c = GetPixel(mem, i, j);
                int k = (j * w + i) * 4;
                data[k] = (byte)(c >> 16); data[k + 1] = (byte)(c >> 8); data[k + 2] = (byte)c; data[k + 3] = 255;
            }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(IntPtr.Zero, screen);
        return data;
    }

    private static double Diff(byte[] a, byte[] b)
    {
        long s = 0; int n = Math.Min(a.Length, b.Length);
        for (int i = 0; i < n; i += 4) s += Math.Abs(a[i] - b[i]) + Math.Abs(a[i + 1] - b[i + 1]) + Math.Abs(a[i + 2] - b[i + 2]);
        return n == 0 ? 0 : s / (n / 4.0 * 3);
    }
}
