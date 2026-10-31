using System.Runtime.InteropServices;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// A controlled notification-area client used by the tray probe, so the probe
// never has to touch real applications' icons. Commands on stdin: RECT, QUIT.
internal static unsafe class TrayClient
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NOTIFYICONDATA
    {
        public uint cbSize;
        public IntPtr hWnd;
        public uint uID;
        public uint uFlags;
        public uint uCallbackMessage;
        public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
        public uint dwState;
        public uint dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
        public uint uVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
        public uint dwInfoFlags;
        public Guid guidItem;
        public IntPtr hBalloonIcon;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NOTIFYICONIDENTIFIER { public uint cbSize; public IntPtr hWnd; public uint uID; public Guid guidItem; }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern bool Shell_NotifyIcon(uint msg, ref NOTIFYICONDATA data);
    [DllImport("shell32.dll")] private static extern int Shell_NotifyIconGetRect(ref NOTIFYICONIDENTIFIER id, out RECT rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll")] private static extern IntPtr LoadIcon(IntPtr inst, IntPtr name);

    private const uint NIM_ADD = 0, NIM_DELETE = 2, NIM_SETVERSION = 4, NIF_MESSAGE = 1, NIF_ICON = 2, NIF_TIP = 4;
    private const uint CallbackMsg = 0x0400 + 200;
    private static uint s_taskbarCreated;
    private static bool s_readd;
    private static WndProc? s_proc;
    private static NOTIFYICONDATA s_nid;

    public static void Run(string[] a)
    {
        s_readd = a.Length > 2 && a[2] == "1";
        string tip = a.Length > 3 ? a[3] : "Atrium test icon";
        s_taskbarCreated = RegisterWindowMessage("TaskbarCreated");
        s_proc = Proc;
        var wc = new WNDCLASSEX { cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(), lpfnWndProc = Marshal.GetFunctionPointerForDelegate(s_proc), hInstance = GetModuleHandle(null), lpszClassName = "AtriumTrayClient" };
        RegisterClassEx(ref wc);
        var hwnd = CreateWindowEx(0, "AtriumTrayClient", tip, 0, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        s_nid = new NOTIFYICONDATA
        {
            cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATA>(), hWnd = hwnd, uID = 1, uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
            uCallbackMessage = CallbackMsg, hIcon = LoadIcon(IntPtr.Zero, new IntPtr(32516)), szTip = tip, szInfo = "", szInfoTitle = "",
        };
        bool deferAdd = a.Length > 4 && a[4] == "noadd";
        bool added = !deferAdd && Shell_NotifyIcon(NIM_ADD, ref s_nid);
        s_nid.uVersion = 4;
        if (!deferAdd) Shell_NotifyIcon(NIM_SETVERSION, ref s_nid);
        Console.WriteLine($"HWND {hwnd.ToInt64()} ADDED {added}");
        Console.Out.Flush();
        string? cmd = null;
        var reader = new Thread(() => { string? l; while ((l = Console.ReadLine()) != null) Volatile.Write(ref cmd, l); }) { IsBackground = true };
        reader.Start();
        var end = Environment.TickCount64 + 120000;
        while (Environment.TickCount64 < end)
        {
            Pump();
            var c = Interlocked.Exchange(ref cmd, null);
            if (c == "QUIT") break;
            if (c == "ADD")
            {
                var t = System.Diagnostics.Stopwatch.StartNew();
                bool ok = Shell_NotifyIcon(NIM_ADD, ref s_nid);
                Console.WriteLine($"ADD ok={ok} ms={t.Elapsed.TotalMilliseconds:F0}");
                Console.Out.Flush();
            }
            if (c != null && c.StartsWith("BURST "))
            {
                // NIM_MODIFY n times with a sequence number in the tooltip.
                int n = int.Parse(c[6..]), ok = 0, fail = 0; double max = 0;
                var nid = s_nid; nid.uFlags = NIF_TIP;
                for (int i = 0; i < n; i++)
                {
                    nid.szTip = "seq " + i;
                    var t = System.Diagnostics.Stopwatch.StartNew();
                    if (Shell_NotifyIcon(1 /* NIM_MODIFY */, ref nid)) ok++; else fail++;
                    max = Math.Max(max, t.Elapsed.TotalMilliseconds);
                }
                Console.WriteLine($"BURST ok={ok} fail={fail} maxms={max:F0}");
                Console.Out.Flush();
            }
            if (c == "RECT")
            {
                var id = new NOTIFYICONIDENTIFIER { cbSize = (uint)Marshal.SizeOf<NOTIFYICONIDENTIFIER>(), hWnd = hwnd, uID = 1 };
                int hr = Shell_NotifyIconGetRect(ref id, out var r);
                Console.WriteLine($"RECT hr=0x{hr:X8} {r}");
                Console.Out.Flush();
            }
            Thread.Sleep(5);
        }
        Shell_NotifyIcon(NIM_DELETE, ref s_nid);
        Console.WriteLine("DELETED");
    }

    private static IntPtr Proc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l)
    {
        if (msg == CallbackMsg)
        {
            Console.WriteLine($"CB 0x{(l.ToInt64() & 0xFFFF):X}");
            Console.Out.Flush();
            return IntPtr.Zero;
        }
        if (msg == s_taskbarCreated && s_taskbarCreated != 0)
        {
            Console.WriteLine("TASKBARCREATED");
            if (s_readd)
            {
                bool ok = Shell_NotifyIcon(NIM_ADD, ref s_nid);
                Shell_NotifyIcon(NIM_SETVERSION, ref s_nid);
                Console.WriteLine("READD " + ok);
            }
            Console.Out.Flush();
            return IntPtr.Zero;
        }
        return DefWindowProc(hwnd, msg, w, l);
    }
}
