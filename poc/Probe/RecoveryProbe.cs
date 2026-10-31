using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P07c: the takeover transaction (protocol v2) with the native guard
// (native/guard). The core waits for the guard's Ready handshake, then runs
// three takeover steps (auto-hide, a real bottom AppBar, hiding the taskbar),
// each under the takeover lock and only while its right is not revoked. Fault
// scenarios cover kills, hangs, startup failures and stalls, guard init
// failure, Explorer missing during restore, a restore step that does not take
// effect and a takeover lock held past its timeout. Scenarios run with the
// user's auto-hide setting temporarily OFF; the real setting is put back at the end.
internal static class RecoveryProbe
{
    [DllImport("kernel32.dll")] private static extern bool GetProcessTimes(IntPtr h, out long c, out long e, out long k, out long u);

    private const string LockName = "Local\\AtriumTaskbarRestore";
    private const int BarHeight = 56;

    private static void Write(string path, Dictionary<string, string> s)
    {
        var tmp = path + ".tmp." + Environment.ProcessId;
        File.WriteAllLines(tmp, s.Select(kv => kv.Key + "=" + kv.Value));
        File.Move(tmp, path, overwrite: true);
    }

    public static Dictionary<string, string> Read(string path) =>
        File.Exists(path) ? File.ReadAllLines(path).Where(l => l.Contains('=')).ToDictionary(l => l[..l.IndexOf('=')], l => l[(l.IndexOf('=') + 1)..]) : new();

    private static string Rect(RECT r) => $"{r.Left},{r.Top},{r.Right},{r.Bottom}";

    // The takeover lock shared with the guard; abandoned counts as acquired.
    private static Mutex? TakeLock()
    {
        var m = new Mutex(false, LockName);
        try { if (m.WaitOne(10000)) return m; }
        catch (AbandonedMutexException) { return m; }
        m.Dispose();
        return null;
    }

    private static void Out(string line) { Console.WriteLine(line); Console.Out.Flush(); }

