using System.Runtime.InteropServices;

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
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern int GetDIBits(IntPtr dc, IntPtr bitmap, uint start, uint lines, byte[] bits, ref BitmapInfoHeader info, uint usage);

    private const int SIIGBF_BIGGERSIZEOK = 0x1, SIIGBF_ICONONLY = 0x4;

    public static string ParsingName(PinnedApp pin) => pin.Path.Length > 0 ? pin.Path : "shell:AppsFolder\\" + pin.Aumid;

    public sealed record Image(int Width, int Height, byte[] Bgra);

    // Loads on a thread-pool (MTA) thread; null if the shell has no icon.
    public static Task<Image?> LoadAsync(string parsingName, int px) => Task.Run(() => Load(parsingName, px));

    private static Image? Load(string parsingName, int px)
    {
        var iid = typeof(IShellItemImageFactory).GUID;
        if (SHCreateItemFromParsingName(parsingName, IntPtr.Zero, ref iid, out var factory) != 0 || factory == null)
            return null;
        try
        {
            var size = new Size { Cx = px, Cy = px };
            if (factory.GetImage(size, SIIGBF_ICONONLY, out var hbitmap) != 0 && factory.GetImage(size, SIIGBF_BIGGERSIZEOK, out hbitmap) != 0)
                return null;
            try
            {
                // BIGGERSIZEOK may return another size: use the bitmap's own.
                if (GetObject(hbitmap, Marshal.SizeOf<Bitmap>(), out var bm) == 0 || bm.Width <= 0 || bm.Height <= 0) return null;
                var header = new BitmapInfoHeader { Size = (uint)Marshal.SizeOf<BitmapInfoHeader>(), Width = bm.Width, Height = -bm.Height, Planes = 1, BitCount = 32 };
                var pixels = new byte[bm.Width * bm.Height * 4];
                var dc = GetDC(IntPtr.Zero);
                int lines = GetDIBits(dc, hbitmap, 0, (uint)bm.Height, pixels, ref header, 0);
                ReleaseDC(IntPtr.Zero, dc);
                if (lines == 0) return null;
                // Bitmaps without an alpha channel are opaque.
                bool hasAlpha = false;
                for (int i = 3; i < pixels.Length && !hasAlpha; i += 4) hasAlpha = pixels[i] != 0;
                if (!hasAlpha)
                    for (int i = 3; i < pixels.Length; i += 4) pixels[i] = 255;
                return new Image(bm.Width, bm.Height, pixels);
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
}
