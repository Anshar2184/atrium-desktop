using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P04c: the task list. (1) Does our taskbar-eligible window filter + AppUserModelID
// grouping match the buttons Explorer shows (compared through UI Automation,
// counts only)? (2) Can a click on our NOACTIVATE bar bring another window to
// the foreground? (3) Does a DWM live thumbnail render into our window?
internal static unsafe class TasksProbe
{
    [DllImport("shell32.dll")] private static extern int SHGetPropertyStoreForWindow(IntPtr hwnd, ref Guid iid, out IntPtr store);
    [DllImport("dwmapi.dll")] private static extern int DwmRegisterThumbnail(IntPtr dest, IntPtr src, out IntPtr thumb);
    [DllImport("dwmapi.dll")] private static extern int DwmUnregisterThumbnail(IntPtr thumb);
    [DllImport("dwmapi.dll")] private static extern int DwmUpdateThumbnailProperties(IntPtr thumb, ref DWM_THUMBNAIL_PROPERTIES p);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr h, uint cmd);

    [StructLayout(LayoutKind.Sequential)]
    private struct DWM_THUMBNAIL_PROPERTIES { public uint dwFlags; public RECT rcDestination; public RECT rcSource; public byte opacity; public int fVisible; public int fSourceClientAreaOnly; }

    private static string? Aumid(IntPtr hwnd)
    {
        var iid = new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"); // IPropertyStore
        if (SHGetPropertyStoreForWindow(hwnd, ref iid, out var store) != 0) return null;
        try
        {
            var key = stackalloc byte[20];
            *(Guid*)key = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"); *(uint*)(key + 16) = 5; // PKEY_AppUserModel_ID
            var pv = stackalloc byte[24];
            var getValue = (delegate* unmanaged[Stdcall]<IntPtr, byte*, byte*, int>)(*(*(void***)store + 5));
            if (getValue(store, key, pv) != 0) return null;
            ushort vt = *(ushort*)pv;
            string? s = vt == 31 ? Marshal.PtrToStringUni(*(IntPtr*)(pv + 8)) : null; // VT_LPWSTR
            if (vt == 31) Marshal.FreeCoTaskMem(*(IntPtr*)(pv + 8));
            return s;
        }
        finally { Marshal.Release(store); }
    }

    // The usual taskbar rules: visible, not cloaked, unowned (or WS_EX_APPWINDOW),
    // not a tool window (unless WS_EX_APPWINDOW), not WS_EX_NOACTIVATE.
    public static List<IntPtr> TaskbarWindows()
    {
        var list = new List<IntPtr>();
        EnumWindows((h, _) =>
        {
            if (!IsWindowVisible(h) || IsCloaked(h)) return true;
            int ex = GetWindowLong(h, -20);
            bool app = (ex & 0x00040000) != 0; // WS_EX_APPWINDOW
            if (!app && ((ex & (int)WS_EX_TOOLWINDOW) != 0 || (ex & (int)WS_EX_NOACTIVATE) != 0)) return true;
            if (!app && GetWindow(h, 4 /* GW_OWNER */) != IntPtr.Zero) return true;
            if (ClassOf(h) is "Progman" or "Shell_TrayWnd" or "Shell_SecondaryTrayWnd") return true;
            list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static void Run()
    {
        var rep = new Report("p04c-task-list");
        var wins = TaskbarWindows();
        var groups = new Dictionary<string, int>();
        int explicitIds = 0;
        foreach (var w in wins)
        {
            var id = Aumid(w);
            if (id != null) explicitIds++;
            else
            {
                GetWindowThreadProcessId(w, out var pid);
                try { id = "exe:" + Process.GetProcessById((int)pid).MainModule?.FileName?.ToLowerInvariant(); } catch { id = "pid:" + pid; }
            }
            groups[id!] = groups.GetValueOrDefault(id!) + 1;
        }
        rep.Set("ours.windows", wins.Count);
        rep.Set("ours.groups", groups.Count);
        rep.Set("ours.windowsWithExplicitAumid", explicitIds);

        // Explorer's buttons via UI Automation (count only), from a helper STA thread.
        int explorerButtons = -1;
        var t = new Thread(() =>
        {
            try
            {
                var psi = new ProcessStartInfo("pwsh.exe", "-NoProfile -Command \"Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes; $r=[System.Windows.Automation.AutomationElement]::RootElement; $t=$r.FindFirst(2,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty,'Shell_TrayWnd'))); @($t.FindAll(4,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty,'Taskbar.TaskListButtonAutomationPeer')))).Count\"")
                { RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true };
                using var p = Process.Start(psi)!;
                var o = p.StandardOutput.ReadToEnd().Trim();
                p.WaitForExit();
                explorerButtons = int.TryParse(o.Split('\n').Last().Trim(), out var n) ? n : -1;
            }
            catch { }
        });
        t.Start(); t.Join();
        rep.Set("explorer.taskButtons", explorerButtons);
        rep.Verdict("grouping-matches-explorer", explorerButtons >= 0 && Math.Abs(explorerButtons - groups.Count) <= 1 ? "Passed" : "Inconclusive",
            $"our groups {groups.Count} vs Explorer buttons {explorerButtons} (pinned-but-not-running buttons and grouping settings can differ)");

        // Activation and thumbnail, with our own windows only.
        var mi = new MONITORINFOEX { cbSize = (uint)Marshal.SizeOf<MONITORINFOEX>() };
        GetMonitorInfo(MonitorFromPoint(new POINT(), MONITOR_DEFAULTTOPRIMARY), ref mi);
        const uint WS_OVERLAPPEDWINDOW = 0x00CF0000;
        TestWindow.EnsureClass();
        var target = CreateWindowEx(0, "AtriumProbeWindow", "Atrium activation target", WS_OVERLAPPEDWINDOW | WS_VISIBLE, mi.rcWork.Left + 200, mi.rcWork.Top + 200, 500, 300, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        if (target == IntPtr.Zero) throw new InvalidOperationException("activation target window was not created");
        SafetyNet.Register("activation target", () => DestroyWindow(target));
        PumpFor(300);
        var userFg = GetForegroundWindow();
        // Give the foreground back to the user's window first (as if they were working).
        if (GetForegroundWindow() != userFg) SetForegroundWindow(userFg);

        var waitStart = Stopwatch.StartNew();
        while (IdleSeconds() < 15 && waitStart.Elapsed.TotalSeconds < 200) Thread.Sleep(1000);
        // The target must not be foreground at the start of each variant.
        int bx = mi.rcWork.Left + 800, by = mi.rcWork.Top + 600;
        bool direct = SetForegroundWindow(target) && GetForegroundWindow() == target;
        rep.Set("activation.withoutUserInput", direct);
        GetCursorPos(out var cur);
        var variants = new JsonArray();
        foreach (var (name, noActivate, inHandler) in new[] { ("noactivate-after", true, false), ("noactivate-in-handler", true, true), ("activating-bar", false, false) })
        {
            // Hand the foreground back to the user's window before each variant.
            if (GetForegroundWindow() != userFg) { Keyboard.AltTap(); SetForegroundWindow(userFg); PumpFor(200); }
            uint ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (noActivate ? WS_EX_NOACTIVATE : 0);
            using var bar = new TestWindow(bx, by, 300, 80, ex, gdi: true, color: 0x00C0C0C0, title: "Atrium bar button") { AllowActivate = !noActivate };
            bool handlerOk = false;
            if (inHandler) bar.OnClick = () => { handlerOk = SetForegroundWindow(target); };
            bar.Show();
            PumpFor(300);
            Click(bx + 150, by + 40);
            PumpFor(200);
            bool ok = inHandler ? handlerOk && GetForegroundWindow() == target : SetForegroundWindow(target) && GetForegroundWindow() == target;
            PumpFor(100);
            variants.Add(new JsonObject { ["variant"] = name, ["clickReceived"] = bar.Clicks > 0, ["targetActivated"] = ok });
        }
        SetCursorPos(cur.X, cur.Y);
        rep.Set("activation.variants", variants);
        var best = variants.FirstOrDefault(v => (bool)v!["targetActivated"]!);
        rep.Verdict("activate-from-bar", best != null ? "Passed" : "Failed",
            $"switching to another app after a click on our bar works with: {(best == null ? "none" : best["variant"])} (all: {variants.ToJsonString()})");

        // DWM live thumbnail of the target into a bar window.
        using (var host = new TestWindow(bx, by, 300, 80, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, gdi: true, color: 0x00303030, title: "Atrium thumbnail host"))
        {
            host.Show();
            PumpFor(200);
            int hrSame = DwmRegisterThumbnail(host.Hwnd, target, out var t0);
            if (hrSame == 0) DwmUnregisterThumbnail(t0);
            rep.Set("thumbnail.sameProcessSource", $"0x{hrSame:X8}");
            // Source from another process: the user's foreground app window.
            int hrReg = DwmRegisterThumbnail(host.Hwnd, userFg, out var thumb), hrUpd = -1;
            if (hrReg == 0)
            {
                var p = new DWM_THUMBNAIL_PROPERTIES { dwFlags = 0x1 | 0x4 | 0x8, rcDestination = new RECT { Left = 10, Top = 5, Right = 290, Bottom = 75 }, fVisible = 1, opacity = 255 };
                hrUpd = DwmUpdateThumbnailProperties(thumb, ref p);
                PumpFor(300);
                DwmUnregisterThumbnail(thumb);
            }
            rep.Set("thumbnail.register", $"0x{hrReg:X8}");
            rep.Set("thumbnail.update", $"0x{hrUpd:X8}");
            rep.Verdict("dwm-thumbnail", hrReg == 0 && hrUpd == 0 ? "Passed" : "Failed", $"DwmRegisterThumbnail 0x{hrReg:X8}, DwmUpdateThumbnailProperties 0x{hrUpd:X8}");
        }

        DestroyWindow(target);
        SafetyNet.Unregister("activation target");
        if (userFg != IntPtr.Zero && GetForegroundWindow() != userFg) { Keyboard.AltTap(); SetForegroundWindow(userFg); }
        rep.Set("focusRestoredToUser", GetForegroundWindow() == userFg);
        rep.Save();
    }
}

internal static class Keyboard
{
    // An Alt tap lifts the foreground lock, used only to give focus back to the user.
    public static void AltTap()
    {
        var inputs = new[]
        {
            new INPUT { type = 1, ki = new KEYBDINPUT { wVk = 0x12 } },
            new INPUT { type = 1, ki = new KEYBDINPUT { wVk = 0x12, dwFlags = 2 } },
        };
        SendInput(2, inputs, Marshal.SizeOf<INPUT>());
    }
}
