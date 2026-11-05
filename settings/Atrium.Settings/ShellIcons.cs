using System.Runtime.InteropServices;

namespace Atrium.Settings;

// App icons the way the bar draws them (native/core/shell/task_model.cpp):
// IShellItemImageFactory on the .lnk/.exe or on shell:AppsFolder\<AUMID>, icon
// only, so shortcuts get no arrow overlay. Returns premultiplied BGRA.
internal static class ShellIcons
{
    [StructLayout(LayoutKind.Sequential)] private struct PropertyKey { public Guid Format; public uint Id; }
    [StructLayout(LayoutKind.Explicit, Size = 24)] private struct PropVariant
    {
        [FieldOffset(0)] public ushort Type;
        [FieldOffset(8)] public IntPtr Text;
    }
    [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore
    {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int GetAt(uint index, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
        [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int Commit();
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHGetPropertyStoreFromParsingName(string path, IntPtr context, uint flags, ref Guid iid, out IPropertyStore store);
    [DllImport("ole32.dll")] private static extern int PropVariantClear(ref PropVariant value);
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, string> LinkTargets = new(StringComparer.OrdinalIgnoreCase);

    internal static string ExecutablePath(string path)
    {
        if (!path.EndsWith(".lnk", StringComparison.OrdinalIgnoreCase)) return ExecutableForIcon(path);
        return LinkTargets.GetOrAdd(path, static link =>
        {
            var iid = typeof(IPropertyStore).GUID;
            if (SHGetPropertyStoreFromParsingName(link, IntPtr.Zero, 0, ref iid, out var store) != 0) return link;
            try
            {
                var key = new PropertyKey { Format = new Guid("b9b4b3fc-2b51-4a42-b5d8-324146afcf25"), Id = 2 };
                if (store.GetValue(ref key, out var value) != 0) return link;
                try { return value.Type == 31 ? ExecutableForIcon(Marshal.PtrToStringUni(value.Text) ?? link) : link; }
                finally { PropVariantClear(ref value); }
            }
            finally { Marshal.ReleaseComObject(store); }
        });
    }
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
        // Executable artwork can use straight alpha; XAML's bitmap buffer expects
        // premultiplied BGRA, as the native taskbar does. Otherwise its edges glow white.
        bool hasAlpha = false, straight = false;
        for (int i = 0; i < pixels.Length; i += 4)
        {
            byte a = pixels[i + 3];
            hasAlpha |= a != 0;
            straight |= pixels[i] > a || pixels[i + 1] > a || pixels[i + 2] > a;
        }
        if (!hasAlpha)
            for (int i = 3; i < pixels.Length; i += 4) pixels[i] = 255;
        else if (straight)
            for (int i = 0; i < pixels.Length; i += 4)
            {
                int a = pixels[i + 3];
                for (int c = 0; c < 3; c++) pixels[i + c] = (byte)((pixels[i + c] * a + 127) / 255);
            }
        return new Image(bm.Width, bm.Height, pixels);
    }

    // Read the installed client's artwork without depending on a running process.
    private static string ExecutableForIcon(string path)
    {
        if (!path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) return path;
        string actual = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(path)!, "app", System.IO.Path.GetFileName(path));
        return System.IO.File.Exists(actual) ? actual : path;
    }
}
