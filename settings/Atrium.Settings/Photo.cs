using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace Atrium.Settings;

// The photograph behind every preview (the hero, the preset swatches, the widget gallery):
// Assets\specimen.jpg when the build has one (a wallpaper kept out of the repository, see
// .gitignore), else the desktop's own wallpaper, else a quiet grey. Decoded once, on first use,
// at most 2048 px wide; first use is on a worker (InitHero), never on the UI thread.
internal sealed class Photo
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool SystemParametersInfoW(uint action, uint param, StringBuilder value, uint winIni);

    // How far down the photo the crop sits when it is cropped top and bottom (0 top, 1 bottom).
    public const float FocusY = 0.62f;
    private const int MaxWidth = 2048;

    public static string? Path { get; } = Find();
    private static readonly Lazy<Photo> s_current = new(Load, LazyThreadSafetyMode.ExecutionAndPublication);
    public static Photo Current => s_current.Value;

    private readonly int _w, _h;
    private readonly byte[]? _bgra; // null: no picture, a flat grey

    private Photo(int w, int h, byte[]? bgra) { _w = w; _h = h; _bgra = bgra; }

    private static string? Find()
    {
        var asset = System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "specimen.jpg");
        if (File.Exists(asset)) return asset;
        var sb = new StringBuilder(520);
        if (SystemParametersInfoW(0x0073 /* SPI_GETDESKWALLPAPER */, (uint)sb.Capacity, sb, 0) && File.Exists(sb.ToString())) return sb.ToString();
        var transcoded = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Microsoft", "Windows", "Themes", "TranscodedWallpaper");
        return File.Exists(transcoded) ? transcoded : null;
    }

    private static Photo Load()
    {
        if (Path == null) return new Photo(1, 1, null);
        try
        {
            var file = StorageFile.GetFileFromPathAsync(Path).AsTask().GetAwaiter().GetResult();
            using var stream = file.OpenReadAsync().AsTask().GetAwaiter().GetResult();
            var decoder = BitmapDecoder.CreateAsync(stream).AsTask().GetAwaiter().GetResult();
            double k = Math.Min(1.0, (double)MaxWidth / decoder.OrientedPixelWidth);
            // The transform scales the stored picture and turns it upright afterwards (EXIF).
            var t = new BitmapTransform
            {
                ScaledWidth = (uint)Math.Max(1, Math.Round(decoder.PixelWidth * k)),
                ScaledHeight = (uint)Math.Max(1, Math.Round(decoder.PixelHeight * k)),
                InterpolationMode = BitmapInterpolationMode.Fant,
            };
            bool turned = decoder.OrientedPixelWidth != decoder.PixelWidth;
            var data = decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore, t,
                ExifOrientationMode.RespectExifOrientation, ColorManagementMode.DoNotColorManage).AsTask().GetAwaiter().GetResult();
            return turned ? new Photo((int)t.ScaledHeight, (int)t.ScaledWidth, data.DetachPixelData())
                          : new Photo((int)t.ScaledWidth, (int)t.ScaledHeight, data.DetachPixelData());
        }
        catch (Exception e)
        {
            System.Diagnostics.Debug.WriteLine("specimen photo unreadable: " + e.Message);
            return new Photo(1, 1, null);
        }
    }

    // The photo covering an art of artW x artH DIP (cropped, never letterboxed), painted for a
    // pw x ph pixel viewport whose top-left is (x0, y0) DIP, at ppd pixels per DIP. RGBA floats.
    public void Paint(float[] rgba, int pw, int ph, float artW, float artH, float x0, float y0, float ppd)
    {
        float s = MathF.Max(artW / _w, artH / _h); // DIP per photo pixel
        float ox = (artW - _w * s) * 0.5f, oy = (artH - _h * s) * FocusY;
        Parallel.For(0, ph, j =>
        {
            float v = (y0 + (j + 0.5f) / ppd - oy) / s;
            for (int i = 0; i < pw; i++)
            {
                float u = (x0 + (i + 0.5f) / ppd - ox) / s;
                Vector3 c = Sample(u, v);
                int o = (j * pw + i) * 4;
                rgba[o] = c.X; rgba[o + 1] = c.Y; rgba[o + 2] = c.Z; rgba[o + 3] = 1;
            }
        });
    }

    // Bilinear, clamped; (u, v) in photo pixels.
    private Vector3 Sample(float u, float v)
    {
        if (_bgra == null) return new Vector3(0.80f, 0.83f, 0.87f);
        u -= 0.5f; v -= 0.5f;
        float fu = MathF.Floor(u), fv = MathF.Floor(v), tu = u - fu, tv = v - fv;
        int x0 = Math.Clamp((int)fu, 0, _w - 1), y0 = Math.Clamp((int)fv, 0, _h - 1);
        int x1 = Math.Min(x0 + 1, _w - 1), y1 = Math.Min(y0 + 1, _h - 1);
        Vector3 a = Px(x0, y0), b = Px(x1, y0), c = Px(x0, y1), d = Px(x1, y1);
        a += (b - a) * tu;
        c += (d - c) * tu;
        return a + (c - a) * tv;
    }

    private Vector3 Px(int x, int y)
    {
        int i = (y * _w + x) * 4;
        return new Vector3(_bgra![i + 2], _bgra[i + 1], _bgra[i]) * (1f / 255f);
    }
}
