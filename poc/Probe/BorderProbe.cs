using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// Does the yellow WGC capture border actually disappear when IsBorderRequired is
// false? Samples the screen ring around a captured test window with GDI.
internal static class BorderProbe
{
    public static void Run()
    {
        var rep = new Report("p01d-capture-border");
        var mon = MonitorFromPoint(new POINT { X = 0, Y = 0 }, MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        using var gpu = Gpu.ForMonitor(mon);
        int x = mi.rcWork.Left + 300, y = mi.rcWork.Top + 300, w = 400, h = 260;
        var arr = new JsonArray();
        foreach (bool borderless in new[] { false, true })
        {
            using var win = new TestWindow(x, y, w, h, WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT, gdi: true, color: 0x00404040, title: "Atrium border probe");
            win.Show();
            PumpFor(300);
            using var feed = new CaptureFeed(gpu, Wgc.ForWindow(win.Hwnd), tryBorderless: borderless);
            feed.WaitFrames(1, 1500);
            PumpFor(700);
            DwmFlush();
            // GDI ring: the outer 6 px of the window, where DWM draws the border.
            var all = WdaProbe.Gdi(x, y, w, h);
            var ring = new List<byte>();
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    if (i < 6 || j < 6 || i >= w - 6 || j >= h - 6) ring.AddRange(all.AsSpan((j * w + i) * 4, 4).ToArray());
            var px = ring.ToArray();
            double yellow = 0;
            for (int i = 0; i < px.Length; i += 4)
            {
                int b = px[i], g = px[i + 1], r = px[i + 2];
                if (r > 200 && g > 150 && b < 90) yellow++;
            }
            yellow /= px.Length / 4.0;
            arr.Add(new JsonObject { ["requestedBorderless"] = borderless, ["note"] = feed.BorderNote, ["yellowFractionOnEdge"] = Math.Round(yellow, 3) });
        }
        rep.Set("cases", arr);
        double withBorder = (double)arr[0]!["yellowFractionOnEdge"]!, without = (double)arr[1]!["yellowFractionOnEdge"]!;
        rep.Verdict("border-visible-by-default", withBorder > 0.2 ? "Passed" : "Inconclusive", $"yellow fraction on window edge with default border: {withBorder}");
        rep.Verdict("borderless-honoured", without < 0.02 && withBorder > 0.2 ? "Passed" : withBorder <= 0.2 ? "Inconclusive" : "Failed",
            $"yellow fraction with IsBorderRequired=false: {without}");
        rep.Save();
    }
}
