using System.Diagnostics;
using System.Text.Json.Nodes;
using Vortice.Direct3D11;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P01h: real-time per-frame timing of the full glass path under a backdrop that
// changes every frame. Each presented frame carries the capture frame's ID and
// timestamps (source, arrival, copied) delivered together with its texture, then
// update / compare / draw / GPU completion / Present return. A helper under the
// glass paints a frame counter; the counter found in the drawn backdrop gives
// the content lag in frames.
internal static class PerfProbe
{
    public static void Run(string tag)
    {
        var rep = new Report("p01h-realtime-perf" + (tag.Length > 0 ? "-" + tag : ""));
        var mon = MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        int w = 1500, h = 196, gx = mi.rcMonitor.Left + (mi.rcMonitor.Width - w) / 2, gy = mi.rcMonitor.Bottom - h - 28;
        int rx = gx - mi.rcMonitor.Left, ry = gy - mi.rcMonitor.Top, P = GlassRenderer.Pad;
        using var gpu = Gpu.ForMonitor(mon);
        rep.Set("adapter", gpu.AdapterName);
        using var feed = new CaptureFeed(gpu, Wgc.ForMonitor(mon), tryBorderless: true);
        feed.WaitFrames(1, 2000);

        var exH = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT;
        using var helper = new TestWindow(gx + 300, gy + 50, 300, 90, exH, gdi: true, color: 0, title: "Atrium perf counter");
        helper.Show();
        using var win = new TestWindow(gx, gy, w, h, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, gdi: false, title: "Atrium perf glass");
        SetWindowDisplayAffinity(win.Hwnd, WDA_EXCLUDEFROMCAPTURE);
        using var glass = new GlassRenderer(gpu, win.Hwnd, w, h);
        var p = GlassParams.Liquid(w, h, 26);
        win.Show();
        PumpFor(800);
        using var ev = gpu.Device.CreateQuery(new QueryDescription(QueryType.Event));

        var srcToArrive = new List<double>(); var arriveToCopied = new List<double>(); var copiedToStart = new List<double>();
        var updateMs = new List<double>(); var compareMs = new List<double>(); var drawCpu = new List<double>(); var gpuDone = new List<double>();
        var presentMs = new List<double>(); var srcToPresent = new List<double>(); var frameWork = new List<double>(); var lag = new List<double>(); var arriveToPresent = new List<double>();
        int counter = 0, presents = 0, skipped = 0;
        long lastId = 0;
        var sw = Stopwatch.StartNew();
        long nextPaint = 0;
        while (sw.ElapsedMilliseconds < 10000)
        {
            long now = sw.ElapsedMilliseconds;
            if (now >= nextPaint)
            {
                counter++;
                helper.GdiColor = (uint)(((counter & 0xFF) << 16) | 0x00004060); // blue channel = counter
                InvalidateRect(helper.Hwnd, IntPtr.Zero, false); UpdateWindow(helper.Hwnd);
                nextPaint = now + 16;
            }
            Pump();
            if (feed.Frames == (int)lastId) { Thread.Sleep(0); continue; }

            lock (gpu.Context)
            {
                long start = FrameMeta.Now();
                FrameMeta? meta = null;
                var t = Stopwatch.StartNew();
                feed.WithLatest((tex, m) => { meta = m; glass.UpdateBackground(tex, rx, ry); glass.PendingFrameId = m.Id; return 0; });
                if (meta == null || meta.Id == lastId) continue;
                lastId = meta.Id;
                double tUpd = t.Elapsed.TotalMilliseconds;
                var c = glass.CompareWithPresented();
                if (!c.Changed) { skipped++; continue; }
                t.Restart();
                glass.Draw(p);
                double tDraw = t.Elapsed.TotalMilliseconds;
                gpu.Context.End(ev);
                gpu.Context.Flush();
                while (!gpu.Context.GetData(ev, AsyncGetDataFlags.None, out int _)) Thread.SpinWait(20);
                double tGpu = t.Elapsed.TotalMilliseconds;
                t.Restart();
                glass.PresentDrawn();
                double tPresent = t.Elapsed.TotalMilliseconds;
                long end = FrameMeta.Now();
                presents++;

                srcToArrive.Add((meta.ArrivalTime - meta.SourceTime) / 1e4);
                arriveToCopied.Add((meta.CopiedTime - meta.ArrivalTime) / 1e4);
                copiedToStart.Add((start - meta.CopiedTime) / 1e4);
                updateMs.Add(tUpd); compareMs.Add(c.Ms); drawCpu.Add(tDraw); gpuDone.Add(tGpu); presentMs.Add(tPresent);
                frameWork.Add((end - start) / 1e4);
                srcToPresent.Add((end - meta.SourceTime) / 1e4);
                arriveToPresent.Add((end - meta.ArrivalTime) / 1e4);
                var px = glass.ReadPresented(gx + 450 - gx + P, gy + 95 - gy + P, 1, 1);
                lag.Add(((counter & 0xFF) - px[0] + 256) % 256);
            }
        }

        void Dist(string name, List<double> v)
        {
            rep.Set(name, new JsonObject { ["p50"] = Stats.Pct(v, 50), ["p95"] = Stats.Pct(v, 95), ["p99"] = Stats.Pct(v, 99), ["max"] = Stats.Pct(v, 100) });
        }
        rep.Set("presents", presents);
        rep.Set("skippedUnchanged", skipped);
        rep.Set("presentsPerSecond", Math.Round(presents / 10.0, 1));
        Dist("ms.sourceToArrival", srcToArrive);
        Dist("ms.arrivalToCopied", arriveToCopied);
        Dist("ms.copiedToRenderStart", copiedToStart);
        Dist("ms.update(roiCopy+mips)", updateMs);
        Dist("ms.compare", compareMs);
        Dist("ms.drawCpu", drawCpu);
        Dist("ms.drawToGpuDone", gpuDone);
        Dist("ms.presentCall", presentMs);
        Dist("ms.frameWork(update..presentReturn)", frameWork);
        Dist("ms.sourceToPresentReturn", srcToPresent);
        Dist("ms.arrivalToPresentReturn", arriveToPresent);
        Dist("frames.contentLag", lag);
        rep.Set("note", "SystemRelativeTime of WGC frames is ~10-12 ms LATER than the callback arrival on this machine, i.e. it behaves like the composition's target vblank, not a render-complete time, so source-based latencies are negative and not used for verdicts. Present return is not photon time: DWM shows the frame at a later vblank (one more refresh period is a reasonable estimate). drawToGpuDone is measured by waiting on an event query right after the draw.");

        double work95 = Stats.Pct(frameWork, 95), lag95 = Stats.Pct(lag, 95), s2p95 = Stats.Pct(arriveToPresent, 95);
        rep.Verdict("frame-work-p95", work95 <= 3 ? "Passed" : "Failed", $"per-frame work from backdrop update to Present return: p95 {work95} ms, p99 {Stats.Pct(frameWork, 99)} ms (budget 3 ms)");
        rep.Verdict("arrival-to-present-p95", s2p95 <= 16.7 ? "Passed" : "Failed", $"capture callback arrival to Present return: p50 {Stats.Pct(arriveToPresent, 50)} ms, p95 {s2p95} ms, max {Stats.Pct(arriveToPresent, 100)} ms (budget one 60 Hz period)");
        rep.Verdict("content-lag", lag95 <= 2 ? "Passed" : "Failed", $"drawn backdrop lags the live counter by p50 {Stats.Pct(lag, 50)} / p95 {lag95} frames at present time");
        rep.Save();
    }
}
