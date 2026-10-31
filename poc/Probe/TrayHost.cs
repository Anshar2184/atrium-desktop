using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P04b: a minimal coexistence tray bridge. It becomes the window that
// Shell_NotifyIcon talks to, records every icon message AND forwards it to
// Explorer's tray, so Explorer never loses an icon. Explorer's taskbar z-order
// is not touched. Driven with controlled test clients only.
internal static unsafe class TrayHost
{
    [StructLayout(LayoutKind.Sequential)]
    private struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }

    private const uint WM_COPYDATA = 0x004A;
    private static IntPtr s_explorer;
    private static IntPtr s_self;
    private static WndProc? s_proc;
    private static readonly Dictionary<(uint Hwnd, uint Id), string> s_icons = new();
    private static int s_forwarded, s_forwardFailed;

    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll")] private static extern bool SendNotifyMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);

    private static IntPtr Proc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l)
    {
        if (msg == WM_COPYDATA)
        {
            var cds = Marshal.PtrToStructure<COPYDATASTRUCT>(l);
            if (cds.dwData == 1 && cds.cbData >= 32)
            {
                // SHELLTRAYDATA: DWORD signature, DWORD message, NOTIFYICONDATA32 (32-bit handles).
                byte* p = (byte*)cds.lpData;
                uint nim = *(uint*)(p + 4);
                uint ihwnd = *(uint*)(p + 8 + 4), uid = *(uint*)(p + 8 + 8);
                var key = (ihwnd, uid);
                if (nim == 0 || nim == 1) s_icons[key] = nim == 0 ? "added" : "modified";
                else if (nim == 2) s_icons.Remove(key);
            }
            // Coexistence: Explorer always gets the same message and its answer wins.
            if (s_explorer != IntPtr.Zero && SendMessageTimeout(s_explorer, msg, w, l, 0x0002 /* ABORTIFHUNG */, 2000, out var res) != IntPtr.Zero)
            {
                s_forwarded++;
                return res;
            }
            s_forwardFailed++;
            return new IntPtr(1);
        }
        if (msg >= WM_USER && s_explorer != IntPtr.Zero)
        {
            SendMessageTimeout(s_explorer, msg, w, l, 0x0002, 2000, out var r);
            return r;
        }
        return DefWindowProc(hwnd, msg, w, l);
    }

    public static void Run()
    {
        var rep = new Report("p04b-tray-bridge-forwarding");
        var exe = Environment.ProcessPath!;
        s_explorer = FindWindow("Shell_TrayWnd", null);
        bool topBefore = (GetWindowLong(s_explorer, -20) & 0x8) != 0;

        TrayClientProc Start(bool readd, string tip) => TrayClientProc.Start(exe, readd, tip);
        using var a = Start(true, "Atrium bridge test A");
        using var b = Start(false, "Atrium bridge test B");
        PumpFor(600);

        s_proc = Proc;
        var wc = new WNDCLASSEX { cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(), lpfnWndProc = Marshal.GetFunctionPointerForDelegate(s_proc), hInstance = GetModuleHandle(null), lpszClassName = "Shell_TrayWnd" };
        RegisterClassEx(ref wc);
        s_self = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "Shell_TrayWnd", "", WS_POPUP, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        SafetyNet.Register("tray bridge window", () => DestroyWindow(s_self));
        SetWindowPos(s_self, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        rep.Set("bridgeIsFirstShell_TrayWnd", FindWindow("Shell_TrayWnd", null) == s_self);

        // Ask apps to re-register so the bridge learns existing icons.
        uint taskbarCreated = RegisterWindowMessage("TaskbarCreated");
        SendNotifyMessage(new IntPtr(0xFFFF), taskbarCreated, IntPtr.Zero, IntPtr.Zero);
        PumpFor(2500);
        int realIcons = s_icons.Keys.Count(k => k.Hwnd != (uint)a.Hwnd && k.Hwnd != (uint)b.Hwnd);
        rep.Set("icons.realAppsLearned", realIcons);
        rep.Set("icons.hasA", s_icons.ContainsKey(((uint)a.Hwnd, 1)));
        rep.Set("icons.hasB", s_icons.ContainsKey(((uint)b.Hwnd, 1)));

        using var c = Start(false, "Atrium bridge test C");
        PumpFor(1200);
        rep.Set("icons.hasC", s_icons.ContainsKey(((uint)c.Hwnd, 1)));
        rep.Set("C.added", c.First);
        rep.Set("forwarded", s_forwarded);
        rep.Set("forwardFailed", s_forwardFailed);
        rep.Set("explorerTopmostDuring", (GetWindowLong(s_explorer, -20) & 0x8) != 0);

        DestroyWindow(s_self);
        SafetyNet.Unregister("tray bridge window");
        PumpFor(800);
        string ra = a.Rect(), rb = b.Rect(), rc = c.Rect();
        rep.Set("after.A", ra); rep.Set("after.B", rb); rep.Set("after.C", rc);
        bool topAfter = (GetWindowLong(s_explorer, -20) & 0x8) != 0;
        rep.Set("explorerTopmostAfter", topAfter);
        bool ok(string r) => r.Contains("hr=0x00000000");
        rep.Verdict("forwarding-keeps-explorer-in-sync", ok(ra) && ok(rb) && ok(rc) ? "Passed" : "Failed",
            "after the bridge exits, Explorer still has A (re-registers), B (pre-existing, ignores TaskbarCreated) and C (added while the bridge ran)");
        rep.Verdict("bridge-learns-icons", (bool)rep.Root["icons.hasA"]! && (bool)rep.Root["icons.hasC"]! && realIcons > 0 ? "Passed" : "Failed",
            $"bridge learned {realIcons} real-app icons via TaskbarCreated, plus A and C");
        rep.Verdict("pre-existing-silent-icons", (bool)rep.Root["icons.hasB"]! ? "Passed" : "Failed",
            "an icon registered before the bridge by an app that ignores TaskbarCreated is not learnable through public messages on Windows 11");
        rep.Verdict("explorer-zorder-untouched", topAfter == topBefore ? "Passed" : "Failed", $"Explorer taskbar topmost before/after: {topBefore}/{topAfter}");
        rep.Save();
    }
}

