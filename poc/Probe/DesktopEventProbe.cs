using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P06b: the desktop-widget lifecycle driven only by EVENT_SYSTEM_FOREGROUND.
// A state machine raises the widget when the desktop becomes foreground and
// lowers it when a normal application window does; shell surfaces (taskbar,
// Start, flyouts) and our own widget do not change the state. Every scenario is
// a user-equivalent action (keys, clicks, UI Automation invoke), never a manual
// raise/lower by the test.
internal static class DesktopEventProbe
{
    private delegate void WinEventProc(IntPtr hook, uint ev, IntPtr hwnd, int obj, int child, uint thread, uint time);
    [DllImport("user32.dll")] private static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr mod, WinEventProc proc, uint pid, uint tid, uint flags);
    [DllImport("user32.dll")] private static extern bool UnhookWinEvent(IntPtr hook);
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);

    private static TestWindow? s_widget;
    private static bool s_raised;
    private static readonly List<(double T, string Event)> s_events = new();
    private static readonly Stopwatch s_clock = Stopwatch.StartNew();

    private static bool IsDesktop(IntPtr h) => ClassOf(h) is "Progman" or "WorkerW";

    // Normal application windows end "desktop mode"; shell surfaces do not.
    private static bool IsAppWindow(IntPtr h)
    {
        if (h == IntPtr.Zero || (s_widget != null && h == s_widget.Hwnd)) return false;
        var cls = ClassOf(h);
        if (cls is "Shell_TrayWnd" or "Shell_SecondaryTrayWnd" or "Windows.UI.Core.CoreWindow" or "TopLevelWindowForOverflowXamlIsland" or "XamlExplorerHostIslandWindow" or "ForegroundStaging" or "MultitaskingViewFrame") return false;
        return TasksProbe.TaskbarWindows().Contains(h);
    }

    // Re-evaluated on foreground changes and on every minimize/restore:
    // desktop mode = every app window is minimized and no app is foreground.
    // (Win+D while the desktop is already foreground fires no foreground event,
    // and a restore can leave the taskbar as foreground; a click on an exposed
    // patch of desktop while apps are open must not lift the widget.)
    private static void OnEvent(IntPtr hook, uint ev, IntPtr hwnd, int obj, int child, uint thread, uint time)
    {
        if (s_widget == null || obj != 0) return;
        var fg = GetForegroundWindow();
        bool desktopMode = !IsAppWindow(fg) && TasksProbe.TaskbarWindows().Where(IsAppWindow).All(IsIconic);
        string why = ev switch { 3 => "foreground", 0x16 => "minimize", 0x17 => "restore", _ => ev.ToString() };
        if (desktopMode && !s_raised)
        {
            SetWindowPos(s_widget.Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            s_raised = true;
            s_events.Add((s_clock.Elapsed.TotalMilliseconds, $"raise ({why}, fg {ClassOf(fg)})"));
        }
        else if (!desktopMode && s_raised)
        {
            SetWindowPos(s_widget.Hwnd, new IntPtr(-2), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(s_widget.Hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            s_raised = false;
            s_events.Add((s_clock.Elapsed.TotalMilliseconds, $"lower ({why}, fg {ClassOf(fg)})"));
        }
    }

    private static void WinD()
    {
        var seq = new (ushort, bool)[] { (0x5B, false), (0x44, false), (0x44, true), (0x5B, true) };
        var inputs = seq.Select(k => new INPUT { type = 1, ki = new KEYBDINPUT { wVk = k.Item1, dwFlags = k.Item2 ? 2u : 0u } }).ToArray();
        SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
    }

    // UI Automation invoke through Windows PowerShell's UIA client (no coordinates).
    private static string Uia(string condition)
    {
        var script = "Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes; " +
            "$r=[System.Windows.Automation.AutomationElement]::RootElement; " +
            "$t=$r.FindFirst(2,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty,'Shell_TrayWnd'))); " +
            $"$e=$t.FindAll(4,[System.Windows.Automation.Condition]::TrueCondition) | ? {{ {condition} }} | Select -First 1; " +
            "if ($e) { $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); 'invoked' } else { 'not found' }";
        var psi = new ProcessStartInfo("powershell.exe", "-NoProfile -Command \"" + script.Replace("\"", "\\\"") + "\"") { RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true };
        using var p = Process.Start(psi)!;
        var o = p.StandardOutput.ReadToEnd().Trim();
        p.WaitForExit();
        return o;
    }

    private static double Teal(int x, int y) => Pixels.Fraction(WdaProbe.Gdi(x, y, 30, 20), 0x20, 0xB0, 0xA0, 20);

    // Waits until the widget's on-screen visibility matches `visible`.
    private static double WaitVisible(int x, int y, bool visible, int timeoutMs = 2500)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs)
        {
            Pump();
            if ((Teal(x, y) > 0.9) == visible) return sw.Elapsed.TotalMilliseconds;
            Thread.Sleep(15);
        }
        return -1;
    }

    public static void Run()
    {
        var rep = new Report("p06b-desktop-events");
        var waitStart = Stopwatch.StartNew();
        while (IdleSeconds() < 20 && waitStart.Elapsed.TotalSeconds < 300) Thread.Sleep(1000);
        if (IdleSeconds() < 20) { rep.Verdict("desktop-events", "Inconclusive", "user was active"); rep.Save(); return; }
        var userFg = GetForegroundWindow();
        // Win+D minimizes the user's windows too; put back every one that was open.
        var userOpen = TasksProbe.TaskbarWindows().Where(h => !IsIconic(h)).ToList();
        void RestoreUserWindows()
        {
            foreach (var h in userOpen) if (IsWindow(h) && IsIconic(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
        }
        SafetyNet.Register("user windows", RestoreUserWindows);
        GetCursorPos(out var cursor0);
        SafetyNet.Register("cursor", () => SetCursorPos(cursor0.X, cursor0.Y));

        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY), ref mi);
        int wx = mi.rcWork.Left + 140, wy = mi.rcWork.Top + 340, ww = 260, wh = 200;
        int sx = wx + 110, sy = wy + 90;
        const uint WS_OVERLAPPEDWINDOW = 0x00CF0000;
        s_widget = new TestWindow(wx, wy, ww, wh, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, gdi: true, color: 0x00A0B020, title: "Atrium widget");
        ShowWindow(s_widget.Hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(s_widget.Hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // A normal, minimizable app window covering the widget.
        using var app = new TestWindow(wx - 40, wy - 40, ww + 80, wh + 80, 0, gdi: true, color: 0x00808080, title: "Atrium covering app", style: WS_OVERLAPPEDWINDOW);
        ShowWindow(app.Hwnd, SW_SHOW);
        app.AllowActivate = true;
        PumpFor(600);

        WinEventProc proc = OnEvent;
        var hook = SetWinEventHook(3, 3, IntPtr.Zero, proc, 0, 0, 0 /* OUTOFCONTEXT, EVENT_SYSTEM_FOREGROUND */);
        var hook2 = SetWinEventHook(0x16, 0x17, IntPtr.Zero, proc, 0, 0, 0 /* EVENT_SYSTEM_MINIMIZESTART..MINIMIZEEND */);
        SafetyNet.Register("hook", () => { UnhookWinEvent(hook); UnhookWinEvent(hook2); });
        var results = new JsonArray();
        var coverers = new List<IntPtr>();
        // The widget must be visible exactly when every app window covering it is
        // minimized (desktop shown) and hidden behind an app otherwise. The
        // expectation is derived from what Windows actually did, because Win+D is a
        // toggle whose direction depends on Windows' own show-desktop state.
        void Step(string name, Action act)
        {
            act();
            PumpFor(400);
            bool expectVisible = coverers.All(h => !IsWindowVisible(h) || IsIconic(h));
            double ms = WaitVisible(sx, sy, expectVisible);
            results.Add(new JsonObject { ["step"] = name, ["expectWidgetVisible"] = expectVisible, ["reactionMs"] = Math.Round(ms, 1), ["foreground"] = ClassOf(GetForegroundWindow()) });
            rep.Verdict(name, ms >= 0 ? "Passed" : "Failed", $"widget {(expectVisible ? "shown (desktop)" : "hidden behind the app")} as expected, settled {Math.Round(ms)} ms after the 400 ms observation delay; foreground {ClassOf(GetForegroundWindow())}");
            PumpFor(300);
        }
        coverers.Add(app.Hwnd);
        rep.Set("start.widgetCovered", Teal(sx, sy) < 0.1);

        // 1 + 2: Win+D and back.
        Step("win-d", WinD);
        Step("win-d-again", WinD);
        // 3: the taskbar's Show Desktop button (mouse path), via UI Automation invoke.
        Step("show-desktop-button", () => rep.Set("showDesktopButton", Uia("$_.Current.ClassName -eq 'SystemTray.ShowDesktopButton'")));
        // 4: clicking the widget itself keeps desktop mode.
        Step("click-widget", () => { Click(sx, sy); });
        // 5: restoring the minimized app from its taskbar button.
        // (A UIA Invoke on the grouped, auto-hidden taskbar button did not restore the
        // window on this build; SW_RESTORE raises the same MINIMIZEEND/FOREGROUND
        // events that a taskbar click or Alt+Tab produces.)
        Step("restore-app", () => { ShowWindow(app.Hwnd, 9 /* SW_RESTORE */); SetForegroundWindow(app.Hwnd); });
        // 6: launching a new app window while the desktop is shown.
        Step("win-d-before-launch", WinD);
        using var launched = new TestWindow(wx - 20, wy - 20, ww + 40, wh + 40, 0, gdi: true, color: 0x00505050, title: "Atrium launched app", style: WS_OVERLAPPEDWINDOW) { AllowActivate = true };
        coverers.Add(launched.Hwnd);
        Step("launch-app", () => { ShowWindow(launched.Hwnd, SW_SHOW); Click(sx, sy); });

        // 7: with desktop icons turned on.
        var defView = FindWindowEx(GetShellWindow(), IntPtr.Zero, "SHELLDLL_DefView", null);
        var list = FindWindowEx(defView, IntPtr.Zero, "SysListView32", null);
        bool iconsWereVisible = IsWindowVisible(list);
        rep.Set("desktopIconsInitiallyVisible", iconsWereVisible);
        void ToggleIcons() => SendMessage(defView, 0x0111 /* WM_COMMAND */, new IntPtr(0x7402), IntPtr.Zero);
        if (!iconsWereVisible)
        {
            SafetyNet.Register("desktop icons", () => { if (IsWindowVisible(list)) ToggleIcons(); });
            ToggleIcons();
            PumpFor(800);
        }
        rep.Set("desktopIconsVisibleForTest", IsWindowVisible(list));
        Step("icons-on-win-d", WinD);
        // Click empty desktop beside the widget: desktop mode (and the widget) stays.
        Step("icons-on-click-desktop", () => Click(wx + ww + 300, wy + wh + 200));
        Step("icons-on-win-d-again", WinD);
        Step("icons-on-win-d-third", WinD);
        if (!iconsWereVisible)
        {
            ToggleIcons();
            PumpFor(800);
            SafetyNet.Unregister("desktop icons");
        }
        rep.Set("desktopIconsRestored", IsWindowVisible(list) == iconsWereVisible);

        UnhookWinEvent(hook); UnhookWinEvent(hook2);
        SafetyNet.Unregister("hook");
        GC.KeepAlive(proc);
        rep.Set("steps", results);
        rep.Set("events", new JsonArray(s_events.Select(e => (JsonNode)$"{e.T:F0}ms {e.Event}").ToArray()));
        s_widget.Dispose();
        launched.Dispose(); app.Dispose();
        PumpFor(300);
        RestoreUserWindows();
        SafetyNet.Unregister("user windows");
        PumpFor(500);
        rep.Set("userWindowsRestored", userOpen.All(h => !IsWindow(h) || !IsIconic(h)));
        SetCursorPos(cursor0.X, cursor0.Y);
        for (int i = 0; i < 5 && GetForegroundWindow() != userFg; i++) { Keyboard.AltTap(); PumpFor(50); SetForegroundWindow(userFg); PumpFor(200); }
        rep.Set("focusRestoredToUser", GetForegroundWindow() == userFg);
        rep.Verdict("focus-restored", GetForegroundWindow() == userFg ? "Passed" : "Failed", "the user's foreground window is back after the test");
        rep.Verdict("icons-restored", (bool)rep.Root["desktopIconsRestored"]! ? "Passed" : "Failed", "desktop icon visibility is back to the user's setting");
        rep.Save();
    }
}
