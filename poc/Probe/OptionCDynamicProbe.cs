using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P01g (option C, dynamic): rebuild the backdrop of a desktop region every time
// a layer delivers a frame, from the shell desktop capture plus one WGC capture
// per window, stacked in the CURRENT z-order. Two overlapping windows repaint
// every frame with a frame counter encoded in their colour; during the run the
// z-order swaps, one window moves, one resizes and a third window appears and
// disappears. Ground truth comes from a monitor capture (the glass is excluded
// from capture only for this measurement). Output: per-rebuild mismatch and the
// lag of the rebuilt content in frames.
internal static class OptionCDynamicProbe
{
    private sealed class Layer : IDisposable
    {
        public IntPtr Hwnd;
        public CaptureFeed Feed = null!;
        public void Dispose() => Feed.Dispose();
    }

    public static void Run()
    {
        var rep = new Report("p01g-option-c-dynamic");
        var mon = MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        using var gpu = Gpu.ForMonitor(mon);
        rep.Set("adapter", gpu.AdapterName);

        // A desktop region without app windows (skip click-through overlays).
        int rw = 760, rh = 230;
        var occupied = new List<RECT>();
        EnumWindows((h, _) =>
        {
            int exs = GetWindowLong(h, -20);
            bool overlay = (exs & (int)WS_EX_LAYERED) != 0 && (exs & (int)(WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW)) != 0;
            if (!overlay && IsWindowVisible(h) && !IsCloaked(h) && !IsIconic(h) && h != GetShellWindow() && ClassOf(h) is not "Progman" and not "WorkerW")
            {
                if (DwmGetWindowAttribute(h, 9, out RECT r, Marshal.SizeOf<RECT>()) != 0) GetWindowRect(h, out r);
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
        if (free == null) { rep.Verdict("option-c-dynamic", "Inconclusive", "no free desktop region"); rep.Save(); return; }
        var R = free.Value;
        rep.Set("region", R.ToString());

        var ex = WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT;
        var a = new TestWindow(R.Left + 40, R.Top + 20, 260, 150, ex, gdi: true, color: 0x00287800, title: "Atrium C layer A");
        var b = new TestWindow(R.Left + 200, R.Top + 60, 260, 140, ex, gdi: true, color: 0x00C80028, title: "Atrium C layer B");
        TestWindow? c3 = null;
        a.Show(); b.Show();
        var ours = new List<TestWindow> { a, b };
        PumpFor(300);

        int gx = R.Left + 120, gy = R.Top + 50, gw = 480, gh = 130;
        using var g = new TestWindow(gx, gy, gw, gh, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, gdi: false, title: "Atrium C glass");
        // Excluded only so the monitor capture can serve as ground truth.
        SetWindowDisplayAffinity(g.Hwnd, WDA_EXCLUDEFROMCAPTURE);
        using var glass = new GlassRenderer(gpu, g.Hwnd, gw, gh);
        var p = GlassParams.Liquid(gw, gh, 16);

        using var truthFeed = new CaptureFeed(gpu, Wgc.ForMonitor(mon), tryBorderless: true);
        using var deskFeed = new CaptureFeed(gpu, Wgc.ForWindow(GetShellWindow()), tryBorderless: true);
        var layers = new Dictionary<IntPtr, Layer>();
        using var rebuilt = gpu.Device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)rw, Height = (uint)rh, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
        });
        g.Show();
        truthFeed.WaitFrames(1, 2000); deskFeed.WaitFrames(1, 2000);
        PumpFor(500);
        // Baseline before anything moves: desktop layer vs monitor over the whole region,
        // with the test windows hidden.
        foreach (var w in ours) ShowWindow(w.Hwnd, SW_HIDE);
        PumpFor(600);
        var baseMask = new bool[rw * rh]; // pixels where the desktop layer already differs (other windows' DWM shadows)
        lock (gpu.Context)
        {
            var d0 = deskFeed.WithLatest(t => gpu.ReadBack(t, R.Left - mi.rcMonitor.Left, R.Top - mi.rcMonitor.Top, rw, rh, out _));
            var t0b = truthFeed.WithLatest(t => gpu.ReadBack(t, R.Left - mi.rcMonitor.Left, R.Top - mi.rcMonitor.Top, rw, rh, out _));
            if (d0 != null && t0b != null)
            {
                int bad0 = 0; long sum = 0;
                for (int i = 0; i < d0.Length; i += 4) { int dd = Math.Abs(d0[i] - t0b[i]) + Math.Abs(d0[i + 1] - t0b[i + 1]) + Math.Abs(d0[i + 2] - t0b[i + 2]); sum += dd; if (dd > 60) { bad0++; baseMask[i / 4] = true; } }
                rep.Set("baseline.desktopVsMonitor.badPercent", Math.Round(bad0 * 100.0 / (rw * rh), 2));
                rep.Set("baseline.desktopVsMonitor.meanDiff", Math.Round(sum / (rw * rh * 3.0), 2));
            }
        }
        foreach (var w in ours) w.Show();
        PumpFor(300);

        // Windows intersecting R, bottom to top, captured individually.
        List<IntPtr> Stack()
        {
            var top = new List<IntPtr>();
            EnumWindows((h, _) =>
            {
                if (h == g.Hwnd || !IsWindowVisible(h) || IsIconic(h) || IsCloaked(h)) return true;
                if (!ours.Any(o => o.Hwnd == h)) return true; // region was chosen free of other windows
                top.Add(h);
                return true;
            }, IntPtr.Zero);
            top.Reverse();
            return top;
        }

        int counter = 0, rebuilds = 0, zSwaps = 0, moves = 0;
        bool maskSaved = false;
        var mismatchDyn = new List<double>(); var startupDyn = new List<double>(); var steadyDyn = new List<double>();
        double cFirstFrameMs = -1; long cCreatedAt = -1;
        double worst = -1; long worstAt = 0; byte[]? worstRb = null, worstTr = null;
        var timeline = new List<string>();
        var mismatch = new List<double>(); var lagA = new List<double>(); var lagB = new List<double>(); var buildMs = new List<double>();
        var sw = Stopwatch.StartNew();
        long nextPaint = 0, nextSwap = 1000, nextMove = 700, nextSample = 0;
        bool aTop = false, resized = false;
        int seenTotal = -1;
        while (sw.ElapsedMilliseconds < 10000)
        {
            long now = sw.ElapsedMilliseconds;
            if (now >= nextPaint)
            {
                counter++;
                a.GdiColor = (uint)(0x00287800 | (counter & 0xFF));            // R = counter
                b.GdiColor = (uint)(0x00C80028 | ((counter & 0xFF) << 8));     // G = counter
                foreach (var w in ours) { InvalidateRect(w.Hwnd, IntPtr.Zero, false); UpdateWindow(w.Hwnd); }
                nextPaint = now + 16;
            }
            if (now >= nextSwap) { SetWindowPos((aTop ? b : a).Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); aTop = !aTop; zSwaps++; nextSwap += 1000; timeline.Add($"{now} swap"); }
            if (now >= nextMove) { GetWindowRect(b.Hwnd, out var br); int dx = (moves % 2 == 0) ? 40 : -40; SetWindowPos(b.Hwnd, IntPtr.Zero, br.Left + dx, br.Top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | 0x0004); moves++; nextMove += 700; timeline.Add($"{now} move"); }
            if (!resized && now >= 3000) { SetWindowPos(a.Hwnd, IntPtr.Zero, 0, 0, 300, 170, SWP_NOMOVE | SWP_NOACTIVATE | 0x0004); resized = true; }
            if (c3 == null && now >= 5000 && now < 7000)
            {
                c3 = new TestWindow(R.Left + 430, R.Top + 30, 200, 120, ex, gdi: true, color: 0x0000C000, title: "Atrium C layer C");
                c3.Show(); ours.Add(c3); cCreatedAt = now;
            }
            if (c3 != null && now >= 7000) { ours.Remove(c3); c3.Dispose(); c3 = null; }
            Pump();

            // Keep one capture session per window currently in the stack.
            var stack = Stack();
            foreach (var h in stack.Where(h => !layers.ContainsKey(h)))
                try { layers[h] = new Layer { Hwnd = h, Feed = new CaptureFeed(gpu, Wgc.ForWindow(h), tryBorderless: true) }; } catch { }
            foreach (var gone in layers.Keys.Where(h => !stack.Contains(h)).ToList()) { layers[gone].Dispose(); layers.Remove(gone); }
            if (c3 != null && cFirstFrameMs < 0 && layers.TryGetValue(c3.Hwnd, out var lc) && lc.Feed.Frames > 0) cFirstFrameMs = now - cCreatedAt;

            int total = deskFeed.Frames + layers.Values.Sum(l => l.Feed.Frames);
            if (total == seenTotal) { Thread.Sleep(1); continue; }
            seenTotal = total;

            // Rebuild: desktop layer, then each window at its current position, bottom to top.
            var t0 = Stopwatch.StartNew();
            lock (gpu.Context)
            {
                deskFeed.WithLatest(t => { gpu.Context.CopySubresourceRegion(rebuilt, 0, 0, 0, 0, t, 0, new Box(R.Left - mi.rcMonitor.Left, R.Top - mi.rcMonitor.Top, 0, R.Right - mi.rcMonitor.Left, R.Bottom - mi.rcMonitor.Top, 1)); return 0; });
                foreach (var h in stack)
                {
                    if (!layers.TryGetValue(h, out var l)) continue;
                    GetWindowRect(h, out var wr);
                    l.Feed.WithLatest(t =>
                    {
                        var d = t.Description;
                        int x0 = Math.Max(wr.Left, R.Left), y0 = Math.Max(wr.Top, R.Top);
                        int x1 = Math.Min(Math.Min(wr.Right, wr.Left + (int)d.Width), R.Right), y1 = Math.Min(Math.Min(wr.Bottom, wr.Top + (int)d.Height), R.Bottom);
                        if (x1 > x0 && y1 > y0)
                            gpu.Context.CopySubresourceRegion(rebuilt, 0, (uint)(x0 - R.Left), (uint)(y0 - R.Top), 0, t, 0, new Box(x0 - wr.Left, y0 - wr.Top, 0, x1 - wr.Left, y1 - wr.Top, 1));
                        return 0;
                    });
                }
                glass.UpdateBackground(rebuilt, gx - R.Left, gy - R.Top);
                glass.RenderAndPresent(p);
            }
            buildMs.Add(t0.Elapsed.TotalMilliseconds);
            rebuilds++;

            // Every ~100 ms: compare with the monitor capture and read the counters.
            if (now >= nextSample)
            {
                nextSample = now + 100;
                byte[]? rb, tr;
                lock (gpu.Context)
                {
                    rb = gpu.ReadBack(rebuilt, 0, 0, rw, rh, out _);
                    tr = truthFeed.WithLatest(t => gpu.ReadBack(t, R.Left - mi.rcMonitor.Left, R.Top - mi.rcMonitor.Top, rw, rh, out _));
                }
                if (tr != null)
                {
                    int bad = 0, badDyn = 0;
                    for (int i = 0; i < rb.Length; i += 4)
                        if (Math.Abs(rb[i] - tr[i]) + Math.Abs(rb[i + 1] - tr[i + 1]) + Math.Abs(rb[i + 2] - tr[i + 2]) > 60) { bad++; if (!baseMask[i / 4]) badDyn++; }
                    double pct = bad * 100.0 / (rw * rh);
                    mismatchDyn.Add(badDyn * 100.0 / (rw * rh));
                    if (now < 1000) startupDyn.Add(badDyn * 100.0 / (rw * rh)); else steadyDyn.Add(badDyn * 100.0 / (rw * rh));
                    mismatch.Add(pct);
                    if (pct > worst) { worst = pct; worstRb = rb; worstTr = tr; worstAt = now; }
                    if (maskSaved == false && now > 5200 && now < 6800)
                    {
                        // Where do steady differences come from? White = differing pixel.
                        var mask = new byte[rb.Length];
                        for (int i = 0; i < rb.Length; i += 4)
                        {
                            byte v = (byte)(Math.Abs(rb[i] - tr[i]) + Math.Abs(rb[i + 1] - tr[i + 1]) + Math.Abs(rb[i + 2] - tr[i + 2]) > 60 ? 255 : 0);
                            mask[i] = mask[i + 1] = mask[i + 2] = v; mask[i + 3] = 255;
                        }
                        Png.Save(Path.Combine(Report.ResultsDir, "p01g", "mid-mask.png"), mask, rw, rh);
                        Png.Save(Path.Combine(Report.ResultsDir, "p01g", "mid-rebuilt.png"), rb, rw, rh);
                        Png.Save(Path.Combine(Report.ResultsDir, "p01g", "mid-screen.png"), tr, rw, rh);
                        maskSaved = true;
                    }
                    // Counter lag at a point of A and of B that no other window covers right now.
                    double Lag(TestWindow w, int channel)
                    {
                        GetWindowRect(w.Hwnd, out var wr);
                        int px = wr.Left + 8 - R.Left, py = wr.Top + 8 - R.Top;
                        if (px < 0 || py < 0 || px >= rw || py >= rh) return double.NaN;
                        int k = (py * rw + px) * 4 + channel;
                        return (tr[k] - rb[k] + 256) % 256;
                    }
                    double la = Lag(a, 2), lb = Lag(b, 1);
                    if (!double.IsNaN(la) && la < 30) lagA.Add(la);
                    if (!double.IsNaN(lb) && lb < 30) lagB.Add(lb);
                }
            }
        }

        // Screenshot visibility without exclusion, still fed by the rebuilt backdrop.
        SetWindowDisplayAffinity(g.Hwnd, WDA_NONE);
        PumpFor(300);
        lock (gpu.Context) glass.RenderAndPresent(p);
        PumpFor(300); DwmFlush();
        var shotIn = WdaProbe.Gdi(gx + 60, gy + 30, gw - 120, gh - 60);
        double glassInShot;
        lock (gpu.Context) glassInShot = Pixels.Mean(shotIn);
        foreach (var l in layers.Values) l.Dispose();
        foreach (var w in ours) w.Dispose();

        var lags = lagA.Concat(lagB).ToList();
        if (worstRb != null && worstTr != null)
        {
            Png.Save(Path.Combine(Report.ResultsDir, "p01g", "worst-rebuilt.png"), worstRb, rw, rh);
            Png.Save(Path.Combine(Report.ResultsDir, "p01g", "worst-screen.png"), worstTr, rw, rh);
        }
        rep.Set("worstAtMs", worstAt);
        rep.Set("timeline", string.Join(", ", timeline));
        rep.Set("desktopLayerFps", Math.Round(deskFeed.Frames / 10.0, 1));
        rep.Set("monitorFps", Math.Round(truthFeed.Frames / 10.0, 1));
        rep.Set("rebuilds", rebuilds);
        rep.Set("rebuildsPerSecond", Math.Round(rebuilds / 10.0, 1));
        rep.Set("zOrderSwaps", zSwaps); rep.Set("moves", moves); rep.Set("resized", resized); rep.Set("createdAndDestroyed", 1);
        rep.Set("rebuildCpuMs.p50", Stats.Pct(buildMs, 50)); rep.Set("rebuildCpuMs.p95", Stats.Pct(buildMs, 95));
        rep.Set("mismatchPercent.p50", Stats.Pct(mismatch, 50)); rep.Set("mismatchPercent.p95", Stats.Pct(mismatch, 95)); rep.Set("mismatchPercent.max", Stats.Pct(mismatch, 100));
        rep.Set("lagFrames.p50", Stats.Pct(lags, 50)); rep.Set("lagFrames.p95", Stats.Pct(lags, 95)); rep.Set("lagSamples", lags.Count);
        rep.Set("screenshotMeanInsideGlass", Math.Round(glassInShot, 1));

        rep.Set("dynamicMismatchPercent.steady.p50", Stats.Pct(steadyDyn, 50));
        rep.Set("dynamicMismatchPercent.steady.p95", Stats.Pct(steadyDyn, 95));
        rep.Set("dynamicMismatchPercent.steady.max", Stats.Pct(steadyDyn, 100));
        rep.Set("dynamicMismatchPercent.firstSecond.max", Stats.Pct(startupDyn, 100));
        rep.Set("newWindowFirstFrameMs", cFirstFrameMs);
        double m95 = Stats.Pct(steadyDyn, 95), l95 = Stats.Pct(lags, 95);
        double shadow = (double)rep.Root["baseline.desktopVsMonitor.badPercent"]!;
        rep.Verdict("shadow-gap", shadow < 0.5 ? "Passed" : "Failed", $"{shadow}% of the region differs before anything moves: DWM shadows of other windows are in the screen but not in any window or desktop capture; option C must synthesize shadows");
        rep.Verdict("dynamic-rebuild-correct", m95 <= 1 ? "Passed" : "Failed", $"excluding the shadow gap, pixels differing during z-order swaps, moves, resize, create/destroy: p50 {Stats.Pct(steadyDyn, 50)}%, p95 {m95}%, max {Stats.Pct(steadyDyn, 100)}%");
        rep.Verdict("new-window-first-frame", cFirstFrameMs >= 0 && cFirstFrameMs <= 100 ? "Passed" : "Failed", $"a newly created window's first captured frame arrives after {cFirstFrameMs} ms; until then it is missing from the backdrop (first second max {Stats.Pct(startupDyn, 100)}%)");
        rep.Verdict("dynamic-rebuild-lag", l95 <= 3 ? "Passed" : "Failed", $"rebuilt content lags the screen by p50 {Stats.Pct(lags, 50)} / p95 {l95} frames (60 Hz counters)");
        rep.Save();
    }
}
