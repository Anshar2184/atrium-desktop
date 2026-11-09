#if ATRIUM_DEVELOPMENT
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;

namespace Atrium.Settings;

// Development only: `Atrium.Settings.exe --export-stage-art <folder>` draws the Stage Manager
// page's windows (1280 x 800 px) and icons (192 x 192 px) from their elements and saves them as
// PNG, for assets/stage. The page itself only shows those files.
public sealed partial class MainWindow
{
    private async void ExportStageArtIfAsked()
    {
        var args = Environment.GetCommandLineArgs();
        int at = Array.IndexOf(args, "--export-stage-art");
        if (at < 0 || at + 1 >= args.Length) return;
        string folder = args[at + 1];
        Directory.CreateDirectory(folder);
        double rs = Content.XamlRoot?.RasterizationScale ?? 1;
        // Out of the window while drawn (RootGrid is a Grid: moved by a transform).
        var host = new Canvas
        {
            IsHitTestVisible = false, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
            RenderTransform = new TranslateTransform { X = -8000 },
        };
        RootGrid.Children.Insert(0, host);
        for (int a = 0; a < StageApps.Length; a++)
        {
            await Save(StageWindowLive(StageApps[a], 1280 / rs, 800 / rs, 32 / rs), Path.Combine(folder, $"window-{a}.png"));
            await Save(StageIconLive(a, 192 / rs), Path.Combine(folder, $"icon-{a}.png"));
        }
        RootGrid.Children.Remove(host);
        Application.Current.Exit();

        // Drawn once it has been on screen for a frame, and again while not whole (its middle is
        // opaque in a finished one).
        async Task Save(FrameworkElement live, string path)
        {
            host.Children.Add(live);
            live.UpdateLayout();
            RenderTargetBitmap shot = new();
            byte[] pixels = Array.Empty<byte>();
            for (int attempt = 0; attempt < 6; attempt++)
            {
                await NextFrame();
                await NextFrame();
                shot = new RenderTargetBitmap();
                await shot.RenderAsync(live);
                pixels = (await shot.GetPixelsAsync()).ToArray();
                if (Whole(pixels, shot.PixelWidth, shot.PixelHeight)) break;
            }
            host.Children.Remove(live);
            using var stream = new Windows.Storage.Streams.InMemoryRandomAccessStream();
            var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
            encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, (uint)shot.PixelWidth, (uint)shot.PixelHeight, 96, 96, pixels);
            await encoder.FlushAsync();
            var bytes = new byte[stream.Size];
            stream.Seek(0);
            await stream.ReadAsync(bytes.AsBuffer(), (uint)bytes.Length, Windows.Storage.Streams.InputStreamOptions.None);
            await File.WriteAllBytesAsync(path, bytes);
        }
    }

    private static Task NextFrame()
    {
        var done = new TaskCompletionSource();
        EventHandler<object>? rendered = null;
        rendered = (_, _) =>
        {
            CompositionTarget.Rendering -= rendered;
            done.TrySetResult();
        };
        CompositionTarget.Rendering += rendered;
        return done.Task;
    }

    private static bool Whole(byte[] pixels, int w, int h)
    {
        if (w < 4 || h < 4 || pixels.Length < w * h * 4) return false;
        for (int gy = 1; gy < 8; gy++)
            for (int gx = 1; gx < 8; gx++)
                if (pixels[((h * gy / 8) * w + w * gx / 8) * 4 + 3] < 250) return false;
        return true;
    }
}
#endif
