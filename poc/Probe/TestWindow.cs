using System.Runtime.InteropServices;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// A minimal top-level Win32 window used by the probes. It never activates and
// is destroyed on Dispose, so a probe can never leave a stray window behind.
internal sealed class TestWindow : IDisposable
{
    private static readonly WndProc s_proc = Proc;
    private static readonly Dictionary<IntPtr, TestWindow> s_windows = new();
    private static bool s_registered;
    private const string ClassName = "AtriumProbeWindow";

    public IntPtr Hwnd { get; }
    public uint GdiColor { get; set; } // 0x00BBGGRR; used when painted with GDI
    public bool PaintWithGdi { get; set; }
    public Func<int, int, int>? HitTest { get; set; }
    public int Clicks;
    public Action? OnClick;          // runs synchronously inside WM_LBUTTONDOWN
    public bool AllowActivate;       // answer WM_MOUSEACTIVATE with MA_ACTIVATE

    public TestWindow(int x, int y, int w, int h, uint exStyle, bool gdi, uint color = 0x00FF00FF, string title = "Atrium Probe")
    {
        Register();
        PaintWithGdi = gdi;
        GdiColor = color;
        Hwnd = CreateWindowEx(exStyle, ClassName, title, WS_POPUP, x, y, w, h, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        if (Hwnd == IntPtr.Zero) throw new InvalidOperationException($"CreateWindowEx failed: {Marshal.GetLastWin32Error()}");
        s_windows[Hwnd] = this;
    }

    public void Show()
    {
        ShowWindow(Hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(Hwnd);
    }

    public void Dispose()
    {
        if (IsWindow(Hwnd)) DestroyWindow(Hwnd);
        s_windows.Remove(Hwnd);
    }

    public static void EnsureClass() => Register();

    private static void Register()
    {
        if (s_registered) return;
        var wc = new WNDCLASSEX
        {
            cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(),
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(s_proc),
            hInstance = GetModuleHandle(null),
            lpszClassName = ClassName,
        };
        if (RegisterClassEx(ref wc) == 0) throw new InvalidOperationException($"RegisterClassEx failed: {Marshal.GetLastWin32Error()}");
        s_registered = true;
    }

    private static IntPtr Proc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l)
    {
        s_windows.TryGetValue(hwnd, out var self);
        switch (msg)
        {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT when self is { PaintWithGdi: true }:
                {
                    var hdc = BeginPaint(hwnd, out var ps);
                    GetClientRect(hwnd, out var rc);
                    var brush = CreateSolidBrush(self.GdiColor);
                    FillRect(hdc, ref rc, brush);
                    DeleteObject(brush);
                    EndPaint(hwnd, ref ps);
                    return IntPtr.Zero;
                }
            case WM_NCHITTEST when self?.HitTest != null:
                {
                    int sx = (short)(l.ToInt64() & 0xFFFF), sy = (short)((l.ToInt64() >> 16) & 0xFFFF);
                    GetWindowRect(hwnd, out var r);
                    return new IntPtr(self.HitTest(sx - r.Left, sy - r.Top));
                }
            case 0x0021 when self is { AllowActivate: true }:
                return new IntPtr(1); // MA_ACTIVATE
            case 0x0021: // WM_MOUSEACTIVATE: never take focus from the user's app
                return new IntPtr(3); // MA_NOACTIVATE
            case WM_LBUTTONDOWN when self != null:
                Interlocked.Increment(ref self.Clicks);
                self.OnClick?.Invoke();
                return IntPtr.Zero;
        }
        return DefWindowProc(hwnd, msg, w, l);
    }
}
