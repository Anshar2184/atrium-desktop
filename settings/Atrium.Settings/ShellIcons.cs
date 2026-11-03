using System.Runtime.InteropServices;
using System.ComponentModel;
using System.Diagnostics;

namespace Atrium.Settings;

// App icons the way the bar draws them (native/core/shell/task_model.cpp):
// IShellItemImageFactory on the .lnk/.exe or on shell:AppsFolder\<AUMID>, icon
// only, so shortcuts get no arrow overlay. Returns premultiplied BGRA.
internal static class ShellIcons
{
    [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItemImageFactory
    {
        [PreserveSig] int GetImage(Size size, int flags, out IntPtr bitmap);
    }

    [StructLayout(LayoutKind.Sequential)] private struct Size { public int Cx, Cy; }

    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfoHeader
    {
        public uint Size;
        public int Width, Height;
        public ushort Planes, BitCount;
        public uint Compression, SizeImage;
        public int XPelsPerMeter, YPelsPerMeter;
        public uint ClrUsed, ClrImportant;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    private static extern int SHCreateItemFromParsingName(string path, IntPtr bindContext, ref Guid iid, out IShellItemImageFactory factory);
    [StructLayout(LayoutKind.Sequential)]
    private struct Bitmap
    {
        public int Type, Width, Height, WidthBytes;
        public ushort Planes, BitsPixel;
        public IntPtr Bits;
    }

    [DllImport("gdi32.dll")] private static extern int GetObject(IntPtr obj, int size, out Bitmap bitmap);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr obj);
    [StructLayout(LayoutKind.Sequential)]
    private struct IconInfo { public int IsIcon; public uint HotX, HotY; public IntPtr Mask, Color; }
    [DllImport("user32.dll")] private static extern bool GetIconInfo(IntPtr icon, out IconInfo info);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(IntPtr icon);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHDefExtractIconW(string path, int index, uint flags, out IntPtr large, IntPtr small, uint size);
    [DllImport("ole32.dll")] private static extern int CoInitializeEx(IntPtr reserved, uint mode);
    [DllImport("ole32.dll")] private static extern void CoUninitialize();
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern int GetDIBits(IntPtr dc, IntPtr bitmap, uint start, uint lines, byte[] bits, ref BitmapInfoHeader info, uint usage);

    private const int SIIGBF_BIGGERSIZEOK = 0x1, SIIGBF_ICONONLY = 0x4;

    public static string ParsingName(PinnedApp pin) => pin.Path.Length > 0 ? pin.Path : "shell:AppsFolder\\" + pin.Aumid;

    public sealed record Image(int Width, int Height, byte[] Bgra);

    // Loads on a thread-pool (MTA) thread; null if the shell has no icon.
    public static Task<Image?> LoadAsync(string parsingName, int px) => Task.Run(() =>
    {
        int hr = CoInitializeEx(IntPtr.Zero, 0);
        try { return Load(parsingName, px); }
        finally { if (hr >= 0) CoUninitialize(); }
    });

    private static Image? Load(string parsingName, int px)
    {
        parsingName = ExecutableForIcon(parsingName);
        // Read executable artwork directly, avoiding the Shell's generic file-type cache.
        if (parsingName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) &&
            SHDefExtractIconW(parsingName, 0, 0, out var icon, IntPtr.Zero, (uint)px) == 0)
        {
            try
            {
                if (GetIconInfo(icon, out var info))
                {
                    try { if (info.Color != IntPtr.Zero && ReadBitmap(info.Color) is { } image) return image; }
                    finally { if (info.Color != IntPtr.Zero) DeleteObject(info.Color); if (info.Mask != IntPtr.Zero) DeleteObject(info.Mask); }
                }
            }
            finally { DestroyIcon(icon); }
        }
        var iid = typeof(IShellItemImageFactory).GUID;
        if (SHCreateItemFromParsingName(parsingName, IntPtr.Zero, ref iid, out var factory) != 0 || factory == null)
            return null;
        try
        {
            var size = new Size { Cx = px, Cy = px };
            if (factory.GetImage(size, SIIGBF_ICONONLY, out var hbitmap) != 0 && factory.GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, out hbitmap) != 0)
                return null;
            try
            {
                return ReadBitmap(hbitmap);
            }
            finally
            {
                DeleteObject(hbitmap);
            }
        }
        finally
        {
            Marshal.ReleaseComObject(factory);
        }
    }

    private static Image? ReadBitmap(IntPtr bitmap)
    {
        if (GetObject(bitmap, Marshal.SizeOf<Bitmap>(), out var bm) == 0 || bm.Width <= 0 || bm.Height <= 0) return null;
        var header = new BitmapInfoHeader { Size = (uint)Marshal.SizeOf<BitmapInfoHeader>(), Width = bm.Width, Height = -bm.Height, Planes = 1, BitCount = 32 };
        var pixels = new byte[bm.Width * bm.Height * 4];
        var dc = GetDC(IntPtr.Zero);
        int lines = GetDIBits(dc, bitmap, 0, (uint)bm.Height, pixels, ref header, 0);
        ReleaseDC(IntPtr.Zero, dc);
        if (lines == 0) return null;
        bool hasAlpha = false;
        for (int i = 3; i < pixels.Length && !hasAlpha; i += 4) hasAlpha = pixels[i] != 0;
        if (!hasAlpha)
            for (int i = 3; i < pixels.Length; i += 4) pixels[i] = 255;
        return new Image(bm.Width, bm.Height, pixels);
    }

    // Versioned clients keep a small launcher beside app\ or a version folder.
    // Use that same installation's running binary for artwork, preserving the pin's launch target.
    private static string ExecutableForIcon(string path)
    {
        if (!path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) return path;
        string directory = System.IO.Path.GetDirectoryName(path)! + System.IO.Path.DirectorySeparatorChar;
        var processes = Process.GetProcessesByName(System.IO.Path.GetFileNameWithoutExtension(path));
        try
        {
            foreach (var process in processes)
            {
                try
                {
                    string? actual = process.MainModule?.FileName;
                    if (actual != null && !actual.Equals(path, StringComparison.OrdinalIgnoreCase) && actual.StartsWith(directory, StringComparison.OrdinalIgnoreCase))
                        return actual;
                }
                catch (Win32Exception) { }
                catch (InvalidOperationException) { } // the process exited while its icon was loading
            }
        }
        finally { foreach (var process in processes) process.Dispose(); }
        return path;
    }
}
