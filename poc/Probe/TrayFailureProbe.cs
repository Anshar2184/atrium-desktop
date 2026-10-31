using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P04d: failure semantics of the forwarding tray bridge. Icons of controlled
// test clients are routed to a fake tray process that can respond, hang or die;
// all other applications keep going to the real Explorer tray. The bridge
// answers the client with the target's real result, FALSE on failure or
// timeout, and never reports success it did not get.
internal static unsafe class TrayFailureProbe
{
    [StructLayout(LayoutKind.Sequential)]
    private struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }

    private const uint WM_COPYDATA = 0x004A;
    private const uint ForwardTimeoutMs = 500;
    private static IntPtr s_explorer, s_fake, s_self;
    private static readonly HashSet<uint> s_routed = new();
    private static WndProc? s_proc;
    private static readonly List<(bool Test, bool Ok, double Ms, string Why)> s_log = new();

    [DllImport("user32.dll", SetLastError = true, EntryPoint = "SendMessageTimeoutW")] private static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);

    // One forwarding job. The payload is copied out of the borrowed WM_COPYDATA
    // buffer, so the worker never touches memory the system may already have freed.
    private sealed class Job
    {
        public IntPtr Target, WParam, DwData;
        public byte[] Payload = Array.Empty<byte>();
        public readonly ManualResetEventSlim Done = new(false);
        public bool Ok; public IntPtr Result; public string Why = "";
    }

    // One worker per target: deliveries to a target are serialized (preserving
    // message order) and a hung target cannot delay messages for another one.
    private static readonly Dictionary<IntPtr, System.Collections.Concurrent.BlockingCollection<Job>> s_queues = new();

    private static void Enqueue(Job job)
    {
        if (!s_queues.TryGetValue(job.Target, out var q))
        {
            q = new System.Collections.Concurrent.BlockingCollection<Job>();
            s_queues[job.Target] = q;
            new Thread(() => Worker(q)) { IsBackground = true, Name = "tray forwarder" }.Start();
        }
        q.Add(job);
    }

    private static void Worker(System.Collections.Concurrent.BlockingCollection<Job> jobs)
    {
        foreach (var job in jobs.GetConsumingEnumerable())
        {
            var mem = Marshal.AllocHGlobal(job.Payload.Length);
            var pcds = Marshal.AllocHGlobal(Marshal.SizeOf<COPYDATASTRUCT>());
            try
            {
                Marshal.Copy(job.Payload, 0, mem, job.Payload.Length);
                Marshal.StructureToPtr(new COPYDATASTRUCT { dwData = job.DwData, cbData = job.Payload.Length, lpData = mem }, pcds, false);
                bool ok = SendMessageTimeout(job.Target, WM_COPYDATA, job.WParam, pcds, 0x0002 | 0x0020, 4000, out var res) != IntPtr.Zero;
                int err = Marshal.GetLastWin32Error();
                job.Ok = ok; job.Result = res; job.Why = ok ? "answered" : err == 1460 ? "timeout" : $"error {err}";
            }
            finally { Marshal.FreeHGlobal(pcds); Marshal.FreeHGlobal(mem); job.Done.Set(); }
        }
    }

    private static IntPtr Proc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l)
    {
        if (msg != WM_COPYDATA)
            return DefWindowProc(hwnd, msg, w, l);
        var cds = Marshal.PtrToStructure<COPYDATASTRUCT>(l);
        IntPtr target = s_explorer;
        bool test = cds.dwData == 1 && cds.cbData >= 20 && s_routed.Contains(*(uint*)((byte*)cds.lpData + 12));
        if (test) target = s_fake;
        var sw = Stopwatch.StartNew();
        // A dead target is a failure, reported immediately.
        if (target == IntPtr.Zero || !IsWindow(target)) { s_log.Add((test, false, 0, "no target")); return IntPtr.Zero; }
        var job = new Job { Target = target, WParam = w, DwData = cds.dwData, Payload = new byte[cds.cbData] };
        Marshal.Copy(cds.lpData, job.Payload, 0, cds.cbData);
        Enqueue(job);
        // Our own bound, independent of how long the delivery itself takes.
        if (!job.Done.Wait((int)ForwardTimeoutMs))
        {
            // The tray may still process the message later: outcome unknown, answer FALSE.
            s_log.Add((test, false, sw.Elapsed.TotalMilliseconds, "bridge timeout"));
            return IntPtr.Zero;
        }
        s_log.Add((test, job.Ok && job.Result != IntPtr.Zero, sw.Elapsed.TotalMilliseconds, job.Why));
        return job.Ok ? job.Result : IntPtr.Zero;
    }

    // Child process: a fake tray that can be told to hang on the next message.
    public static void FakeTray()
    {
        int hang = 0; var icons = new Dictionary<(uint, uint), (int Adds, int Mods, int LastSeq, bool Ordered)>();
        WndProc proc = (h, m, w, l) =>
        {
            if (m == WM_COPYDATA)
            {
                var cds = Marshal.PtrToStructure<COPYDATASTRUCT>(l);
                if (cds.dwData == 1)
                {
                    if (hang > 0) { Thread.Sleep(hang); hang = 0; }
                    byte* p = (byte*)cds.lpData;
                    uint nim = *(uint*)(p + 4), ih = *(uint*)(p + 12), uid = *(uint*)(p + 16);
                    string tip = new string((char*)(p + 8 + 24));
                    var key = (ih, uid);
                    icons.TryGetValue(key, out var st);
                    if (st == default) st = (0, 0, -1, true);
                    if (nim == 0) st.Adds++;
                    if (nim == 1)
                    {
                        st.Mods++;
                        if (tip.StartsWith("seq ") && int.TryParse(tip[4..], out var seq)) { if (seq <= st.LastSeq) st.Ordered = false; st.LastSeq = seq; }
                    }
                    if (nim == 2) { icons.Remove(key); return new IntPtr(1); }
                    icons[key] = st;
                    return new IntPtr(1);
                }
            }
            return DefWindowProc(h, m, w, l);
        };
        var wc = new WNDCLASSEX { cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(), lpfnWndProc = Marshal.GetFunctionPointerForDelegate(proc), hInstance = GetModuleHandle(null), lpszClassName = "AtriumFakeTray" };
        RegisterClassEx(ref wc);
        var hwnd = CreateWindowEx(0, "AtriumFakeTray", "", 0, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        Console.WriteLine("HWND " + hwnd.ToInt64()); Console.Out.Flush();
        string? cmd = null;
        new Thread(() => { string? x; while ((x = Console.ReadLine()) != null) Volatile.Write(ref cmd, x); }) { IsBackground = true }.Start();
        var end = Environment.TickCount64 + 120000;
        while (Environment.TickCount64 < end)
        {
            Pump();
            var c = Interlocked.Exchange(ref cmd, null);
            if (c == "QUIT") break;
            if (c != null && c.StartsWith("HANG ")) { hang = int.Parse(c[5..]); Console.WriteLine("HANG set"); Console.Out.Flush(); }
            if (c == "STATE")
            {
                var parts = icons.Select(kv => $"{kv.Key.Item1}:{kv.Value.Adds}:{kv.Value.Mods}:{kv.Value.LastSeq}:{kv.Value.Ordered}");
                Console.WriteLine("STATE " + string.Join(",", parts)); Console.Out.Flush();
            }
            Thread.Sleep(2);
        }
        GC.KeepAlive(proc);
    }

    private static (int Adds, int Mods, int LastSeq, bool Ordered)? IconState(TrayClientProc fake, long hwnd)
    {
        var line = fake.Command("STATE", "STATE", 3000);
        foreach (var part in line.Length > 6 ? line[6..].Split(',', StringSplitOptions.RemoveEmptyEntries) : Array.Empty<string>())
        {
            var f = part.Split(':');
            if (long.Parse(f[0]) == (uint)hwnd) return (int.Parse(f[1]), int.Parse(f[2]), int.Parse(f[3]), bool.Parse(f[4]));
        }
        return null;
    }

    private static int Field(string line, string key)
    {
        var t = line.Split(' ').FirstOrDefault(x => x.StartsWith(key + "="));
        return t == null ? -1 : (int)double.Parse(t[(key.Length + 1)..]);
    }

    public static void Run()
    {
        var rep = new Report("p04d-tray-bridge-failures");
        var exe = Environment.ProcessPath!;
        s_explorer = FindWindow("Shell_TrayWnd", null);
        bool topBefore = (GetWindowLong(s_explorer, -20) & 0x8) != 0;

        TrayClientProc Fake() { var f = TrayClientProc.StartRaw(exe, "faketray 150"); s_fake = new IntPtr(f.Hwnd); return f; }
        TrayClientProc Client(string tip) { var c = TrayClientProc.StartRaw(exe, $"trayclient 150 0 \"{tip}\" noadd"); s_routed.Add((uint)c.Hwnd); return c; }

        var fake = Fake();
        s_proc = Proc;
        var wc = new WNDCLASSEX { cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(), lpfnWndProc = Marshal.GetFunctionPointerForDelegate(s_proc), hInstance = GetModuleHandle(null), lpszClassName = "Shell_TrayWnd" };
        RegisterClassEx(ref wc);
        s_self = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "Shell_TrayWnd", "", WS_POPUP, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        SafetyNet.Register("bridge window", () => DestroyWindow(s_self));
        SetWindowPos(s_self, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        rep.Set("bridgeIsFirstShell_TrayWnd", FindWindow("Shell_TrayWnd", null) == s_self);

        // S1 normal path.
        using var x = Client("Atrium failure test X");
        var s1 = x.Command("ADD", "ADD", 3000);
        var st1 = IconState(fake, x.Hwnd);
        rep.Set("S1.normal.client", s1);
        rep.Verdict("S1-normal", s1.Contains("ok=True") && st1?.Adds == 1 ? "Passed" : "Failed", $"client got {s1}; target has the icon: {st1 != null}");

        // S2 hung target: one message times out.
        fake.Command("HANG 3000", "HANG", 3000);
        int logBefore = s_log.Count;
        var s2 = x.Command("BURST 1", "BURST", 8000);
        var bridgeMs = s_log.Skip(logBefore).Where(e => e.Test).Select(e => e.Ms).DefaultIfEmpty(0).Max();
        PumpFor(3500);
        var st2 = IconState(fake, x.Hwnd);
        rep.Set("S2.hung.client", s2);
        rep.Set("S2.hung.log", new JsonArray(s_log.Skip(logBefore).Select(e => (JsonNode)$"{(e.Test ? "test" : "real")} ok={e.Ok} ms={e.Ms:F0} {e.Why}").ToArray()));
        rep.Set("S2.hung.bridgeBlockedMs", Math.Round(bridgeMs, 1));
        rep.Set("S2.hung.targetProcessedLater", st2?.Mods == 1);
        rep.Verdict("S2-hung-target", s2.Contains("ok=0") && bridgeMs <= ForwardTimeoutMs + 50 ? "Passed" : "Failed",
            $"hung target: client told FALSE ({s2}), bridge blocked {Math.Round(bridgeMs)} ms (bound {ForwardTimeoutMs}); target processed the message later: {st2?.Mods == 1} -> outcome after a timeout is uncertain");

        // S3 dead target, then recovery through a fresh target and an app re-add.
        fake.Kill();
        PumpFor(300);
        logBefore = s_log.Count;
        var s3 = x.Command("BURST 1", "BURST", 3000);
        var deadMs = s_log.Skip(logBefore).Where(e => e.Test).Select(e => e.Ms).DefaultIfEmpty(-1).Max();
        fake = Fake();
        var s3b = x.Command("ADD", "ADD", 3000);
        var st3 = IconState(fake, x.Hwnd);
        rep.Set("S3.dead.client", s3);
        rep.Set("S3.readd.client", s3b);
        rep.Verdict("S3-dead-target", s3.Contains("ok=0") && deadMs < 50 && s3b.Contains("ok=True") && st3?.Adds == 1 ? "Passed" : "Failed",
            $"dead target: client told FALSE ({s3}) in {Math.Round(deadMs, 1)} ms; after a new target appears the app's re-add succeeds and the target has the icon");

        // S4 burst from three clients at once.
        var ys = Enumerable.Range(0, 3).Select(i => Client("Atrium burst " + i)).ToList();
        foreach (var y in ys) y.Command("ADD", "ADD", 3000);
        var marks = ys.Select(y => y.Mark()).ToList();
        foreach (var y in ys) y.Send("BURST 100");
        var bursts = ys.Select((y, i) => y.WaitFor("BURST", marks[i], 30000)).ToList();
        var states = ys.Select(y => IconState(fake, y.Hwnd)).ToList();
        bool allOk = bursts.All(b => b.Contains("ok=100")) && states.All(s => s?.Mods == 100 && s.Value.Ordered && s.Value.LastSeq == 99);
        rep.Set("S4.burst.clients", new JsonArray(bursts.Select(b => (JsonNode)b).ToArray()));
        rep.Set("S4.burst.maxBridgeMs", Math.Round(s_log.Where(e => e.Test).Select(e => e.Ms).DefaultIfEmpty(0).Max(), 1));
        rep.Verdict("S4-burst", allOk ? "Passed" : "Failed", "3 clients x 100 NIM_MODIFY: every call succeeded, target received all 300 in per-client order");

        // S5 bridge disappears in the middle of a burst: successes reported to the
        // client must equal what the target actually received.
        using var z = Client("Atrium exit test Z");
        z.Command("ADD", "ADD", 3000);
        int mark = z.Mark();
        int forwardedBefore = s_log.Count(e => e.Test);
        z.Send("BURST 400");
        var swz = Stopwatch.StartNew();
        while (s_log.Count(e => e.Test) - forwardedBefore < 150 && swz.ElapsedMilliseconds < 10000) { Pump(); Thread.Sleep(0); }
        DestroyWindow(s_self);
        SafetyNet.Unregister("bridge window");
        var s5 = z.WaitFor("BURST", mark, 30000);
        var st5 = IconState(fake, z.Hwnd);
        int okCount = Field(s5, "ok");
        rep.Set("S5.exit.client", s5);
        rep.Set("S5.exit.targetReceived", st5?.Mods ?? -1);
        rep.Verdict("S5-bridge-exit", okCount == (st5?.Mods ?? -1) ? "Passed" : "Failed",
            $"bridge destroyed mid-burst: client counted {okCount} successes, target received {st5?.Mods}; calls after the exit went to Explorer, which correctly refused an unknown icon");

        var realLog = s_log.Where(e => !e.Test).ToList();
        rep.Set("realApps.messages", realLog.Count);
        rep.Set("realApps.failures", realLog.Count(e => !e.Ok && e.Why != "answered"));
        rep.Verdict("real-apps-unaffected", realLog.All(e => e.Why != "bridge timeout") ? "Passed" : "Failed", "messages of real applications never timed out while the test target was hung");
        rep.Set("realApps.maxBridgeMs", Math.Round(realLog.Select(e => e.Ms).DefaultIfEmpty(0).Max(), 1));
        bool topAfter = (GetWindowLong(s_explorer, -20) & 0x8) != 0;
        rep.Verdict("explorer-untouched", topAfter == topBefore ? "Passed" : "Failed", $"Explorer taskbar topmost before/after: {topBefore}/{topAfter}");

        foreach (var y in ys) y.Dispose();
        fake.Command("QUIT", "", 500);
        fake.Dispose();
        rep.Save();
    }
}
