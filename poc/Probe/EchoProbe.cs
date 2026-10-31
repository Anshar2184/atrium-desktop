using System.Diagnostics;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P01f: does the echo filter keep slow and small backdrop changes? The glass is
// redrawn only when the current backdrop differs from the backdrop that was last
// PRESENTED (exact GPU comparison), plus a 2 s staleness cap. Under the glass:
// a 1-step-per-frame gradient, a 1 px blinking line, low-contrast scrolling
// text and a one-frame transient. Every present records what was drawn.
internal static class EchoProbe
{
    private sealed record Sample(double T, int Step, int PresentedGray, bool CursorOn, bool CursorShown, int TextOffset, ulong TextHash, bool TransientOn, bool TransientShown);

    public static void Run()
    {
        var rep = new Report("p01f-echo-slow-changes");
        var mon = MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        int w = 1500, h = 196, gx = mi.rcMonitor.Left + (mi.rcMonitor.Width - w) / 2, gy = mi.rcMonitor.Bottom - h - 28;
        int rx = gx - mi.rcMonitor.Left, ry = gy - mi.rcMonitor.Top;
        int P = GlassRenderer.Pad;
        (int X, int Y) Roi(int sx, int sy) => (sx - gx + P, sy - gy + P);

        using var gpu = Gpu.ForMonitor(mon);
        rep.Set("adapter", gpu.AdapterName);
        using var feed = new CaptureFeed(gpu, Wgc.ForMonitor(mon), tryBorderless: true);
        feed.WaitFrames(1, 2000);

        var exH = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT;
        // Helpers live inside the glass rectangle, away from the rounded ends.
        int baseY = gy + 50;
        using var grad = new TestWindow(gx + 200, baseY, 160, 90, exH, gdi: true, color: Gray(60), title: "Atrium echo gradient");
        using var cursor = new TestWindow(gx + 460, baseY + 20, 1, 40, exH, gdi: true, color: 0x000000FF, title: "Atrium echo cursor");
        using var text = new TestWindow(gx + 560, baseY + 10, 320, 44, exH, gdi: true, color: Gray(200), title: "Atrium echo text");
        using var flash = new TestWindow(gx + 960, baseY + 20, 60, 40, exH, gdi: true, color: 0x0000FF00, title: "Atrium echo transient");
        int textOffset = 0;
        text.Painter = (hdc, rc) =>
        {
            SetBkMode(hdc, 1);
            SetTextColor(hdc, Gray(196)); // 4 levels from the background
            TextOut(hdc, 8 - textOffset, 12, "low contrast text 0123456789 abcdefghij low contrast text 0123456789", 69);
        };
        grad.Show(); text.Show();
        ShowWindow(cursor.Hwnd, SW_HIDE); ShowWindow(flash.Hwnd, SW_HIDE);

        var ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;
        using var win = new TestWindow(gx, gy, w, h, ex, gdi: false, title: "Atrium echo glass");
        SetWindowDisplayAffinity(win.Hwnd, WDA_EXCLUDEFROMCAPTURE);
        using var glass = new GlassRenderer(gpu, win.Hwnd, w, h);
        var p = GlassParams.Frosted(w, h, 26);
        p.TintMix = 0.08f;
        win.Show();
        PumpFor(800);

        var (gX, gY) = Roi(gx + 280, baseY + 45);
        var (cX, cY) = Roi(gx + 460, baseY + 40);
        var (tX, tY) = Roi(gx + 560, baseY + 10);
        var (fX, fY) = Roi(gx + 990, baseY + 40);

        int step = 0; bool cursorOn = false, flashOn = false;
        var samples = new List<Sample>();
        var cursorToggles = new List<(double T, bool On)>();
        var flashEvents = new List<(double T, bool On)>();
        var textSteps = new List<double>();
        int presents = 0, compares = 0, quietPresents = 0, quietFrames = 0, stalePresents = 0;
        var cmpMs = new List<double>();
        var sw = Stopwatch.StartNew();
        double lastPresent = 0;
        int seen = feed.Frames;
        long nextFlash = 6600;

        void Repaint(TestWindow t) { InvalidateRect(t.Hwnd, IntPtr.Zero, false); UpdateWindow(t.Hwnd); }

        while (sw.ElapsedMilliseconds < 12600)
        {
            double now = sw.Elapsed.TotalMilliseconds;
            // Drive the helpers along a fixed timeline.
            if (now < 2100)
            {
                int s = Math.Min(120, (int)(now / 16.7));
                if (s != step) { step = s; grad.GdiColor = Gray(60 + step); Repaint(grad); }
            }
            else if (now >= 2200 && now < 4300)
            {
                bool on = ((int)((now - 2200) / 400) % 2) == 0;
                if (on != cursorOn || cursorToggles.Count == 0)
                {
                    cursorOn = on; ShowWindow(cursor.Hwnd, on ? SW_SHOWNA : SW_HIDE);
                    cursorToggles.Add((now, on));
                }
            }
            else if (now >= 4300 && cursorOn) { cursorOn = false; ShowWindow(cursor.Hwnd, SW_HIDE); cursorToggles.Add((now, false)); }
            if (now >= 4400 && now < 6400)
            {
                int o = (int)((now - 4400) / 33);
                if (o != textOffset) { textOffset = o; Repaint(text); textSteps.Add(now); }
            }
            if (now >= nextFlash && nextFlash < 9400 && !flashOn)
            {
                flashOn = true; ShowWindow(flash.Hwnd, SW_SHOWNA); Repaint(flash); flashEvents.Add((now, true));
            }
            else if (flashOn && now - flashEvents[^1].T >= 16)
            {
                flashOn = false; ShowWindow(flash.Hwnd, SW_HIDE); flashEvents.Add((now, false)); nextFlash += 700;
            }
            Pump();

            if (feed.Frames == seen) { Thread.Sleep(1); continue; }
            seen = feed.Frames;
            bool quiet = now >= 9600;
            if (quiet) quietFrames++;
            lock (gpu.Context)
            {
                feed.WithLatest((t, m) => { glass.UpdateBackground(t, rx, ry); glass.PendingFrameId = m.Id; return 0; });
                var c = glass.CompareWithPresented();
                compares++; cmpMs.Add(c.Ms);
                bool stale = now - lastPresent > 2000;
                if (!c.Changed && !stale) continue;
                if (!c.Changed) stalePresents++;
                glass.RenderAndPresent(p);
                presents++; lastPresent = now;
                if (quiet) quietPresents++;
                var g = glass.ReadPresented(gX, gY, 1, 1);
                var cu = glass.ReadPresented(cX, cY, 1, 1);
                var tx = glass.ReadPresented(tX, tY, 320, 44);
                var fl = glass.ReadPresented(fX, fY, 1, 1);
                samples.Add(new Sample(now, step, g[2], cursorOn, cu[2] > 200 && cu[1] < 60 && cu[0] < 60, textOffset, Fnv(tx), flashOn, fl[1] > 200 && fl[2] < 60 && fl[0] < 60));
            }
        }

        // Final state: after everything stopped, what was presented equals the screen.
        PumpFor(600);
        bool finalEqual;
        lock (gpu.Context)
        {
            feed.WithLatest((t, m) => { glass.UpdateBackground(t, rx, ry); return 0; });
            finalEqual = !glass.CompareWithPresented().Changed;
        }

        // Gradient: lag in steps between the helper and what was drawn.
        var gradSamples = samples.Where(s => s.T < 2100).ToList();
        var lags = gradSamples.Select(s => (double)(s.Step - (s.PresentedGray - 60))).ToList();
        var lastGrad = samples.Where(s => s.T < 2200).LastOrDefault();
        rep.Set("gradient.presents", gradSamples.Count);
        rep.Set("gradient.lagSteps.p50", Stats.Pct(lags, 50));
        rep.Set("gradient.lagSteps.p95", Stats.Pct(lags, 95));
        rep.Set("gradient.finalPresentedStep", lastGrad == null ? -1 : lastGrad.PresentedGray - 60);

        // Cursor: delay from each toggle to the first present showing that state.
        var cursorDelays = new JsonArray(); bool cursorOk = true;
        foreach (var (t, on) in cursorToggles)
        {
            var hit = samples.FirstOrDefault(s => s.T >= t && s.CursorShown == on);
            double d = hit == null ? -1 : Math.Round(hit.T - t, 1);
            cursorDelays.Add(d);
            if (d < 0 || d > 100) cursorOk = false;
        }
        rep.Set("cursor.toggles", cursorToggles.Count);
        rep.Set("cursor.delayMs", cursorDelays);

        // Text: every 1 px scroll step should produce a distinct drawn backdrop.
        var textHashes = samples.Where(s => s.T >= 4400 && s.T < 6500).Select(s => s.TextHash).Distinct().Count();
        rep.Set("text.scrollSteps", textSteps.Count);
        rep.Set("text.distinctPresentedStates", textHashes);

        // Transient: shown for one frame; it may or may not be captured, but it
        // must never stay on the glass after it disappeared.
        int flashesSeen = 0; bool ghost = false;
        for (int i = 0; i + 1 < flashEvents.Count; i += 2)
        {
            var (on, off) = (flashEvents[i].T, flashEvents[i + 1].T);
            if (samples.Any(s => s.T >= on && s.T < off + 100 && s.TransientShown)) flashesSeen++;
            if (samples.Any(s => s.T > off + 100 && s.T < off + 600 && s.TransientShown)) ghost = true;
        }
        rep.Set("transient.shown", flashEvents.Count / 2);
        rep.Set("transient.captured", flashesSeen);
        rep.Set("transient.ghostAfter100ms", ghost);

        rep.Set("presents", presents);
        rep.Set("compares", compares);
        rep.Set("stalenessPresents", stalePresents);
        rep.Set("compareMs.p50", Stats.Pct(cmpMs, 50));
        rep.Set("compareMs.p95", Stats.Pct(cmpMs, 95));
        rep.Set("quiet.captureFrames", quietFrames);
        rep.Set("quiet.presents", quietPresents);
        rep.Set("finalPresentedEqualsScreen", finalEqual);

        double lag95 = Stats.Pct(lags, 95);
        rep.Verdict("slow-gradient", lastGrad != null && lastGrad.PresentedGray - 60 == 120 && lag95 <= 2 ? "Passed" : "Failed",
            $"+1 per frame for 120 frames: final drawn step {(lastGrad == null ? -1 : lastGrad.PresentedGray - 60)}/120, lag p95 {lag95} steps");
        rep.Verdict("one-pixel-cursor", cursorOk ? "Passed" : "Failed", $"1 px line toggles drawn within (ms): {cursorDelays.ToJsonString()}");
        rep.Verdict("low-contrast-scroll", textHashes >= textSteps.Count * 0.9 ? "Passed" : "Failed", $"{textHashes} distinct drawn states for {textSteps.Count} one-pixel scroll steps of 4-level contrast text");
        rep.Verdict("transient-no-ghost", !ghost ? "Passed" : "Failed", $"one-frame content captured {flashesSeen}/{flashEvents.Count / 2} times and never left on the glass afterwards");
        rep.Verdict("quiet-no-echo", quietPresents <= 2 ? "Passed" : "Failed", $"quiet 3 s: {quietPresents} presents for {quietFrames} capture frames");
        rep.Verdict("final-state", finalEqual ? "Passed" : "Failed", "after all changes stop, the drawn backdrop equals the current screen exactly");
        rep.Save();
    }

    private static uint Gray(int v) => (uint)(v | (v << 8) | (v << 16));

    private static ulong Fnv(byte[] b)
    {
        ulong h = 1469598103934665603UL;
        for (int i = 0; i < b.Length; i += 4) { h = (h ^ b[i]) * 1099511628211UL; h = (h ^ b[i + 1]) * 1099511628211UL; h = (h ^ b[i + 2]) * 1099511628211UL; }
        return h;
    }
}
