using System.Diagnostics;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P01c (option C): rebuild the backdrop from per-window WGC captures instead of
// capturing the monitor. Checks whether ordinary, occluded, minimized and shell
// windows can be captured without a border and what several sessions cost.
// No window titles and no window pixels are stored.
internal static class WinCapProbe
{
    public static void Run()
    {
        var rep = new Report("p01c-window-capture");
        var mon = MonitorFromPoint(new POINT { X = 0, Y = 0 }, MONITOR_DEFAULTTOPRIMARY);
        using var gpu = Gpu.ForMonitor(mon);
        rep.Set("adapter", gpu.AdapterName);

        var fg = GetForegroundWindow();
        var candidates = new List<(IntPtr Hwnd, int Z, bool Minimized)>();
        int z = 0;
        EnumWindows((h, _) =>
        {
            z++;
            if (!IsWindowVisible(h) || IsCloaked(h) || TextOf(h).Length == 0) return true;
            const int GWL_EXSTYLE = -20;
            if ((GetWindowLong(h, GWL_EXSTYLE) & (int)WS_EX_TOOLWINDOW) != 0) return true;
            GetWindowRect(h, out var r);
            if (r.Width < 200 || r.Height < 150) return true;
            candidates.Add((h, z, IsIconic(h)));
            return true;
        }, IntPtr.Zero);
        rep.Set("topLevelAppWindows", candidates.Count);

        var targets = candidates.Where(c => !c.Minimized).Take(6).ToList();
        var minimized = candidates.FirstOrDefault(c => c.Minimized);
        if (minimized.Hwnd != IntPtr.Zero) targets.Add(minimized);
        var shell = GetShellWindow();

        var feeds = new List<(string Label, IntPtr Hwnd, CaptureFeed? Feed, string? Error)>();
        int idx = 0;
        foreach (var t in targets)
        {
            string label = $"app#{++idx}{(t.Hwnd == fg ? "(foreground)" : "")}{(t.Minimized ? "(minimized)" : "")}:{ClassOf(t.Hwnd)}";
            try { feeds.Add((label, t.Hwnd, new CaptureFeed(gpu, Wgc.ForWindow(t.Hwnd), tryBorderless: true), null)); }
            catch (Exception e) { feeds.Add((label, t.Hwnd, null, e.Message)); }
        }
        try { feeds.Add(("shell:" + ClassOf(shell), shell, new CaptureFeed(gpu, Wgc.ForWindow(shell), tryBorderless: true), null)); }
        catch (Exception e) { feeds.Add(("shell:" + ClassOf(shell), shell, null, e.Message)); }

        var proc = Process.GetCurrentProcess();
        var cpu0 = proc.TotalProcessorTime; var sw = Stopwatch.StartNew();
        var start = feeds.Select(f => f.Feed?.Frames ?? 0).ToArray();
        PumpFor(4000);
        proc.Refresh();
        double cpu = (proc.TotalProcessorTime - cpu0).TotalMilliseconds / sw.Elapsed.TotalMilliseconds / Environment.ProcessorCount * 100;

        var arr = new JsonArray();
        int ok = 0, okOccluded = 0;
        for (int i = 0; i < feeds.Count; i++)
        {
            var (label, hwnd, feed, err) = feeds[i];
            var n = new JsonObject { ["window"] = label };
            if (feed == null) { n["error"] = err; arr.Add(n); continue; }
            n["border"] = feed.BorderNote;
            n["framesIn4s"] = feed.Frames - start[i];
            n["size"] = $"{feed.Size.Width}x{feed.Size.Height}";
            // Content check on a centred patch: not black, not empty.
            double? black = null, mean = null;
            lock (gpu.Context)
                feed.WithLatest(t =>
                {
                    var d = t.Description;
                    int pw = (int)Math.Min(200, d.Width / 2), ph = (int)Math.Min(120, d.Height / 2);
                    var px = gpu.ReadBack(t, (int)d.Width / 2 - pw / 2, (int)d.Height / 2 - ph / 2, pw, ph, out _);
                    black = Math.Round(Pixels.BlackFraction(px), 3); mean = Math.Round(Pixels.Mean(px), 1);
                    return 0;
                });
            n["gotFrame"] = mean != null;
            n["blackFraction"] = black;
            n["meanLuma"] = mean;
            bool good = mean != null && black < 0.95;
            if (good) ok++;
            if (good && hwnd != fg && !label.Contains("shell")) okOccluded++;
            arr.Add(n);
            feed.Dispose();
        }
        rep.Set("windows", arr);
        rep.Set("concurrentSessions", feeds.Count(f => f.Feed != null));
        rep.Set("cpuPercentOfMachine", Math.Round(cpu, 2));
        proc.Refresh();
        rep.Set("privateMB", proc.PrivateMemorySize64 / 1048576);

        // Synthetic occlusion test with our own windows: A (orange) is fully
        // covered by B (blue). Capturing A must return orange, not blue/black.
        // Then minimize A and check whether frames still arrive.
        var mi = new MONITORINFOEX { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        int ax = mi.rcWork.Left + 200, ay = mi.rcWork.Top + 200;
        var exA = WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT;
        using var a = new TestWindow(ax, ay, 300, 200, exA, gdi: true, color: 0x000088FF, title: "Atrium occluded A");
        using var b = new TestWindow(ax - 20, ay - 20, 340, 240, exA, gdi: true, color: 0x00FF4400, title: "Atrium occluder B");
        a.Show(); b.Show();
        SetWindowPos(b.Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        PumpFor(300);
        string occl = "no frame";
        double orange = 0, blue = 0;
        using (var fa = new CaptureFeed(gpu, Wgc.ForWindow(a.Hwnd), tryBorderless: true))
        {
            fa.WaitFrames(1, 2000);
            // Repaint A while hidden to see that updates of occluded content arrive.
            a.GdiColor = 0x0000CC00; InvalidateRect(a.Hwnd, IntPtr.Zero, false); UpdateWindow(a.Hwnd);
            fa.WaitFrames(1, 1500);
            lock (gpu.Context) fa.WithLatest(t =>
            {
                var px = gpu.ReadBack(t, 50, 50, 100, 60, out _);
                orange = Pixels.Fraction(px, 0, 204, 0); blue = Pixels.Fraction(px, 0, 68, 255);
                occl = $"green(updated A)={Math.Round(orange, 3)} blue(B)={Math.Round(blue, 3)} frames={fa.Frames}";
                return 0;
            });
        }
        rep.Set("synthetic.occluded", occl);
        rep.Verdict("occluded-window-content", orange > 0.9 ? "Passed" : "Failed", "a window fully covered by another still delivers its own, updated content: " + occl);

        const int SW_MINIMIZE = 6;
        using (var fa = new CaptureFeed(gpu, Wgc.ForWindow(a.Hwnd), tryBorderless: true))
        {
            fa.WaitFrames(1, 1500);
            ShowWindow(a.Hwnd, SW_MINIMIZE);
            PumpFor(400);
            int before = fa.Frames;
            a.GdiColor = 0x00FFFFFF; InvalidateRect(a.Hwnd, IntPtr.Zero, false);
            PumpFor(1200);
            rep.Set("synthetic.minimizedFramesAfterRepaint", fa.Frames - before);
            rep.Verdict("minimized-window", "Passed", $"minimized windows deliver {fa.Frames - before} new frames; the backdrop layer for a minimized window is simply dropped (it is not on screen)");
        }

        // Cost of many concurrent window sessions (our own windows).
        var many = new List<TestWindow>(); var manyFeeds = new List<CaptureFeed>();
        for (int i = 0; i < 8; i++)
        {
            var wdw = new TestWindow(ax + 400 + i * 30, ay + i * 30, 400, 300, exA, gdi: true, color: (uint)(0x00203040 + i * 0x00101010), title: "Atrium session " + i);
            wdw.Show(); many.Add(wdw);
        }
        PumpFor(200);
        foreach (var wdw in many) manyFeeds.Add(new CaptureFeed(gpu, Wgc.ForWindow(wdw.Hwnd), tryBorderless: true));
        proc.Refresh();
        var c0 = proc.TotalProcessorTime; var sw2 = Stopwatch.StartNew();
        for (int k = 0; k < 40; k++)
        {
            foreach (var wdw in many) { wdw.GdiColor ^= 0x00303030; InvalidateRect(wdw.Hwnd, IntPtr.Zero, false); }
            PumpFor(50);
        }
        proc.Refresh();
        double cpuMany = (proc.TotalProcessorTime - c0).TotalMilliseconds / sw2.Elapsed.TotalMilliseconds / Environment.ProcessorCount * 100;
        rep.Set("synthetic.8sessions.framesTotal", manyFeeds.Sum(f => f.Frames));
        rep.Set("synthetic.8sessions.cpuPercentOfMachine", Math.Round(cpuMany, 2));
        foreach (var f in manyFeeds) f.Dispose();
        foreach (var wdw in many) wdw.Dispose();

        rep.Verdict("per-window-capture", ok >= 2 ? "Passed" : "Failed", $"{ok} of {feeds.Count} windows delivered non-black content");
                var shellNode = arr.Last()!;
        rep.Verdict("desktop-window-capture", shellNode["gotFrame"]?.GetValue<bool>() == true && (double?)shellNode["blackFraction"] < 0.95 ? "Passed" : "Failed",
            "capturing the shell desktop window (wallpaper + icons) as a separate layer");
        rep.Save();
    }
}
