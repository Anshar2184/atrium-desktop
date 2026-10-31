using System.Diagnostics;
using System.Runtime.InteropServices;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// Isolated check of SendMessageTimeout timing against a target that sleeps
// inside its WM_COPYDATA handler (cross-process, using the fake tray).
internal static class SmtoProbe
{
    [DllImport("user32.dll", SetLastError = true, EntryPoint = "SendMessageTimeoutW")]
    private static extern IntPtr Smto(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);

    [DllImport("user32.dll", SetLastError = true, EntryPoint = "SendMessageTimeoutA")]
    private static extern IntPtr SmtoA(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);

    [StructLayout(LayoutKind.Sequential)]
    private struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }

    public static void SendTo(string[] a) => SendMessage(new IntPtr(long.Parse(a[2])), 0x0400 + 5, IntPtr.Zero, IntPtr.Zero);

    public static void Run()
    {
        var exe = Environment.ProcessPath!;
        using var fake = TrayClientProc.StartRaw(exe, "faketray 120");
        var target = new IntPtr(fake.Hwnd);
        var payload = new byte[8 + 956];
        BitConverter.GetBytes(0x34753423).CopyTo(payload, 0);
        BitConverter.GetBytes(1).CopyTo(payload, 4); // NIM_MODIFY
        var h = GCHandle.Alloc(payload, GCHandleType.Pinned);
        var cds = new COPYDATASTRUCT { dwData = new IntPtr(1), cbData = payload.Length, lpData = h.AddrOfPinnedObject() };
        var pc = Marshal.AllocHGlobal(Marshal.SizeOf<COPYDATASTRUCT>());
        Marshal.StructureToPtr(cds, pc, false);
        foreach (var (name, flags, msg) in new (string, uint, uint)[] { ("copydata normal", 0, 0x4A), ("copydata abortifhung", 2, 0x4A), ("copydata abortifhung|erroronexit", 0x22, 0x4A), ("user msg normal", 0, 0x0400 + 999) })
        {
            fake.Command("HANG 2000", "HANG", 3000);
            if (msg != 0x4A) { /* the fake only sleeps on WM_COPYDATA; send a copydata first to trigger */ }
            var sw = Stopwatch.StartNew();
            var r = Smto(target, msg, IntPtr.Zero, msg == 0x4A ? pc : IntPtr.Zero, flags, 500, out var res);
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine($"{name}: returned={r} err={err} ms={sw.Elapsed.TotalMilliseconds:F0}");
            PumpFor(2500);
        }
        // Nested: the SMTO happens inside the handler of a message that another
        // thread sent to us (this is the tray bridge situation).
        foreach (var (name, entryW) in new[] { ("nested same-process W", true), ("nested cross-process A", true) })
        {
            TestWindow.EnsureClass();
            using var relay = new TestWindow(0, 0, 1, 1, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, gdi: false, title: "smto relay");
            double inner = -1; IntPtr innerRet = IntPtr.Zero; int innerErr = 0;
            relay.OnUser = () =>
            {
                var sw2 = Stopwatch.StartNew();
                innerRet = entryW ? Smto(target, 0x4A, IntPtr.Zero, pc, 0x22, 500, out _) : SmtoA(target, 0x4A, IntPtr.Zero, pc, 0x22, 500, out _);
                innerErr = Marshal.GetLastWin32Error();
                inner = sw2.Elapsed.TotalMilliseconds;
            };
            fake.Command("HANG 2000", "HANG", 3000);
            var hw = relay.Hwnd;
            var swo = Stopwatch.StartNew();
            if (name.EndsWith("A"))
            {
                // Cross-process sender this time.
                var child = Process.Start(new ProcessStartInfo(exe, $"sendto 20 {hw.ToInt64()}") { UseShellExecute = false, CreateNoWindow = true })!;
                while (!child.HasExited) { Pump(); Thread.Sleep(1); }
            }
            else
            {
                var sender = new Thread(() => SendMessage(hw, 0x0400 + 5, IntPtr.Zero, IntPtr.Zero));
                sender.Start();
                while (sender.IsAlive) { Pump(); Thread.Sleep(1); }
            }
            Console.WriteLine($"{name}: inner returned={innerRet} err={innerErr} innerMs={inner:F0} outerMs={swo.Elapsed.TotalMilliseconds:F0}");
            PumpFor(2500);
        }
        Marshal.FreeHGlobal(pc); h.Free();
        fake.Command("QUIT", "", 200);
    }
}
