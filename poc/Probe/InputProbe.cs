using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Vortice.Direct3D11;
using Vortice.DXGI;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P02: can clicks on the transparent corners and the shadow of a glass surface
// reach a window of ANOTHER process underneath, while clicks on the glass body
// reach us? Three strategies:
//   H  one window, WM_NCHITTEST returns HTTRANSPARENT outside the shape
//   R  one window with a rounded window region (SetWindowRgn)
//   S  split: a click-through visual window + an invisible input window with a region
// All clicks land on probe-owned windows; the cursor is restored afterwards.
internal static class InputProbe
{
    public static void ClickTarget(string[] a)
    {
        int x = int.Parse(a[1]), y = int.Parse(a[2]), w = int.Parse(a[3]), h = int.Parse(a[4]), ms = int.Parse(a[5]);
        using var t = new TestWindow(x, y, w, h, WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, gdi: true, color: 0x00909090, title: "Atrium click target");
        t.Show();
        Console.WriteLine("READY");
        int reported = 0; var end = Environment.TickCount64 + ms;
        while (Environment.TickCount64 < end)
        {
            Pump();
            if (t.Clicks != reported) { reported = t.Clicks; Console.WriteLine("CLICK " + reported); Console.Out.Flush(); }
            Thread.Sleep(5);
        }
    }

