using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P06: desktop widgets and Show Desktop (Win+D) using public APIs only, the way
// Rainmeter does on 24H2+: a bottom-most top-level window that is raised while
// Show Desktop is active and lowered again afterwards. No SetParent into
// Explorer's desktop windows. The user's windows are restored at the end.
internal static class DesktopProbe
{
    private static void OnSta(Action a)
    {
        Exception? err = null;
        var t = new Thread(() => { try { a(); } catch (Exception e) { err = e; } });
        t.SetApartmentState(ApartmentState.STA);
        t.Start(); t.Join();
        if (err != null) throw err;
    }

    // Win+D through SendInput: the same path as the user's keyboard. (The
    // Shell.Application ToggleDesktop COM call had no effect from a background
    // process on this build.)
    private static void ToggleDesktop()
    {
        const ushort VK_LWIN = 0x5B, VK_D = 0x44;
        var seq = new (ushort, bool)[] { (VK_LWIN, false), (VK_D, false), (VK_D, true), (VK_LWIN, true) };
        var inputs = seq.Select(k => new INPUT { type = 1, ki = new KEYBDINPUT { wVk = k.Item1, dwFlags = k.Item2 ? 2u : 0u } }).ToArray();
        SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
    }

    private static List<IntPtr> UserWindows()
    {
        var list = new List<IntPtr>();
        EnumWindows((h, _) =>
        {
            const int GWL_EXSTYLE = -20;
            if (IsWindowVisible(h) && !IsIconic(h) && !IsCloaked(h) && TextOf(h).Length > 0 &&
                (GetWindowLong(h, GWL_EXSTYLE) & (int)WS_EX_TOOLWINDOW) == 0 && ClassOf(h) is not "Progman" and not "Shell_TrayWnd")
                list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }

    private static double Teal(int x, int y)
    {
        var px = WdaProbe.Gdi(x, y, 40, 30);
        return Pixels.Fraction(px, 0x20, 0xB0, 0xA0, 20);
    }

    public static void Run()
    {
        var rep = new Report("p06-desktop-layer");
        var waitStart = Stopwatch.StartNew();
        while (IdleSeconds() < 20 && waitStart.Elapsed.TotalSeconds < 240) Thread.Sleep(1000);
        if (IdleSeconds() < 20) { rep.Verdict("desktop-layer", "Inconclusive", "user was active"); rep.Save(); return; }

        var userBefore = UserWindows();
        rep.Set("userWindowsBefore", userBefore.Count);
        bool toggled = false;
        SafetyNet.Register("show desktop", () => { if (toggled) ToggleDesktop(); });

        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY), ref mi);
        int wx = mi.rcWork.Left + 120, wy = mi.rcWork.Top + 320, ww = 260, wh = 200;
        using var widget = new TestWindow(wx, wy, ww, wh, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, gdi: true, color: 0x00A0B020, title: "Atrium widget probe");
        ShowWindow(widget.Hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(widget.Hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // A normal app window (minimizable, not a tool window) covering the widget.
        const uint WS_OVERLAPPEDWINDOW = 0x00CF0000;
        TestWindow.EnsureClass();
        var app = CreateWindowEx(0, "AtriumProbeWindow", "Atrium covering app", WS_OVERLAPPEDWINDOW | WS_VISIBLE, wx - 40, wy - 40, ww + 80, wh + 80, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        SafetyNet.Register("covering app", () => DestroyWindow(app));
        PumpFor(500);
        int sx = wx + 100, sy = wy + 80;
        rep.Set("widgetVisibleWhenCovered", Math.Round(Teal(sx, sy), 2));

        // 1) Plain bottom-most widget during Show Desktop.
        ToggleDesktop(); toggled = true;
        PumpFor(1200);
        var fg = GetForegroundWindow();
        rep.Set("showDesktop.foregroundClass", ClassOf(fg));
        rep.Set("showDesktop.userWindowsLeft", UserWindows().Count);
        rep.Set("showDesktop.coveringAppMinimized", IsIconic(app) || IsCloaked(app) || !IsWindowVisible(app));
        rep.Set("showDesktop.widgetWindowVisible", IsWindowVisible(widget.Hwnd) && !IsIconic(widget.Hwnd));
        double plain = Teal(sx, sy);
        rep.Set("showDesktop.plainWidgetOnScreen", Math.Round(plain, 2));

        // 2) Rainmeter-style: raise while the desktop is in front.
        SetWindowPos(widget.Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        PumpFor(400);
        double raised = Teal(sx, sy);
        rep.Set("showDesktop.raisedWidgetOnScreen", Math.Round(raised, 2));

        // Leave Show Desktop, lower the widget again.
        ToggleDesktop(); toggled = false;
        PumpFor(1500);
        SetWindowPos(widget.Hwnd, new IntPtr(-2) /* HWND_NOTOPMOST */, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(widget.Hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        PumpFor(400);
        rep.Set("after.coveringAppRestored", IsWindowVisible(app) && !IsIconic(app));
        DestroyWindow(app);
        SafetyNet.Unregister("covering app");
        var userAfter = UserWindows();
        rep.Set("userWindowsAfter", userAfter.Count);
        bool restored = userBefore.All(h => userAfter.Contains(h));
        bool belowApps = true;
        // The widget must be below every restored user window in z-order.
        for (var h = GetWindow(widget.Hwnd, 3 /* GW_HWNDPREV */); h != IntPtr.Zero; h = GetWindow(h, 3))
            if (userAfter.Contains(h)) { belowApps = true; break; }
        for (var h = GetWindow(widget.Hwnd, 2 /* GW_HWNDNEXT */); h != IntPtr.Zero; h = GetWindow(h, 2))
            if (userAfter.Contains(h)) { belowApps = false; break; }
        SafetyNet.Unregister("show desktop");

        rep.Verdict("show-desktop-happened", rep.Root["showDesktop.coveringAppMinimized"]!.GetValue<bool>() ? "Passed" : "Failed", "Show Desktop minimized the covering app window");
        rep.Verdict("plain-bottom-widget", plain > 0.9 ? "Passed" : "Failed", $"a bottom-most top-level widget stays on screen during Show Desktop: {Math.Round(plain, 2)}");
        rep.Verdict("raised-widget", raised > 0.9 ? "Passed" : "Failed", $"raising the widget while the desktop is in front shows it: {Math.Round(raised, 2)}");
        rep.Verdict("lowered-after", belowApps ? "Passed" : "Failed", "after leaving Show Desktop the widget is below all user windows again");
        rep.Verdict("user-windows-restored", restored ? "Passed" : "Failed", $"user windows before/after: {userBefore.Count}/{userAfter.Count}");
        rep.Save();
    }
}
