using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P03 + P07: register an AppBar, hide the Explorer taskbar(s), check work area
// and whether Explorer fights back, then kill the "core" process hard and
// measure how fast an independent guard process restores everything.
internal static class TaskbarProbe
{
    [StructLayout(LayoutKind.Sequential)]
    private struct APPBARDATA
    {
        public uint cbSize;
        public IntPtr hWnd;
        public uint uCallbackMessage;
        public uint uEdge;
        public RECT rc;
        public IntPtr lParam;
    }

    [DllImport("shell32.dll")] private static extern UIntPtr SHAppBarMessage(uint msg, ref APPBARDATA data);
    private const uint ABM_NEW = 0, ABM_REMOVE = 1, ABM_QUERYPOS = 2, ABM_SETPOS = 3, ABM_GETSTATE = 4, ABM_SETSTATE = 10, ABE_BOTTOM = 3;

    public static void SetAutoHide(bool on)
    {
        var abd = new APPBARDATA { cbSize = (uint)Marshal.SizeOf<APPBARDATA>(), hWnd = FindWindow("Shell_TrayWnd", null), lParam = new IntPtr(on ? 1 : 0) };
        SHAppBarMessage(ABM_SETSTATE, ref abd);
    }

    public static bool GetAutoHide()
    {
        var abd = new APPBARDATA { cbSize = (uint)Marshal.SizeOf<APPBARDATA>() };
        return ((uint)SHAppBarMessage(ABM_GETSTATE, ref abd) & 1) != 0;
    }

    // Variant for the common configuration: taskbar NOT auto-hidden. Temporarily
    // turns auto-hide off, checks whether hiding the taskbar leaves its reserved
    // strip behind, tries the ManagedShell approach (switch Explorer to auto-hide
    // while hidden) and always puts the user's setting back.
    public static void RunNoAutoHide()
    {
        var rep = new Report("p03b-taskbar-not-autohide");
        bool userAuto = GetAutoHide();
        rep.Set("userAutoHide", userAuto);
        SafetyNet.Register("user auto-hide setting", () => { SetAutoHide(userAuto); foreach (var t in Taskbars()) ShowWindow(t, SW_SHOWNA); });
        SetAutoHide(false);
        PumpFor(1500);
        var pinned = Snapshot();
        rep.Set("notAutoHide.before", pinned);

        foreach (var t in Taskbars()) ShowWindow(t, SW_HIDE);
        PumpFor(1200);
        var hidden = Snapshot();
        rep.Set("notAutoHide.hiddenOnly", hidden);
        bool gapLeft = (string)hidden["workArea"]! == (string)pinned["workArea"]! && (string)pinned["workArea"]! != WholeMonitor();
        rep.Verdict("hide-leaves-reserved-strip", gapLeft ? "Passed" : "Failed",
            $"hiding the pinned taskbar keeps its work-area reservation ({hidden["workArea"]}); an empty strip remains unless auto-hide is used");

        // ManagedShell approach: put Explorer into auto-hide while it is hidden.
        SetAutoHide(true);
        PumpFor(1500);
        var hiddenAuto = Snapshot();
        rep.Set("notAutoHide.hiddenPlusAutoHide", hiddenAuto);
        rep.Verdict("autohide-releases-strip", (string)hiddenAuto["workArea"]! == WholeMonitor() && !Taskbars().Any(IsWindowVisible) ? "Passed" : "Failed",
            $"with Explorer switched to auto-hide the work area is {hiddenAuto["workArea"]} and the taskbar stays hidden");

        // Put everything back exactly.
        foreach (var t in Taskbars()) ShowWindow(t, SW_SHOWNA);
        SetAutoHide(false);
        PumpFor(1200);
        var back = Snapshot();
        rep.Verdict("restore-pinned", JsonNode.DeepEquals(back, pinned) ? "Passed" : "Failed", "pinned state restored exactly");
        SetAutoHide(userAuto);
        PumpFor(1500);
        foreach (var t in Taskbars()) if (!IsWindowVisible(t)) ShowWindow(t, SW_SHOWNA);
        SafetyNet.Unregister("user auto-hide setting");
        rep.Set("final", Snapshot());
        rep.Set("finalAutoHideMatchesUser", GetAutoHide() == userAuto);
        rep.Save();
    }