// Out-of-process controlled tray client wrapper (see TrayClient).
internal sealed class TrayClientProc : IDisposable
{
    private Process _p = null!;
    private readonly List<string> _lines = new();
    public long Hwnd;
    public string First = "";

    public static TrayClientProc Start(string exe, bool readd, string tip) => StartRaw(exe, $"trayclient 150 {(readd ? 1 : 0)} \"{tip}\"");

    // Starts a child probe whose first stdout line is "HWND <n> ...".
    public static TrayClientProc StartRaw(string exe, string args)
    {
        var c = new TrayClientProc();
        c._p = Process.Start(new ProcessStartInfo(exe, args) { RedirectStandardOutput = true, RedirectStandardInput = true, UseShellExecute = false, CreateNoWindow = true })!;
        new Thread(() => { string? l; while ((l = c._p.StandardOutput.ReadLine()) != null) lock (c._lines) c._lines.Add(l); }) { IsBackground = true }.Start();
        var end = Environment.TickCount64 + 5000;
        while (Environment.TickCount64 < end)
        {
            lock (c._lines) if (c._lines.Count > 0) { c.First = c._lines[0]; c.Hwnd = long.Parse(c.First.Split(' ')[1]); break; }
            Native.PumpFor(20);
        }
        return c;
    }

    public int Mark() { lock (_lines) return _lines.Count; }
    public void Send(string cmd) => _p.StandardInput.WriteLine(cmd);

    // Waits (pumping this thread's messages) for a line starting with prefix after mark.
    public string WaitFor(string prefix, int mark, int timeoutMs)
    {
        var end = Environment.TickCount64 + timeoutMs;
        while (Environment.TickCount64 < end)
        {
            lock (_lines) { var r = _lines.Skip(mark).FirstOrDefault(l => l.StartsWith(prefix)); if (r != null) return r; }
            Native.PumpFor(5);
        }
        return prefix + " timeout";
    }

    public string Command(string cmd, string prefix, int timeoutMs)
    {
        int mark = Mark();
        Send(cmd);
        return prefix.Length == 0 ? "" : WaitFor(prefix, mark, timeoutMs);
    }

    public void Kill() { if (!_p.HasExited) _p.Kill(); _p.WaitForExit(2000); }

    public string Rect()
    {
        int n; lock (_lines) n = _lines.Count;
        _p.StandardInput.WriteLine("RECT");
        var end = Environment.TickCount64 + 3000;
        while (Environment.TickCount64 < end)
        {
            lock (_lines) { var r = _lines.Skip(n).FirstOrDefault(l => l.StartsWith("RECT")); if (r != null) return r; }
            Native.PumpFor(20);
        }
        return "RECT timeout";
    }

    public void Dispose()
    {
        try { _p.StandardInput.WriteLine("QUIT"); _p.WaitForExit(3000); } catch { }
        if (!_p.HasExited) _p.Kill();
    }
}
