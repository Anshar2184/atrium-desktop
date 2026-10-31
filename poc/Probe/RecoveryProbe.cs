using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P07b: the takeover transaction with the native guard (native/guard). The core
// writes a recovery state file before every step, publishes a heartbeat, and the
// guard restores from that file on core exit, crash, hang, startup failure, or
// at the next start if both died. Scenarios run with the user's auto-hide
// setting temporarily OFF (the common configuration); the real setting is put
// back at the end.
internal static class RecoveryProbe
{
    [DllImport("kernel32.dll")] private static extern bool GetProcessTimes(IntPtr h, out long c, out long e, out long k, out long u);

    private static void Write(string path, Dictionary<string, string> s)
    {
        var tmp = path + ".tmp";
        File.WriteAllLines(tmp, s.Select(kv => kv.Key + "=" + kv.Value));
        File.Move(tmp, path, overwrite: true);
    }

    public static Dictionary<string, string> Read(string path) =>
        File.Exists(path) ? File.ReadAllLines(path).Where(l => l.Contains('=')).ToDictionary(l => l[..l.IndexOf('=')], l => l[(l.IndexOf('=') + 1)..]) : new();

    // Child process: the core. args: txcore <limit> <stateFile> <guardExe> <mode>
    public static void Core(string[] a)
    {
        string state = a[2], guardExe = a[3], mode = a[4];
        var me = Process.GetCurrentProcess();
        GetProcessTimes(me.Handle, out long created, out _, out _, out _);
        var s = new Dictionary<string, string>
        {
            ["version"] = "1", ["phase"] = "prepared", ["corePid"] = me.Id.ToString(), ["coreStart"] = created.ToString(),
            ["userAutoHide"] = TaskbarProbe.GetAutoHide() ? "1" : "0", ["autoHideChanged"] = "0",
            ["taskbarVisible"] = TaskbarProbe.Taskbars().Any(IsWindowVisible) ? "1" : "0",
        };
        Write(state, s);
        using var map = MemoryMappedFile.CreateNew($"Local\\AtriumHeartbeat-{me.Id}", 8);
        using var beat = map.CreateViewAccessor(0, 8);
        // The guard inherits our stdout, so its lines reach the test even after we die.
        var guard = Process.Start(new ProcessStartInfo(guardExe, $"--watch {me.Id} \"{state}\"") { UseShellExecute = false })!;
        Console.WriteLine("GUARD " + guard.Id); Console.Out.Flush();
        Thread.Sleep(300);

        if (!TaskbarProbe.GetAutoHide())
        {
            s["autoHideChanged"] = "1"; s["phase"] = "autohide-changed"; Write(state, s);
            TaskbarProbe.SetAutoHide(true);
        }
        if (mode == "crash-after-autohide") { Console.WriteLine("CRASHING"); Console.Out.Flush(); Environment.FailFast("injected startup failure"); }

        foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_HIDE);
        s["phase"] = "active"; Write(state, s);
        Console.WriteLine("ACTIVE"); Console.Out.Flush();