    private static string WholeMonitor()
    {
        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY), ref mi);
        return mi.rcMonitor.ToString();
    }

    public static List<IntPtr> Taskbars()
    {
        var list = new List<IntPtr>();
        var main = FindWindow("Shell_TrayWnd", null);
        if (main != IntPtr.Zero) list.Add(main);
        IntPtr h = IntPtr.Zero;
        while ((h = FindWindowEx(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) list.Add(h);
        return list;
    }

    public static JsonObject Snapshot()
    {
        var abd = new APPBARDATA { cbSize = (uint)Marshal.SizeOf<APPBARDATA>() };
        var state = (uint)SHAppBarMessage(ABM_GETSTATE, ref abd);
        var bars = new JsonArray();
        foreach (var t in Taskbars())
        {
            GetWindowRect(t, out var r);
            bars.Add(new JsonObject { ["class"] = ClassOf(t), ["visible"] = IsWindowVisible(t), ["rect"] = r.ToString() });
        }
        return new JsonObject { ["autoHide"] = (state & 1) != 0, ["workArea"] = WorkArea().ToString(), ["taskbars"] = bars };
    }

    // Shows every taskbar that was visible in `before` (idempotent).
    public static void Restore(JsonObject before)
    {
        var wanted = before["taskbars"]!.AsArray().Where(b => (bool)b!["visible"]!).Select(b => (string)b!["class"]!).ToHashSet();
        foreach (var t in Taskbars())
            if (wanted.Contains(ClassOf(t)) && !IsWindowVisible(t)) ShowWindow(t, SW_SHOWNA);
    }

    // Child process: the "core". Registers a bottom AppBar, hides taskbars, then idles.
    public static void Core(string[] a)
    {
        int height = int.Parse(a[2]); // a[1] is the watchdog limit
        var mon = MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY);
        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(mon, ref mi);
        using var bar = new TestWindow(mi.rcMonitor.Left, mi.rcMonitor.Bottom - height, mi.rcMonitor.Width, height,
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, gdi: true, color: 0x00B0A090, title: "Atrium probe bar");
        var abd = new APPBARDATA { cbSize = (uint)Marshal.SizeOf<APPBARDATA>(), hWnd = bar.Hwnd, uCallbackMessage = WM_USER + 77 };
        SHAppBarMessage(ABM_NEW, ref abd);
        abd.uEdge = ABE_BOTTOM;
        abd.rc = new RECT { Left = mi.rcMonitor.Left, Right = mi.rcMonitor.Right, Top = mi.rcMonitor.Bottom - height, Bottom = mi.rcMonitor.Bottom };
        SHAppBarMessage(ABM_QUERYPOS, ref abd);
        abd.rc.Top = abd.rc.Bottom - height;
        SHAppBarMessage(ABM_SETPOS, ref abd);
        SetWindowPos(bar.Hwnd, HWND_TOPMOST, abd.rc.Left, abd.rc.Top, abd.rc.Width, abd.rc.Height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        bar.Show();
        foreach (var t in Taskbars()) ShowWindow(t, SW_HIDE);
        Console.WriteLine($"HIDDEN appbar={abd.rc}");
        Console.Out.Flush();
        bool graceful = false;
        var input = new Thread(() => { if (Console.ReadLine() == "EXIT") graceful = true; }) { IsBackground = true };
        input.Start();
        var end = Environment.TickCount64 + 60000;
        while (!graceful && Environment.TickCount64 < end) { Pump(); Thread.Sleep(10); }
        // Graceful exit path: restore first, then remove the AppBar.
        foreach (var t in Taskbars()) ShowWindow(t, SW_SHOWNA);
        SHAppBarMessage(ABM_REMOVE, ref abd);
        Console.WriteLine("RESTORED");
    }

    // Child process: the "guard". Waits on the core process handle, then restores.
    public static void Guard(string[] a)
    {
        int pid = int.Parse(a[1]);
        var before = (JsonObject)JsonNode.Parse(File.ReadAllText(a[2]))!;
        using var core = Process.GetProcessById(pid);
        Console.WriteLine("GUARDING");
        Console.Out.Flush();
        core.WaitForExit();
        var sw = Stopwatch.StartNew();
        Restore(before);
        Console.WriteLine($"GUARD_RESTORED {sw.Elapsed.TotalMilliseconds:F1}ms");
        Console.Out.Flush();
    }

    public static void Run()
    {
        var rep = new Report("p03-p07-taskbar");
        var before = Snapshot();
        rep.Set("before", before.DeepClone());
        var snapPath = Path.Combine(Path.GetTempPath(), "atrium-taskbar-before.json");
        File.WriteAllText(snapPath, before.ToJsonString());
        SafetyNet.Register("explorer taskbar visibility", () => Restore(before));
        var exe = Environment.ProcessPath!;
        const int height = 110;

        Process StartChild(string args) => Process.Start(new ProcessStartInfo(exe, args)
        { RedirectStandardOutput = true, RedirectStandardInput = true, UseShellExecute = false, CreateNoWindow = true })!;

        // Round 1: graceful exit.
        using (var core = StartChild($"tbcore 70 {height}"))
        {
            SafetyNet.Register("core1", () => { if (!core.HasExited) core.Kill(); });
            rep.Set("r1.core", core.StandardOutput.ReadLine());
            PumpFor(800);
            var during = Snapshot();
            rep.Set("r1.during", during);
            // Does Explorer re-show the hidden taskbar on its own within 3 s?
            PumpFor(3000);
            rep.Set("r1.after3s.taskbarVisible", Taskbars().Any(IsWindowVisible));
            core.StandardInput.WriteLine("EXIT");
            core.WaitForExit(5000);
            SafetyNet.Unregister("core1");
            PumpFor(800);
            var after = Snapshot();
            rep.Set("r1.after", after);
            rep.Verdict("appbar-reserves-work-area", (string)during["workArea"]! != (string)before["workArea"]! ? "Passed" : "Failed",
                $"work area {before["workArea"]} -> {during["workArea"]} while the AppBar is registered");
            rep.Verdict("hide-taskbar", !during["taskbars"]!.AsArray().Any(b => (bool)b!["visible"]!) ? "Passed" : "Failed", "Explorer taskbar windows hidden with ShowWindow(SW_HIDE)");
            rep.Verdict("explorer-does-not-fight", !(bool)rep.Root["r1.after3s.taskbarVisible"]! ? "Passed" : "Failed", "taskbar stays hidden for 3 s without re-hide loops");
            rep.Verdict("graceful-restore", JsonNode.DeepEquals(after, before) ? "Passed" : "Failed", "state after graceful exit equals the snapshot");
        }

        // Round 2: hard kill of the core while a guard watches it.
        using (var core = StartChild($"tbcore 70 {height}"))
        {
            SafetyNet.Register("core2", () => { if (!core.HasExited) core.Kill(); });
            core.StandardOutput.ReadLine();
            using var guard = StartChild($"tbguard {core.Id} \"{snapPath}\"");
            SafetyNet.Register("guard", () => { if (!guard.HasExited) guard.Kill(); });
            guard.StandardOutput.ReadLine(); // GUARDING
            PumpFor(800);
            var sw = Stopwatch.StartNew();
            core.Kill(); // no cleanup code runs in the core
            string? g = guard.StandardOutput.ReadLine();
            double guardMs = sw.Elapsed.TotalMilliseconds;
            // Poll until taskbar visible again and the work area is back.
            double visibleMs = -1, workAreaMs = -1;
            while (sw.ElapsedMilliseconds < 10000 && (visibleMs < 0 || workAreaMs < 0))
            {
                if (visibleMs < 0 && Taskbars().Any(IsWindowVisible)) visibleMs = sw.Elapsed.TotalMilliseconds;
                if (workAreaMs < 0 && WorkArea().ToString() == (string)before["workArea"]!) workAreaMs = sw.Elapsed.TotalMilliseconds;
                Pump(); Thread.Sleep(10);
            }
            SafetyNet.Unregister("core2");
            SafetyNet.Unregister("guard");
            PumpFor(500);
            var after = Snapshot();
            rep.Set("r2.guard", g);
            rep.Set("r2.killToGuardRestoreMs", Math.Round(guardMs, 1));
            rep.Set("r2.killToTaskbarVisibleMs", Math.Round(visibleMs, 1));
            rep.Set("r2.killToWorkAreaRestoredMs", Math.Round(workAreaMs, 1));
            rep.Set("r2.after", after);
            rep.Verdict("crash-recovery", visibleMs >= 0 && visibleMs < 2000 && workAreaMs >= 0 && JsonNode.DeepEquals(after, before) ? "Passed" : "Failed",
                $"hard kill -> taskbar visible after {Math.Round(visibleMs)} ms, work area restored after {Math.Round(workAreaMs)} ms (target < 2000 ms), final state equals snapshot: {JsonNode.DeepEquals(after, before)}");
        }
        Restore(before);
        SafetyNet.Unregister("explorer taskbar visibility");
        rep.Set("final", Snapshot());
        rep.Save();
    }
}