    // Child process: the core. args: txcore <limit> <stateFile> <guardExe> <mode> <fault>
    // mode: active | crash-after:<step> | pause-before:<step> | pause-inside:<step>
    public static void Core(string[] a)
    {
        string state = a[2], guardExe = a[3], mode = a[4], fault = a.Length > 5 && a[5] != "-" ? a[5] : "";
        var me = Process.GetCurrentProcess();
        GetProcessTimes(me.Handle, out long created, out _, out _, out _);
        var txId = (ulong)Random.Shared.NextInt64(1, long.MaxValue);
        using var map = MemoryMappedFile.CreateNew($"Local\\AtriumTx-{me.Id}", 32);
        using var ctl = map.CreateViewAccessor(0, 32);
        ctl.Write(8, txId);
        ulong n = 0;
        void Beat() => ctl.Write(0, ++n);
        bool Revoked() => ctl.ReadInt32(20) != 0;

        var s = new Dictionary<string, string>
        {
            ["version"] = "2", ["phase"] = "prepared", ["txId"] = txId.ToString(), ["corePid"] = me.Id.ToString(), ["coreStart"] = created.ToString(),
            ["userAutoHide"] = TaskbarProbe.GetAutoHide() ? "1" : "0", ["autoHideChanged"] = "0", ["appbar"] = "0",
            ["taskbarVisible"] = TaskbarProbe.Taskbars().Any(IsWindowVisible) ? "1" : "0", ["workArea"] = Rect(WorkArea()),
        };
        using (var m = TakeLock()) { Write(state, s); m?.ReleaseMutex(); }

        // The guard inherits our stdout, so its lines reach the test even after we die.
        var psi = new ProcessStartInfo(guardExe, $"--watch {me.Id} \"{state}\"") { UseShellExecute = false };
        if (fault != "") psi.Environment["ATRIUM_GUARD_FAULT"] = fault;
        var guard = Process.Start(psi)!;
        Out("GUARD " + guard.Id);

        // Ready handshake: nothing is changed before the guard can watch us.
        var wait = Stopwatch.StartNew();
        while (ctl.ReadInt32(16) == 0 && !guard.HasExited && wait.ElapsedMilliseconds < 3000) { Beat(); Thread.Sleep(10); }
        if (ctl.ReadInt32(16) == 0)
        {
            using (var m = TakeLock()) { s["phase"] = "none"; Write(state, s); m?.ReleaseMutex(); }
            if (!guard.HasExited) guard.Kill();
            Out($"NOT-READY guardExited={guard.HasExited} ms={wait.ElapsedMilliseconds}");
            return;
        }
        Out($"READY ms={wait.ElapsedMilliseconds}");

        TestWindow? bar = null;
        bool appbar = false;
        void Pause(string tag)
        {
            if (mode != tag) return;
            Out("PAUSING " + tag);
            Thread.Sleep(5000); // no heartbeat: looks like a stalled startup
            Out("RESUMED " + tag);
        }
        // One takeover step: write-ahead record, then the change, both under the
        // lock and only while the takeover right is still valid.
        bool Step(string name, Action record, Action change)
        {
            Pause("pause-before:" + name);
            using var m = TakeLock();
            if (m == null || Revoked() || guard.HasExited) { m?.ReleaseMutex(); Out($"STEP-REFUSED {name} revoked={Revoked()}"); return false; }
            record();
            Write(state, s);
            Pause("pause-inside:" + name);
            change();
            m.ReleaseMutex();
            Beat();
            if (mode == "crash-after:" + name) { Out("CRASHING"); Environment.FailFast("injected startup failure"); }
            return true;
        }
        void Release()
        {
            if (appbar) TaskbarProbe.RemoveAppBar(bar!.Hwnd);
            appbar = false;
            bar?.Dispose();
        }
        void SelfRestore(string why)
        {
            // Our own resources first, then the shell through the guard executable.
            using (var m = TakeLock()) { Release(); s["appbar"] = "0"; s["phase"] = "releasing"; Write(state, s); m?.ReleaseMutex(); }
            // Keep beating while the restore runs: we are exiting, not hung.
            var restore = Process.Start(new ProcessStartInfo(guardExe, $"--restore \"{state}\"") { UseShellExecute = false })!;
            while (!restore.WaitForExit(50)) Beat();
            Out("CORE-EXIT " + why);
        }

        bool ok = true;
        if (!TaskbarProbe.GetAutoHide())
            ok = Step("autohide", () => { s["autoHideChanged"] = "1"; s["phase"] = "autohide-changed"; }, () => TaskbarProbe.SetAutoHide(true));
        if (ok)
        {
            var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
            GetMonitorInfo(MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY), ref mi);
            bar = new TestWindow(mi.rcMonitor.Left, mi.rcMonitor.Bottom - BarHeight, mi.rcMonitor.Width, BarHeight,
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, gdi: true, color: 0x00B0A090, title: "Atrium probe bar");
            ok = Step("appbar", () => { s["appbar"] = "1"; s["appbarHwnd"] = ((long)bar.Hwnd).ToString(); s["phase"] = "appbar"; },
                () => { TaskbarProbe.RegisterBottomAppBar(bar.Hwnd, BarHeight); appbar = true; bar.Show(); });
        }
        if (ok)
            ok = Step("hide", () => s["phase"] = "active", () => { foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_HIDE); });
        if (!ok)
        {
            // Revoked: the guard owns the restore. Only give back our AppBar.
            if (Revoked()) { Release(); Out("ABORTED revoked"); return; }
            SelfRestore("guard-lost-before-active");
            return;
        }
        Out($"ACTIVE workArea={Rect(WorkArea())}");

        string? cmd = null;
        new Thread(() => { string? l; while ((l = Console.ReadLine()) != null) Volatile.Write(ref cmd, l); }) { IsBackground = true }.Start();
        var end = Environment.TickCount64 + 60000;
        while (Environment.TickCount64 < end)
        {
            Pump();
            Beat();
            var c = Interlocked.Exchange(ref cmd, null);
            if (c == "HANG") { Out("HANGING"); Thread.Sleep(Timeout.Infinite); }
            if (c == "EXIT") break;
            if (Revoked()) { Release(); Out("ABORTED revoked"); return; }
            if (guard.HasExited) { Out("GUARD-LOST"); SelfRestore("guard-lost"); return; }
            Thread.Sleep(10);
        }
        // Graceful exit (the guard restores at the same moment; restores are serialized).
        SelfRestore("graceful");
    }

    // Test helper: holds the takeover lock. args: holdlock <limit> <ms>
    public static void HoldLock(string[] a)
    {
        using var m = TakeLock()!;
        Out("HELD");
        Thread.Sleep(int.Parse(a[2]));
        m.ReleaseMutex();
        Out("RELEASED");
    }

    private sealed record Snap(bool AutoHide, bool Visible, string WorkArea)
    {
        public static Snap Now() => new(TaskbarProbe.GetAutoHide(), TaskbarProbe.Taskbars().Any(IsWindowVisible), Native.WorkArea().ToString());
    }

    private sealed class Ctx
    {
        public required Process Core;
        public Process? Guard;
        public required string State;
        public required List<string> Lines;
        public readonly JsonObject Notes = new();
        public bool Has(string prefix) { lock (Lines) return Lines.Any(l => l.StartsWith(prefix)); }
        public bool WaitFor(string prefix, int ms)
        {
            var t = Stopwatch.StartNew();
            while (!Has(prefix) && t.ElapsedMilliseconds < ms) PumpFor(10);
            return Has(prefix);
        }
        public void Collect(Process p) => new Thread(() => { string? l; while ((l = p.StandardOutput.ReadLine()) != null) lock (Lines) Lines.Add(l); }) { IsBackground = true }.Start();
    }

    public static void Run(string guardExe, string only = "")
    {
        var rep = new Report("p07c-recovery-protocol" + (only == "" ? "" : "-" + only.ToLowerInvariant()));
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

        Process Start(string args, Ctx? into, string fault = "")
        {
            var psi = new ProcessStartInfo(exe, args) { RedirectStandardOutput = true, RedirectStandardInput = true, UseShellExecute = false, CreateNoWindow = true };
            var p = Process.Start(psi)!;
            into?.Collect(p);
            return p;
        }
        int ManualRestore(Ctx c)
        {
            var p = Process.Start(new ProcessStartInfo(guardExe, $"--restore \"{c.State}\"") { RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true })!;
            c.Collect(p);
            p.WaitForExit();
            return p.ExitCode;
        }

        var results = new JsonArray();
        // expectBy: who must have completed the restore ("none": the takeover must never start).
        void Scenario(string name, string mode, string fault, Action<Ctx> inject, string expectBy, string? expectLine = null)
        {
            if (only != "" && !name.StartsWith(only + "-")) return;
            // Simulated user configuration: auto-hide OFF, taskbar visible.
            TaskbarProbe.SetAutoHide(false);
            foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_SHOWNA);
            PumpFor(1500);
            var before = Snap.Now();
            var state = Path.Combine(dir, name + ".state");
            File.Delete(state);
            var lines = new List<string>();
            var ctx = new Ctx { Core = null!, State = state, Lines = lines };
            ctx.Core = Start($"txcore 90 \"{state}\" \"{guardExe}\" {mode} {(fault == "" ? "-" : fault)}", ctx);
            SafetyNet.Register("core " + name, () => { try { if (!ctx.Core.HasExited) ctx.Core.Kill(); } catch { } });
            var wait = Stopwatch.StartNew();
            while (!new[] { "ACTIVE", "CRASHING", "PAUSING", "NOT-READY" }.Any(ctx.Has) && wait.ElapsedMilliseconds < 8000) PumpFor(10);
            lock (lines)
            {
                var g = lines.FirstOrDefault(l => l.StartsWith("GUARD "));
                if (g != null) try { ctx.Guard = Process.GetProcessById(int.Parse(g[6..])); _ = ctx.Guard.Handle; } catch { }
            }
            var during = Snap.Now();
            int changedSamples = 0; // takeover must not start at all when expectBy == "none"
            if (expectBy == "none") { for (int i = 0; i < 50; i++) { if (Snap.Now() != before) changedSamples++; PumpFor(10); } }
            else PumpFor(500);

            var sw = Stopwatch.StartNew();
            inject(ctx);
            double restoredMs = -1;
            string goal = expectBy == "none" ? "none" : "restored";
            while (sw.ElapsedMilliseconds < 30000)
            {
                if (Snap.Now() == before && Read(state).GetValueOrDefault("phase") == goal) { restoredMs = sw.Elapsed.TotalMilliseconds; break; }
                PumpFor(10);
            }
            // After the restore: a stalled core that resumes must never take over again.
            int regressions = 0;
            var settle = Stopwatch.StartNew();
            while (settle.ElapsedMilliseconds < 12000 && !(ctx.Core.HasExited && (ctx.Guard?.HasExited ?? true))) { if (Snap.Now() != before) regressions++; PumpFor(20); }
            for (int i = 0; i < 75; i++) { if (Snap.Now() != before) regressions++; PumpFor(20); }
            var after = Snap.Now();
            var st = Read(state);
            bool coreGone = ctx.Core.HasExited, guardGone = ctx.Guard?.HasExited ?? true;
            if (!coreGone) try { ctx.Core.Kill(); } catch { }
            SafetyNet.Unregister("core " + name);
            string log; lock (lines) log = string.Join(" | ", lines.Where(l => !l.StartsWith("GUARD ") && !l.StartsWith("HELD")));
            bool lineOk = expectLine == null || log.Contains(expectLine);
            bool ok = restoredMs >= 0 && after == before && regressions == 0 && changedSamples == 0 && st.GetValueOrDefault("phase") == goal &&
                      (expectBy == "none" || expectBy == "*" || st.GetValueOrDefault("restoredBy") == expectBy) && coreGone && guardGone && lineOk;
            var node = new JsonObject
            {
                ["scenario"] = name, ["mode"] = mode, ["fault"] = fault, ["before"] = before.ToString(), ["duringTakeover"] = during.ToString(), ["after"] = after.ToString(),
                ["restoredMs"] = Math.Round(restoredMs, 1), ["phase"] = st.GetValueOrDefault("phase"), ["restoredBy"] = st.GetValueOrDefault("restoredBy"),
                ["attempts"] = st.GetValueOrDefault("attempts"), ["regressions"] = regressions, ["coreExited"] = coreGone, ["guardExited"] = guardGone,
                ["notes"] = ctx.Notes, ["log"] = log,
            };
            results.Add(node);
            rep.Verdict(name, ok ? "Passed" : "Failed",
                $"{goal} in {Math.Round(restoredMs)} ms by {st.GetValueOrDefault("restoredBy") ?? "-"}; after == before: {after == before}; regressions {regressions}; " +
                $"core/guard exited: {coreGone}/{guardGone}; expected line {(expectLine ?? "-")}: {lineOk}");
            Console.WriteLine($"{name}: {(ok ? "Passed" : "FAILED")} {log}");
        }

        void Kill(Ctx c) => c.Core.Kill();
        void ThenManual(Ctx c, string waitLine)
        {
            c.Notes["guardResult"] = c.WaitFor(waitLine, 20000) ? waitLine : "missing";
            var st = Read(c.State);
            c.Notes["phaseAfterGuard"] = st.GetValueOrDefault("phase");
            c.Notes["lastError"] = st.GetValueOrDefault("lastError");
            c.Guard?.WaitForExit(5000);
            c.Notes["guardExitCode"] = c.Guard is { HasExited: true } ? c.Guard.ExitCode : null;
            c.Notes["manualRestoreExitCode"] = ManualRestore(c);
        }

        // Takeover with a real AppBar, then the earlier six failures.
        Scenario("R1-kill-core", "active", "", Kill, "core-exit");
        Scenario("R2-core-hang", "active", "", c => c.Core.StandardInput.WriteLine("HANG"), "core-hang", "REVOKED hung core terminated=1");
        // FailFast goes through error reporting, which may outlast the hang threshold.
        Scenario("R3-crash-after-appbar", "crash-after:appbar", "", c => { }, "*");
        Scenario("R4-concurrent-restores", "active", "", c =>
        {
            // Graceful exit (core restore + guard restore) and two more restores at the same time.
            c.Core.StandardInput.WriteLine("EXIT");
            var extra = Enumerable.Range(0, 2).Select(_ => Task.Run(() => ManualRestore(c))).ToArray();
            Task.WaitAll(extra);
            c.Notes["extraRestoreExitCodes"] = string.Join(",", extra.Select(t => t.Result));
        }, "*");
        Scenario("R5-guard-killed", "active", "", c => c.Guard?.Kill(), "manual", "CORE-EXIT guard-lost");
        Scenario("R6-both-killed-next-start", "active", "", c =>
        {
            c.Guard?.Kill(); c.Core.Kill();
            Thread.Sleep(800);
            c.Notes["manualRestoreExitCode"] = ManualRestore(c); // simulates the next login
        }, "manual");
        // Review round 2, finding 1: handshake and revocation.
        Scenario("R7-guard-init-failure", "active", "no-map", c => { }, "none", "NOT-READY");
        Scenario("R8-startup-stall", "pause-before:autohide", "", c => { }, "core-hang", "REVOKED hung core terminated=1");
        Scenario("R9-stall-then-resume", "pause-before:hide", "no-terminate", c => { }, "core-hang", "ABORTED revoked");
        Scenario("R10-stall-inside-step-then-resume", "pause-inside:appbar", "no-terminate", c => { }, "core-hang", "ABORTED revoked");
        // Finding 2: restore must not be marked done when it did not take effect.
        Scenario("R11-explorer-missing", "active", "no-shell", c => { Kill(c); ThenManual(c, "RESTORE pending"); }, "manual", "error=shell-missing");
        Scenario("R12-explorer-late", "active", "shell-late", Kill, "core-exit");
        Scenario("R13-autohide-not-applied", "active", "autohide-noop", c => { Kill(c); ThenManual(c, "RESTORE pending"); }, "manual", "error=autohide-mismatch");
        // Finding 3: lock held past the timeout.
        Scenario("R14-lock-held-past-timeout", "active", "", c =>
        {
            var holder = Start("holdlock 60 14000", c);
            c.WaitFor("HELD", 5000);
            Kill(c);
            c.WaitFor("RESTORE lock-unavailable", 15000);
            var st = Read(c.State);
            c.Notes["phaseWhileLocked"] = st.GetValueOrDefault("phase");
            c.Notes["taskbarVisibleWhileLocked"] = Snap.Now().Visible;
            holder.WaitForExit(10000);
            c.Notes["manualRestoreExitCode"] = ManualRestore(c);
        }, "manual", "RESTORE lock-unavailable");
        // The core dies while holding the lock: the guard must take the abandoned lock.
        Scenario("R15-core-killed-holding-lock", "pause-inside:hide", "", c => { c.WaitFor("PAUSING", 5000); Kill(c); }, "core-exit", "abandoned=1");
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