    public static void Run()
    {
        var rep = new Report("p02-input");
        var mon = MonitorFromPoint(new POINT { X = 0, Y = 0 }, MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        using var gpu = Gpu.ForMonitor(mon);

        // Wait (up to 3 minutes) until the user has been idle for 15 s.
        var waitStart = Stopwatch.StartNew();
        while (IdleSeconds() < 15 && waitStart.Elapsed.TotalSeconds < 180) Thread.Sleep(1000);
        double idle = IdleSeconds();
        rep.Set("idleSecondsBeforeClicks", Math.Round(idle, 1));
        if (idle < 15)
        {
            rep.Verdict("input", "Inconclusive", "user was active; clicks were not injected");
            rep.Save();
            return;
        }

        int tx = mi.rcWork.Left + 160, ty = mi.rcWork.Top + 160, tw = 700, th = 420;
        int gw = 420, gh = 160, inset = 22;
        int gx = tx + 140, gy = ty + 120;
        int radius = (gh - 2 * inset) / 2;
        var points = new (string Name, int X, int Y, bool ExpectGlass)[]
        {
            ("body", gx + gw / 2, gy + gh / 2, true),
            ("corner", gx + inset + 6, gy + inset + 6, false),   // inside window rect, outside capsule
            ("shadow", gx + gw / 2, gy + gh - 6, false),        // shadow band below the shape
        };

        var exe = Environment.ProcessPath!;
        var psi = new ProcessStartInfo(exe, $"clicktarget {tx} {ty} {tw} {th} 60000") { RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true };
        using var target = Process.Start(psi)!;
        SafetyNet.Register("click target process", () => { if (!target.HasExited) target.Kill(); });
        target.StandardOutput.ReadLine(); // READY
        int targetClicks = 0;
        var reader = new Thread(() =>
        {
            string? line;
            while ((line = target.StandardOutput.ReadLine()) != null)
                if (line.StartsWith("CLICK ")) Volatile.Write(ref targetClicks, int.Parse(line[6..]));
        }) { IsBackground = true };
        reader.Start();
        GetCursorPos(out var cursor0);
        var fg0 = GetForegroundWindow();
        SafetyNet.Register("cursor position", () => SetCursorPos(cursor0.X, cursor0.Y));

        using var bg = gpu.Device.CreateTexture2D(new Texture2DDescription
        {
            Width = 4, Height = 4, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
        });
        var p = GlassParams.Frosted(gw, gh, inset);
        p.Shadow = 0.6f;

        var results = new JsonArray();
        foreach (var method in new[] { "H", "R", "S" })
        {
            uint visEx = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
            if (method == "S") visEx |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
            using var vis = new TestWindow(gx, gy, gw, gh, visEx, gdi: false, title: "Atrium input glass " + method);
            TestWindow? input = null;
            Func<int, int, bool> inShape = (lx, ly) =>
            {
                float cx = lx - gw / 2f, cy = ly - gh / 2f, bx = gw / 2f - inset, by = gh / 2f - inset;
                float qx = Math.Abs(cx) - bx + radius, qy = Math.Abs(cy) - by + radius;
                float d = MathF.Sqrt(MathF.Max(qx, 0) * MathF.Max(qx, 0) + MathF.Max(qy, 0) * MathF.Max(qy, 0)) + MathF.Min(MathF.Max(qx, qy), 0) - radius;
                return d <= 0;
            };
            if (method == "H") vis.HitTest = (lx, ly) => inShape(lx, ly) ? HTCLIENT : HTTRANSPARENT;
            if (method == "R") SetWindowRgn(vis.Hwnd, CreateRoundRectRgn(inset, inset, gw - inset + 1, gh - inset + 1, radius * 2, radius * 2), true);
            if (method == "S")
            {
                input = new TestWindow(gx, gy, gw, gh, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, gdi: false, title: "Atrium input region");
                SetWindowRgn(input.Hwnd, CreateRoundRectRgn(inset, inset, gw - inset + 1, gh - inset + 1, radius * 2, radius * 2), true);
            }
            if (method == "S") SetLayeredWindowAttributes(vis.Hwnd, 0, 255, 2 /* LWA_ALPHA */);
            using var glass = new GlassRenderer(gpu, vis.Hwnd, gw, gh);
            lock (gpu.Context) { glass.UpdateBackground(bg, 0, 0); glass.RenderAndPresent(p); glass.RenderAndPresent(p); }
            vis.Show();
            input?.Show();
            PumpFor(400);
            DwmFlush();
            // Is the shadow still drawn (not clipped by a region)? Compare the
            // shadow band with the plain target colour 0x909090.
            var band = WdaProbe.Gdi(gx + gw / 2 - 40, gy + gh - 8, 80, 4);
            double shadowLuma = Pixels.Mean(band);

            var clicks = new JsonArray();
            foreach (var pt in points)
            {
                int targetBefore = Volatile.Read(ref targetClicks);
                int glassBefore = vis.Clicks + (input?.Clicks ?? 0);
                var hit = WindowFromPoint(new POINT { X = pt.X, Y = pt.Y });
                Click(pt.X, pt.Y);
                PumpFor(250);
                bool glassGot = vis.Clicks + (input?.Clicks ?? 0) > glassBefore;
                bool targetGot = Volatile.Read(ref targetClicks) > targetBefore;
                clicks.Add(new JsonObject
                {
                    ["point"] = pt.Name,
                    ["expectGlass"] = pt.ExpectGlass,
                    ["glassReceived"] = glassGot,
                    ["otherProcessReceived"] = targetGot,
                    ["windowFromPointIsOurs"] = hit == vis.Hwnd || (input != null && hit == input.Hwnd),
                    ["correct"] = glassGot == pt.ExpectGlass && targetGot == !pt.ExpectGlass,
                });
            }
            results.Add(new JsonObject { ["method"] = method, ["shadowBandLuma"] = Math.Round(shadowLuma, 1), ["clicks"] = clicks });
            input?.Dispose();
            PumpFor(100);
        }
        SetCursorPos(cursor0.X, cursor0.Y);
        target.Kill();
        SafetyNet.Unregister("click target process");
        rep.Set("methods", results);
        rep.Set("note", "H = HTTRANSPARENT from WM_NCHITTEST; R = window region on the visual window; S = layered click-through visual window + region input window");
        rep.Set("foregroundUnchanged", GetForegroundWindow() == fg0);

        foreach (var r in results)
        {
            var m = (string)r!["method"]!;
            bool allCorrect = r["clicks"]!.AsArray().All(c => (bool)c!["correct"]!);
            double luma = (double)r["shadowBandLuma"]!;
            bool shadowKept = luma < 0x90 - 12;
            rep.Verdict("method-" + m, allCorrect ? "Passed" : "Failed",
                $"body/corner/shadow clicks routed correctly: {allCorrect}; shadow drawn: {shadowKept} (band luma {luma}, target 144)");
        }
        rep.Verdict("no-focus-steal", GetForegroundWindow() == fg0 ? "Passed" : "Failed", "the user's foreground window is unchanged after all clicks");
        rep.Save();
    }
}
