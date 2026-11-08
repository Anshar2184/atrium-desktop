namespace Atrium.Settings;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        // A BITS completion launches the same executable without creating XAML,
        // a window, a graphics device, or the settings-window singleton.
        if (args.Length > 0 && args[0] is "--queue-update" or "--finish-update" or "--resume-update")
            return Task.Run(() => ReleaseInstaller.RunBackground(args)).GetAwaiter().GetResult();
        XamlGeneratedProgram.XamlGeneratedMain();
        return 0;
    }
}
