namespace Atrium.Settings;

// The settings-model checks compile DesktopDisplay.cs without the WinUI app.
// It only needs the two lookup calls; English needs the real Loc in the app.
internal static class Loc
{
    public static string T(string text) => text;
    public static string F(string zh, params object?[] args) => string.Format(zh, args);
}
