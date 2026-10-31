using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P03c: with the Explorer taskbar hidden, can Start, Search, Quick Settings and
// the Notification Center still be opened via their public shortcuts, where do
// they appear, and does Explorer re-show its taskbar? Also: does an auto-hide
// edge hover bring the hidden taskbar back? Every panel is closed with Esc.
internal static class ShellEntryProbe
{
    private const ushort VK_LWIN = 0x5B, VK_ESCAPE = 0x1B, VK_S = 0x53, VK_A = 0x41, VK_N = 0x4E;

    private static void Keys(params (ushort Vk, bool Up)[] seq)
    {
        var inputs = seq.Select(k => new INPUT { type = 1, ki = new KEYBDINPUT { wVk = k.Vk, dwFlags = k.Up ? 2u : 0u } }).ToArray();
        SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
    }

    private static void Chord(ushort? second)
    {
        if (second == null) Keys((VK_LWIN, false), (VK_LWIN, true));
        else Keys((VK_LWIN, false), (second.Value, false), (second.Value, true), (VK_LWIN, true));
    }

    private static JsonObject Foreground()
    {
        var fg = GetForegroundWindow();
        GetWindowThreadProcessId(fg, out var pid);
        string proc = "?";
        try { proc = Process.GetProcessById((int)pid).ProcessName; } catch { }
        const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
        DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, out RECT r, Marshal.SizeOf<RECT>());
        return new JsonObject { ["process"] = proc, ["class"] = ClassOf(fg), ["rect"] = r.ToString() };
    }

    private static bool ModifiersUp() =>
        new[] { 0x10, 0x11, 0x12, 0x5B, 0x5C }.All(vk => (GetAsyncKeyState(vk) & 0x8000) == 0);

    public static void Run()
    {
        var rep = new Report("p03c-native-entry-points");
        var waitStart = Stopwatch.StartNew();
        while ((IdleSeconds() < 15 || !ModifiersUp()) && waitStart.Elapsed.TotalSeconds < 240) Thread.Sleep(1000);
        rep.Set("idleSecondsBefore", Math.Round(IdleSeconds(), 1));
        if (IdleSeconds() < 15) { rep.Verdict("entry-points", "Inconclusive", "user was active"); rep.Save(); return; }

        var before = TaskbarProbe.Snapshot();
        SafetyNet.Register("taskbar visibility", () => TaskbarProbe.Restore(before));
        GetCursorPos(out var cursor0);
        SafetyNet.Register("cursor", () => SetCursorPos(cursor0.X, cursor0.Y));
        var userFg = Foreground();
        var fgUser = GetForegroundWindow();
        rep.Set("foregroundBefore.process", userFg["process"]!.DeepClone());

        var cases = new (string Name, ushort? Key)[] { ("Start (Win)", null), ("Search (Win+S)", VK_S), ("Quick Settings (Win+A)", VK_A), ("Notification Center (Win+N)", VK_N) };
        var results = new JsonArray();
        foreach (bool hidden in new[] { false, true })
        {
            if (hidden) foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_HIDE);
            PumpFor(500);
            foreach (var c in cases)
            {
                if (IdleSeconds() < 1.5 && IdleSeconds() > 0.3) { PumpFor(2000); }
                var fgBefore = GetForegroundWindow();
                Chord(c.Key);
                PumpFor(1300);
                var opened = Foreground();
                bool changed = GetForegroundWindow() != fgBefore;
                bool taskbarShown = TaskbarProbe.Taskbars().Any(IsWindowVisible);
                Keys((VK_ESCAPE, false), (VK_ESCAPE, true));
                PumpFor(700);
                bool closed = GetForegroundWindow() != IntPtr.Zero && Foreground()["process"]!.ToString() != opened["process"]!.ToString();
                var afterEsc = GetForegroundWindow();
                bool focusBack = afterEsc == fgBefore;
                string focusClass = ClassOf(afterEsc);
                bool focusVisible = IsWindowVisible(afterEsc);
                // Put focus back for the next case (Alt tap lifts the foreground lock).
                if (!focusBack) { Keys((0x12, false), (0x12, true)); SetForegroundWindow(fgBefore); PumpFor(200); }
                results.Add(new JsonObject
                {
                    ["taskbarHidden"] = hidden,
                    ["entry"] = c.Name,
                    ["opened"] = changed,
                    ["panel"] = opened,
                    ["explorerTaskbarReappeared"] = hidden && taskbarShown,
                    ["closedWithEsc"] = closed,
                    ["focusReturnedToApp"] = focusBack,
                    ["focusAfterEsc"] = focusClass + (focusVisible ? "" : " (invisible)"),
                });
                if (hidden && taskbarShown) foreach (var t in TaskbarProbe.Taskbars()) ShowWindow(t, SW_HIDE);
            }
        }
        rep.Set("cases", results);

        // Auto-hide edge hover while the taskbar is hidden.
        bool revealed = false;
        if (before["autoHide"]!.GetValue<bool>())
        {
            var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
            GetMonitorInfo(MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY), ref mi);
            SetCursorPos(mi.rcMonitor.Left + mi.rcMonitor.Width / 2, mi.rcMonitor.Bottom - 1);
            for (int i = 0; i < 20; i++) { PumpFor(100); revealed |= TaskbarProbe.Taskbars().Any(IsWindowVisible); }
            SetCursorPos(cursor0.X, cursor0.Y);
            rep.Set("edgeHover.taskbarReappeared", revealed);
        }

        TaskbarProbe.Restore(before);
        SetCursorPos(cursor0.X, cursor0.Y);
        if (GetForegroundWindow() != fgUser) { Keys((0x12, false), (0x12, true)); SetForegroundWindow(fgUser); }
        PumpFor(500);
        SafetyNet.Unregister("taskbar visibility");
        var after = TaskbarProbe.Snapshot();
        rep.Set("after", after);

        foreach (var c in cases)
        {
            var h = results.First(r => (bool)r!["taskbarHidden"]! && (string)r!["entry"]! == c.Name)!;
            var v = results.First(r => !(bool)r!["taskbarHidden"]! && (string)r!["entry"]! == c.Name)!;
            bool ok = (bool)h["opened"]! && (bool)h["closedWithEsc"]!;
            rep.Verdict("hidden:" + c.Name, ok ? "Passed" : "Failed",
                $"opens with taskbar hidden: {h["opened"]} ({h["panel"]!["process"]} at {h["panel"]!["rect"]}; with taskbar visible at {v["panel"]!["rect"]}); Explorer taskbar reappeared: {h["explorerTaskbarReappeared"]}");
        }
        if (before["autoHide"]!.GetValue<bool>())
            rep.Verdict("edge-hover", revealed ? "Failed" : "Passed", $"auto-hide edge hover re-shows the hidden Explorer taskbar: {revealed}");
        foreach (var r in results)
            if (!(bool)r!["focusReturnedToApp"]!)
                rep.Verdict($"focus:{r["entry"]}{((bool)r["taskbarHidden"]! ? " (taskbar hidden)" : "")}", "Failed", $"after Esc focus went to {r["focusAfterEsc"]}, not back to the app");
        rep.Set("focusRestoredToUser", GetForegroundWindow() == fgUser);
        rep.Verdict("state-restored", JsonNode.DeepEquals(after, before) ? "Passed" : "Failed", "taskbar state equals the snapshot");
        rep.Save();
    }
}
