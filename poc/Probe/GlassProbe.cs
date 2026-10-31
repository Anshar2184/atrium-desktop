using System.Diagnostics;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P01b: a live liquid-glass bar fed by WGC monitor capture with self-exclusion.
// Measures GPU time, capture-to-present latency, self-capture echo and CPU, and
// writes an internal composite screenshot (capture + our glass) for review.
internal static class GlassProbe
{
    public static void Run(string tag)
    {
        var rep = new Report("p01b-glass" + (tag.Length > 0 ? "-" + tag : ""));
        rep.Set("idleSecondsAtStart", Math.Round(IdleSeconds(), 1));
        var mon = MonitorFromPoint(new POINT { X = 0, Y = 0 }, MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        int mw = mi.rcMonitor.Width, mh = mi.rcMonitor.Height;
        int w = 1500, h = 196;
        int x = mi.rcMonitor.Left + (mw - w) / 2, y = mi.rcMonitor.Bottom - h - 28;
        int rx = x - mi.rcMonitor.Left, ry = y - mi.rcMonitor.Top;
        rep.Set("window", $"{x},{y},{w},{h}");

        using var gpu = Gpu.ForMonitor(mon);
        rep.Set("adapter", gpu.AdapterName);
        int echoFrames = 0, otherFrames = 0, noDirtyInfo = 0;
        bool phaseStatic = false;
        using var feed = new CaptureFeed(gpu, Wgc.ForMonitor(mon), tryBorderless: true, reportDirty: true);
        feed.Inspect = f =>
        {
            if (!phaseStatic) return;
            IReadOnlyList<Windows.Graphics.RectInt32>? dirty = null;
            try { dirty = f.DirtyRegions; } catch { }
            if (dirty == null || dirty.Count == 0) { Interlocked.Increment(ref noDirtyInfo); return; }
            bool inside = dirty.All(r => r.X >= rx && r.Y >= ry && r.X + r.Width <= rx + w && r.Y + r.Height <= ry + h);
            if (inside) Interlocked.Increment(ref echoFrames); else Interlocked.Increment(ref otherFrames);
        };
        rep.Set("wgc.border", feed.BorderNote);
        rep.Set("wgc.dirty", feed.DirtyNote);
        feed.WaitFrames(1, 2000);

        var proc = Process.GetCurrentProcess();
        // Phase 0: capture rate with no glass on screen.
        int f0 = feed.Frames; var t0 = Stopwatch.StartNew();
        PumpFor(3000);
        double rate0 = (feed.Frames - f0) / t0.Elapsed.TotalSeconds;
        rep.Set("phase0.captureFps", Math.Round(rate0, 1));

        var ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;
        using var win = new TestWindow(x, y, w, h, ex, gdi: false, title: "Atrium Glass Probe");
        SetWindowDisplayAffinity(win.Hwnd, WDA_EXCLUDEFROMCAPTURE);
        using var glass = new GlassRenderer(gpu, win.Hwnd, w, h);
        float inset = 26;
        var liquid = GlassParams.Liquid(w, h, inset);
        var frosted = GlassParams.Frosted(w, h, inset);

        void Frame(GlassParams p)
        {
            lock (gpu.Context)
            {
                feed.WithLatest(t => { glass.UpdateBackground(t, rx, ry); return 0; });
                glass.RenderAndPresent(p);
            }
        }

        Frame(liquid);
        win.Show();

        // Phase 1: continuous animation (capsule breathing) for 5 s at vsync.
        var latency = new List<double>();
        var cpuStart = proc.TotalProcessorTime; var t1 = Stopwatch.StartNew();
        int p1 = glass.Presents, f1 = feed.Frames;
        while (t1.ElapsedMilliseconds < 5000)
        {
            if (!Pump()) break;
            float k = (float)(0.5 + 0.5 * Math.Sin(t1.Elapsed.TotalSeconds * 2.2));
            var p = liquid; p.Inset = inset + 8 * k; p.Refract = 22 + 10 * k;
            Frame(p);
            latency.Add(Environment.TickCount64 - feed.LastFrameTicks);
        }
        proc.Refresh();
        double cpu1 = (proc.TotalProcessorTime - cpuStart).TotalMilliseconds / t1.Elapsed.TotalMilliseconds / Environment.ProcessorCount * 100;
        rep.Set("phase1.presentFps", Math.Round((glass.Presents - p1) / t1.Elapsed.TotalSeconds, 1));
        rep.Set("phase1.captureFps", Math.Round((feed.Frames - f1) / t1.Elapsed.TotalSeconds, 1));
        rep.Set("phase1.cpuPercentOfMachine", Math.Round(cpu1, 2));
        rep.Set("phase1.frameAgeMs.p50", Stats.Pct(latency, 50));
        rep.Set("phase1.frameAgeMs.p95", Stats.Pct(latency, 95));

        // Phase 2: static glass, redraw only when a new capture frame arrives.
        phaseStatic = true;
        cpuStart = proc.TotalProcessorTime; var t2 = Stopwatch.StartNew();
        int p2 = glass.Presents, f2 = feed.Frames, seen = feed.Frames;
        while (t2.ElapsedMilliseconds < 5000)
        {
            if (!Pump()) break;
            if (feed.Frames != seen) { seen = feed.Frames; Frame(liquid); }
            else Thread.Sleep(2);
        }
        phaseStatic = false;
        proc.Refresh();
        double cpu2 = (proc.TotalProcessorTime - cpuStart).TotalMilliseconds / t2.Elapsed.TotalMilliseconds / Environment.ProcessorCount * 100;
        double rate2 = (feed.Frames - f2) / t2.Elapsed.TotalSeconds;
        rep.Set("phase2.captureFps", Math.Round(rate2, 1));
        rep.Set("phase2.presentFps", Math.Round((glass.Presents - p2) / t2.Elapsed.TotalSeconds, 1));
        rep.Set("phase2.cpuPercentOfMachine", Math.Round(cpu2, 2));
        rep.Set("phase2.dirty.onlyInsideGlass", echoFrames);
        rep.Set("phase2.dirty.elsewhere", otherFrames);
        rep.Set("phase2.dirty.none", noDirtyInfo);

        // Phase 2b: same static glass with an echo filter. A frame whose dirty
        // rects all lie inside the glass is drawn only if the backdrop under the
        // glass really changed (coarse mip comparison). A helper window *under*
        // the glass flips colour twice per second to prove real changes still
        // get through.
        var helper = new TestWindow(x + 420, y + 50, 160, 90, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT, gdi: true, color: 0x00FF8800);
        helper.Show();
        SetWindowPos(win.Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        PumpFor(300);
        int echoSkipped = 0, echoDrawn = 0, outsideDrawn = 0; var reactMs = new List<double>();
        var lastDirtyInside = false;
        feed.Inspect = f =>
        {
            IReadOnlyList<Windows.Graphics.RectInt32>? dirty = null;
            try { dirty = f.DirtyRegions; } catch { }
            lastDirtyInside = dirty != null && dirty.Count > 0 &&
                dirty.All(r => r.X >= rx && r.Y >= ry && r.X + r.Width <= rx + w && r.Y + r.Height <= ry + h);
        };
        var sigCost = new List<double>();
        cpuStart = proc.TotalProcessorTime; var t2b = Stopwatch.StartNew();
        int p2b = glass.Presents, f2b = feed.Frames; seen = feed.Frames; long nextFlip = 0; bool flip = false;
        while (t2b.ElapsedMilliseconds < 6000)
        {
            if (!Pump()) break;
            if (t2b.ElapsedMilliseconds >= nextFlip)
            {
                flip = !flip; helper.GdiColor = flip ? 0x000088FFu : 0x00FF8800u;
                InvalidateRect(helper.Hwnd, IntPtr.Zero, false); UpdateWindow(helper.Hwnd);
                nextFlip += 500;
            }
            if (feed.Frames == seen) { Thread.Sleep(2); continue; }
            seen = feed.Frames;
            long arrived = feed.LastFrameTicks;
            bool inside = lastDirtyInside;
            lock (gpu.Context)
            {
                feed.WithLatest(t => { glass.UpdateBackground(t, rx, ry); return 0; });
                var cmp = glass.CompareWithPresented();
                sigCost.Add(cmp.Ms);
                if (!cmp.Changed) { if (inside) echoSkipped++; continue; }
                if (inside) echoDrawn++; else outsideDrawn++;
                glass.RenderAndPresent(liquid);
            }
            reactMs.Add(Environment.TickCount64 - arrived);
        }
        proc.Refresh();
        double cpu2b = (proc.TotalProcessorTime - cpuStart).TotalMilliseconds / t2b.Elapsed.TotalMilliseconds / Environment.ProcessorCount * 100;
        double rate2b = (feed.Frames - f2b) / t2b.Elapsed.TotalSeconds;
        helper.Dispose();
        feed.Inspect = null;
        rep.Set("phase2b.captureFps", Math.Round(rate2b, 1));
        rep.Set("phase2b.presentFps", Math.Round((glass.Presents - p2b) / t2b.Elapsed.TotalSeconds, 1));
        rep.Set("phase2b.cpuPercentOfMachine", Math.Round(cpu2b, 2));
        rep.Set("phase2b.echoSkipped", echoSkipped);
        rep.Set("phase2b.insideButChangedDrawn", echoDrawn);
        rep.Set("phase2b.outsideDrawn", outsideDrawn);
        rep.Set("phase2b.signatureMs.p95", Stats.Pct(sigCost, 95));
        rep.Set("phase2b.arrivalToPresentMs.p50", Stats.Pct(reactMs, 50));
        rep.Set("phase2b.arrivalToPresentMs.p95", Stats.Pct(reactMs, 95));

        // Phase 3: animated switch Liquid -> Frosted with the same engine.
        var t3 = Stopwatch.StartNew();
        while (t3.ElapsedMilliseconds < 1200)
        {
            if (!Pump()) break;
            float k = Math.Min(1f, (float)t3.Elapsed.TotalMilliseconds / 900f);
            Frame(liquid.Lerp(frosted, k * k * (3 - 2 * k)));
        }
        PumpFor(800);

        (List<double> Update, List<double> Draw) bench = (new(), new());
        lock (gpu.Context) feed.WithLatest(t => { bench = glass.BenchOffscreen(t, rx, ry, liquid, 200); return 0; });
        double thr = double.NaN;
        lock (gpu.Context) feed.WithLatest(t => { thr = glass.BenchThroughputMs(t, rx, ry, liquid, 300); return 0; });
        rep.Set("bench.throughputMsPerFrame", Math.Round(thr, 3));
        rep.Set("bench.updateMs.p50", Stats.Pct(bench.Update, 50));
        rep.Set("bench.updateMs.p95", Stats.Pct(bench.Update, 95));
        rep.Set("bench.drawMs.p50", Stats.Pct(bench.Draw, 50));
        rep.Set("bench.drawMs.p95", Stats.Pct(bench.Draw, 95));
        rep.Set("gpuMs.note", "gpuMs.* brackets the draw into the swap-chain buffer and can include waiting for DWM to release it; bench.* is the pure GPU cost");
        rep.Set("gpuMs.samples", glass.GpuMs.Count);
        rep.Set("gpuMs.p50", Stats.Pct(glass.GpuMs, 50));
        rep.Set("gpuMs.p95", Stats.Pct(glass.GpuMs, 95));
        rep.Set("gpuMs.p99", Stats.Pct(glass.GpuMs, 99));
        proc.Refresh();
        rep.Set("privateMB", proc.PrivateMemorySize64 / 1048576);
        rep.Set("workingSetMB", proc.WorkingSet64 / 1048576);

        // Internal screenshot: what the user sees = capture beneath + our glass.
        int m = 90, cx = Math.Max(0, rx - m), cy = Math.Max(0, ry - m);
        int cw = Math.Min(mw - cx, w + 2 * m), ch = Math.Min(mh - cy, h + 2 * m);
        byte[]? under = null;
        lock (gpu.Context) under = feed.WithLatest(t => gpu.ReadBack(t, cx, cy, cw, ch, out _));
        if (under != null)
        {
            Png.Save(Path.Combine(Report.ResultsDir, "p01b", $"{tag}system-screenshot-view.png"), under, cw, ch);
            foreach (var (name, p) in new[] { ("liquid", liquid), ("frosted", frosted) })
            {
                byte[] g;
                lock (gpu.Context) g = glass.RenderOffscreen(p);
                var comp = (byte[])under.Clone();
                for (int j = 0; j < h; j++)
                    for (int i = 0; i < w; i++)
                    {
                        int s = (j * w + i) * 4, d = ((j + ry - cy) * cw + (i + rx - cx)) * 4;
                        int a = g[s + 3];
                        for (int k = 0; k < 3; k++) comp[d + k] = (byte)Math.Min(255, g[s + k] + comp[d + k] * (255 - a) / 255);
                    }
                Png.Save(Path.Combine(Report.ResultsDir, "p01b", $"{tag}composite-{name}.png"), comp, cw, ch);
            }
        }

        rep.Verdict("live-glass-renders", glass.Presents > 100 ? "Passed" : "Failed", $"{glass.Presents} presents through D3D11 + DirectComposition with WDA exclusion");
        double p95 = Math.Round(Stats.Pct(bench.Update, 95) + Stats.Pct(bench.Draw, 95), 3);
        double cost = p95 > 0 ? p95 : thr;
        rep.Verdict("gpu-budget", double.IsNaN(cost) ? "Inconclusive" : cost <= 3 ? "Passed" : "Failed",
            $"pure GPU cost {cost} ms per frame (timestamp p95 {p95}, throughput {Math.Round(thr, 3)}) for ROI copy+mips+glass draw of a {w}x{h} bar (target <= 3 ms)");
        double age95 = Stats.Pct(latency, 95);
        rep.Verdict("backdrop-age", age95 <= 34 ? "Passed" : "Inconclusive", $"p95 age of the backdrop frame when drawn: {age95} ms (target <= 2 refresh periods); WGC only delivers frames when the screen changes, so high values can mean a static desktop");
        bool echo = echoFrames > Math.Max(10, otherFrames);
        rep.Verdict("no-self-echo", echo ? "Failed" : "Passed", $"static phase: {echoFrames} capture frames dirty only inside the glass vs {otherFrames} elsewhere; capture {Math.Round(rate2, 1)} fps vs {Math.Round(rate0, 1)} fps without glass");
        // 12 flips under the glass; each should produce at least one redraw.
        rep.Verdict("echo-filter", rate2b < rate2 * 0.4 && echoDrawn >= 10 ? "Passed" : "Failed",
            $"with filter: capture {Math.Round(rate2b, 1)} fps, {echoSkipped} echo frames skipped, {echoDrawn} real changes under the glass drawn (12 colour flips), {outsideDrawn} outside changes");
        rep.Save();
    }
}
