using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;

namespace Atrium.Settings;

public partial class App : Application
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string? cls, string title);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hwnd, int cmd);

    public const string WindowTitle = "Atrium 设置";
    private static Mutex? s_instance;
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Single instance: a second launch (tray menu, double click) brings the
        // existing window to the front instead of opening another one.
        s_instance = new Mutex(true, @"Local\Atrium.Settings", out bool created);
        if (!created)
        {
            var other = FindWindow(null, WindowTitle);
            if (other != IntPtr.Zero)
            {
                ShowWindow(other, 9 /* SW_RESTORE */);
                SetForegroundWindow(other);
            }
            Exit();
            return;
        }
        _window = new MainWindow();
        _window.Activate();
    }
}