        string? cmd = null;
        new Thread(() => { string? l; while ((l = Console.ReadLine()) != null) Volatile.Write(ref cmd, l); }) { IsBackground = true }.Start();
        ulong n = 0;
        var end = Environment.TickCount64 + 60000;
        while (Environment.TickCount64 < end)
        {
            Pump();
            beat.Write(0, ++n);
            var c = Interlocked.Exchange(ref cmd, null);
            if (c == "HANG") { Console.WriteLine("HANGING"); Console.Out.Flush(); Thread.Sleep(Timeout.Infinite); }
            if (c == "EXIT") break;
            if (guard.HasExited)
            {
                // The guard is gone: restore ourselves with the same native code path.
                Console.WriteLine("GUARD-LOST"); Console.Out.Flush();
                Process.Start(new ProcessStartInfo(guardExe, $"--restore \"{state}\"") { UseShellExecute = false })!.WaitForExit();
                break;
            }
            Thread.Sleep(10);
        }
        // Graceful exit: restore through the guard executable (the guard may be
        // restoring at the same moment; the restore is idempotent and serialized).
        Process.Start(new ProcessStartInfo(guardExe, $"--restore \"{state}\"") { UseShellExecute = false })!.WaitForExit();
        Console.WriteLine("CORE-EXIT"); Console.Out.Flush();
    }

    private sealed record Snap(bool AutoHide, bool Visible, string WorkArea)
    {
        public static Snap Now() => new(TaskbarProbe.GetAutoHide(), TaskbarProbe.Taskbars().Any(IsWindowVisible), Native.WorkArea().ToString());
    }

    public static void Run(string guardExe)
    {
        var rep = new Report("p07b-recovery-transaction");
        guardExe = Path.GetFullPath(guardExe);
        rep.Set("guard", Path.GetFileName(guardExe));
        var real = Snap.Now();
        rep.Set("realUserState", $"{real}");
        SafetyNet.Register("real taskbar state", () =>
        {
            foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_SHOWNA);
            TaskbarProbe.SetAutoHide(real.AutoHide);
        });
        var exe = Environment.ProcessPath!;
        var dir = Path.Combine(Path.GetTempPath(), "atrium-recovery-test");
        Directory.CreateDirectory(dir);

        var results = new JsonArray();
        void Scenario(string name, string mode, Action<Process, Process?, string> inject, string expectRestoredBy)
        {
            // Simulated user configuration: auto-hide OFF, taskbar visible.
            TaskbarProbe.SetAutoHide(false);
            foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_SHOWNA);
            PumpFor(1500);
            var before = Snap.Now();
            var state = Path.Combine(dir, name + ".state");
            File.Delete(state);
            var lines = new List<string>();
            var core = Process.Start(new ProcessStartInfo(exe, $"txcore 70 \"{state}\" \"{guardExe}\" {mode}")
            { RedirectStandardOutput = true, RedirectStandardInput = true, UseShellExecute = false, CreateNoWindow = true })!;
            new Thread(() => { string? l; while ((l = core.StandardOutput.ReadLine()) != null) lock (lines) lines.Add(l); }) { IsBackground = true }.Start();
            SafetyNet.Register("core " + name, () => { try { if (!core.HasExited) core.Kill(); } catch { } });
            bool Has(string p) { lock (lines) return lines.Any(l => l.StartsWith(p)); }
            var wait = Stopwatch.StartNew();
            while (!Has(mode == "crash-after-autohide" ? "CRASHING" : "ACTIVE") && wait.ElapsedMilliseconds < 8000) PumpFor(10);
            Process? guard = null;
            lock (lines) { var g = lines.FirstOrDefault(l => l.StartsWith("GUARD ")); if (g != null) guard = Process.GetProcessById(int.Parse(g[6..])); }
            var during = Snap.Now();
            PumpFor(500);

            var sw = Stopwatch.StartNew();
            inject(core, guard, state);
            double restoredMs = -1;
            while (sw.ElapsedMilliseconds < 12000)
            {
                var n = Snap.Now();
                if (n == before && Read(state).GetValueOrDefault("phase") == "restored") { restoredMs = sw.Elapsed.TotalMilliseconds; break; }
                PumpFor(10);
            }
            PumpFor(1200);
            var after = Snap.Now();
            var st = Read(state);
            bool coreGone = core.WaitForExit(3000);
            SafetyNet.Unregister("core " + name);
            string log; lock (lines) log = string.Join(" | ", lines.Where(l => l.StartsWith("RESTORE") || l.StartsWith("TERMINATED") || l.StartsWith("GUARD-LOST") || l.StartsWith("WATCH")));
            bool ok = restoredMs >= 0 && after == before && st.GetValueOrDefault("phase") == "restored" &&
                      (expectRestoredBy == "*" || st.GetValueOrDefault("restoredBy") == expectRestoredBy) && coreGone;
            var node = new JsonObject
            {
                ["scenario"] = name, ["before"] = before.ToString(), ["duringTakeover"] = during.ToString(), ["after"] = after.ToString(),
                ["restoredMs"] = Math.Round(restoredMs, 1), ["restoredBy"] = st.GetValueOrDefault("restoredBy"), ["log"] = log,
            };
            results.Add(node);
            rep.Verdict(name, ok ? "Passed" : "Failed",
                $"state restored to the user's configuration in {Math.Round(restoredMs)} ms by {st.GetValueOrDefault("restoredBy")}; after == before: {after == before}; core exited: {coreGone}");
        }

        Scenario("T1-kill-core", "active", (c, g, s) => c.Kill(), "core-exit");
        Scenario("T2-core-hang", "active", (c, g, s) => c.StandardInput.WriteLine("HANG"), "core-hang");
        Scenario("T3-startup-failure", "crash-after-autohide", (c, g, s) => { }, "*");
        Scenario("T4-reentrant-restore", "active", (c, g, s) =>
        {
            c.StandardInput.WriteLine("EXIT"); // core restores while the guard also restores on exit
            c.WaitForExit(5000);
            for (int i = 0; i < 2; i++)
                Process.Start(new ProcessStartInfo(guardExe, $"--restore \"{s}\"") { UseShellExecute = false, CreateNoWindow = true })!.WaitForExit();
        }, "*");
        Scenario("T5-guard-killed", "active", (c, g, s) =>
        {
            g?.Kill();
            var t = Stopwatch.StartNew();
            while (!c.HasExited && t.ElapsedMilliseconds < 5000) Thread.Sleep(10);
        }, "manual");
        Scenario("T6-both-killed-next-start", "active", (c, g, s) =>
        {
            g?.Kill(); c.Kill();
            Thread.Sleep(800);
            // Simulates the next login: run the restore from the leftover state.
            Process.Start(new ProcessStartInfo(guardExe, $"--restore \"{s}\"") { UseShellExecute = false, CreateNoWindow = true })!.WaitForExit();
        }, "manual");
        rep.Set("scenarios", results);

        // Put the user's real configuration back and check it.
        foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_SHOWNA);
        TaskbarProbe.SetAutoHide(real.AutoHide);
        PumpFor(1500);
        var final = Snap.Now();
        SafetyNet.Unregister("real taskbar state");
        rep.Set("final", final.ToString());
        rep.Verdict("user-state-restored", final == real ? "Passed" : "Failed", $"real user configuration before/after: {real} / {final}");
        rep.Save();
    }
}
