using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Input;
using System.Windows.Threading;
using ManagedShell.WindowsTray;

namespace Atrium.TrayProbe;

// P04: third-party notification-area icons through ManagedShell on Windows 11.
// Uses controlled test clients (Atrium.Probe trayclient) and records only
// counts for real applications' icons, never their titles.
internal static class Program
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string? cls, string? name);
    [DllImport("user32.dll")] private static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);

    private sealed class Client : IDisposable
    {
        public Process P = null!;
        public long Hwnd;
        public readonly List<string> Lines = new();
        // Never block the bridge thread: it must keep pumping, because every
        // Shell_NotifyIcon call from any app is a synchronous message to it.
        public static Client Start(string exe, bool readd, string tip)
        {
            var c = new Client();
            c.P = Process.Start(new ProcessStartInfo(exe, $"trayclient 150 {(readd ? 1 : 0)} \"{tip}\"") { RedirectStandardOutput = true, RedirectStandardInput = true, UseShellExecute = false, CreateNoWindow = true })!;
            new Thread(() => { string? l; while ((l = c.P.StandardOutput.ReadLine()) != null) lock (c.Lines) c.Lines.Add(l); }) { IsBackground = true }.Start();
            var end = Environment.TickCount64 + 5000;
            while (Environment.TickCount64 < end)
            {
                lock (c.Lines) if (c.Lines.Count > 0) { c.Hwnd = long.Parse(c.Lines[0].Split(' ')[1]); break; }
                Wait(20);
            }
            return c;
        }
        public string Rect()
        {
            int n; lock (Lines) n = Lines.Count;
            P.StandardInput.WriteLine("RECT");
            var end = Environment.TickCount64 + 3000;
            while (Environment.TickCount64 < end)
            {
                lock (Lines) { var r = Lines.Skip(n).FirstOrDefault(l => l.StartsWith("RECT")); if (r != null) return r; }
                Wait(20);
            }
            return "RECT timeout";
        }
        public bool Saw(string prefix) { lock (Lines) return Lines.Any(l => l.StartsWith(prefix)); }
        public List<string> Callbacks() { lock (Lines) return Lines.Where(l => l.StartsWith("CB ")).ToList(); }
        public void Dispose() { try { P.StandardInput.WriteLine("QUIT"); P.WaitForExit(3000); } catch { } if (!P.HasExited) P.Kill(); }
    }

    private static void Wait(int ms)
    {
        var frame = new DispatcherFrame();
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(ms) };
        timer.Tick += (_, _) => { timer.Stop(); frame.Continue = false; };
        timer.Start();
        Dispatcher.PushFrame(frame);
    }

    private static bool Ok(string rect) => rect.Contains("hr=0x00000000");

    [STAThread]
    private static int Main(string[] args)
    {
        var probeExe = args[0];
        var resultsDir = args[1];
        var rep = new JsonObject { ["probe"] = "p04-tray", ["startedAt"] = DateTimeOffset.Now.ToString("O"), ["os"] = Environment.OSVersion.VersionString };
        void Set(string k, JsonNode? v) { rep[k] = v; Console.WriteLine($"  {k} = {v?.ToJsonString()}"); }
        var verdicts = new JsonObject(); rep["verdicts"] = verdicts;
        void Verdict(string k, string v, string note) { verdicts[k] = new JsonObject { ["verdict"] = v, ["note"] = note }; Console.WriteLine($"[{v}] {k}: {note}"); }

        var explorerBar = FindWindow("Shell_TrayWnd", null);
        bool topmostBefore = (GetWindowLong(explorerBar, -20) & 0x8) != 0;
        Set("explorerTaskbarTopmostBefore", topmostBefore);

        using var a = Client.Start(probeExe, readd: true, "Atrium test A (handles TaskbarCreated)");
        using var b = Client.Start(probeExe, readd: false, "Atrium test B (ignores TaskbarCreated)");
        Wait(800);
        Set("before.A.rect", a.Rect());
        Set("before.B.rect", b.Rect());

        NotificationArea? area = null;
        Client? c = null;
        try
        {
            area = new NotificationArea(new TrayService(), new ExplorerTrayService());
            area.Initialize();
            Set("initializeFailed", area.IsFailed);
            Wait(3000);
            var icons = area.TrayIcons.ToList();
            bool hasA = icons.Any(i => i.HWnd.ToInt64() == a.Hwnd), hasB = icons.Any(i => i.HWnd.ToInt64() == b.Hwnd);
            int ours = icons.Count(i => i.HWnd.ToInt64() == a.Hwnd || i.HWnd.ToInt64() == b.Hwnd);
            Set("icons.total", icons.Count);
            Set("icons.realApps", icons.Count - ours);
            Set("icons.realAppsWithImage", icons.Count(i => i.Icon != null) - ours);
            Set("icons.realAppsWithPath", icons.Count(i => !string.IsNullOrEmpty(i.Path)) - ours);
            Set("icons.hasA", hasA);
            Set("icons.hasB", hasB);
            Set("A.sawTaskbarCreated", a.Saw("TASKBARCREATED"));

            // An icon added while the bridge runs.
            var trayTop = FindWindow("Shell_TrayWnd", null);
            GetWindowThreadProcessId(trayTop, out var trayPid);
            Set("during.FindWindowShell_TrayWnd.owner", Process.GetProcessById((int)trayPid).ProcessName);
            c = Client.Start(probeExe, readd: false, "Atrium test C (added during bridge)");
            Set("C.first", c.Lines[0]);
            Wait(2500);
            Set("icons.hasC", area.TrayIcons.Any(i => i.HWnd.ToInt64() == c.Hwnd));
            Set("during.C.rect", c.Rect());

            // Click forwarding: left click and right click on A.
            var ia = area.TrayIcons.FirstOrDefault(i => i.HWnd.ToInt64() == a.Hwnd);
            if (ia != null)
            {
                uint pos = (uint)((1000 << 16) | 1000);
                ia.IconMouseDown(MouseButton.Left, pos, 500); ia.IconMouseUp(MouseButton.Left, pos, 500);
                Wait(300);
                ia.IconMouseDown(MouseButton.Right, pos, 500); ia.IconMouseUp(MouseButton.Right, pos, 500);
                Wait(600);
            }
            Set("A.callbacks", new JsonArray(a.Callbacks().Select(x => (JsonNode)x).ToArray()));
            Set("explorerTaskbarTopmostDuring", (GetWindowLong(explorerBar, -20) & 0x8) != 0);
        }
        finally
        {
            area?.Dispose();
        }
        Wait(1500);
        Set("after.A.rect", a.Rect());
        Set("after.B.rect", b.Rect());
        if (c != null) Set("after.C.rect", c.Rect());
        bool topmostAfter = (GetWindowLong(explorerBar, -20) & 0x8) != 0;
        Set("explorerTaskbarTopmostAfter", topmostAfter);
        if (topmostBefore && !topmostAfter)
        {
            SetWindowPos(explorerBar, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010);
            Set("explorerTaskbarTopmostRepaired", (GetWindowLong(explorerBar, -20) & 0x8) != 0);
        }
        // ManagedShell's own Shell_TrayWnd must be gone.
        var top = FindWindow("Shell_TrayWnd", null);
        GetWindowThreadProcessId(top, out var pid);
        Set("shellTrayWndOwner", Process.GetProcessById((int)pid).ProcessName);

        var cbs = (JsonArray)rep["A.callbacks"]!;
        Verdict("icons-collected", (bool)rep["icons.hasA"]! && (int)rep["icons.realApps"]! > 0 ? "Passed" : "Failed",
            $"{rep["icons.realApps"]} real-app icons and test icon A (re-registers on TaskbarCreated) collected on Windows 11");
        Verdict("icons-without-taskbarcreated", (bool)rep["icons.hasB"]! ? "Passed" : "Failed",
            "an icon that existed before the bridge and ignores TaskbarCreated is visible to the bridge (needs reading Explorer's existing icons)");
        Verdict("icons-added-during", rep["icons.hasC"]?.GetValue<bool>() == true ? "Passed" : "Failed", "an icon added while the bridge runs is collected");
        Verdict("click-forwarding", cbs.Count > 0 ? "Passed" : "Failed", $"callback messages delivered to the owning app: {string.Join(",", cbs.Select(x => x!.ToString()))}");
        bool explorerIntact = Ok((string)rep["after.A.rect"]!) && Ok((string)rep["after.B.rect"]!) && (rep["after.C.rect"] == null || Ok((string)rep["after.C.rect"]!));
        Verdict("explorer-tray-intact-after", explorerIntact ? "Passed" : "Failed", "after the bridge exits Explorer still knows icons A, B and C (Shell_NotifyIconGetRect succeeds)");
        Verdict("explorer-taskbar-zorder", topmostAfter || !topmostBefore ? "Passed" : "Failed", $"Explorer taskbar topmost before/after: {topmostBefore}/{topmostAfter} (repaired if needed)");
        c?.Dispose();
        rep["finishedAt"] = DateTimeOffset.Now.ToString("O");
        Directory.CreateDirectory(resultsDir);
        File.WriteAllText(Path.Combine(resultsDir, "p04-tray.json"), rep.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
        return 0;
    }
}
