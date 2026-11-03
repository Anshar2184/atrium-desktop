using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;

namespace Atrium.Settings;

// One picture behind every preview: the bar's, the notifications', the widget map's and gallery's,
// the design page's. Picking another crossfades each visible preview: a snapshot of how it looked
// stays on top until the new picture is in (decoded, or the glass rendered over it), then fades out.
public sealed partial class MainWindow
{
    private readonly ImageBrush _sharedPhoto = new() { Stretch = Stretch.UniformToFill };
    private BitmapImage? _sharedSource;
    private string? _sharedPath = "\0";
    private readonly Dictionary<string, (FrameworkElement Target, Image Snapshot)> _frozen = new();
    private readonly Dictionary<string, int> _waits = new(); // what each snapshot still waits for
    private bool _compareBuilt; // the design page's material comparison renders over the picture too
    private volatile int _heroPhotoGen = -1; // the photo generation of the newest hero background

    // Every preview that renders over the picture on the CPU re-renders when it changes.
    private event Action? PhotoChanged;

    private Brush PhotoBrush()
    {
        EnsureSharedPhoto();
        return _sharedPhoto;
    }

    private void EnsureSharedPhoto()
    {
        var path = Photo.SelectedPath;
        if (path == _sharedPath) return;
        _sharedPath = path;
        if (path == null)
        {
            _sharedSource = null;
            _sharedPhoto.ImageSource = null;
            Ready("notify"); Ready("design");
            return;
        }
        var source = new BitmapImage { DecodePixelWidth = 1600 };
        source.ImageOpened += (_, _) => { if (source == _sharedSource) { Ready("notify"); Ready("design"); } };
        source.ImageFailed += (_, _) => { if (source == _sharedSource) { Ready("notify"); Ready("design"); } };
        source.UriSource = new Uri(path);
        _sharedSource = source;
        _sharedPhoto.ImageSource = source;
    }

    // The preview's photograph: a sample, or the user's own wallpaper (Photo.cs), everywhere at once.
    private async void OnHeroBackgroundChecked(object sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton { Tag: string tag } || tag == Photo.Selected) return;
        await FreezeAsync("hero", HeroFrame, 1);
        await FreezeAsync("notify", NotifyStage, 1);
        await FreezeAsync("widgets", WidgetStage, 1);
        await FreezeAsync("design", DesignStack, _compareBuilt ? 2 : 1);
        if (!Photo.Select(tag)) return;
        EnsureSharedPhoto();
        PhotoChanged?.Invoke();
        UpdateHero();
        // Never keep a snapshot up for long, whatever happens to the decode.
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromSeconds(8);
        timer.IsRepeating = false;
        timer.Tick += (_, _) => ThawAllBut(null);
        timer.Start();
    }

    // A snapshot of `target` over its own content (target is a Border with a Panel inside, or a Panel).
    private async Task FreezeAsync(string key, FrameworkElement target, int waits)
    {
        if (_frozen.ContainsKey(key) || target.Visibility != Visibility.Visible || target.ActualWidth < 2 || target.ActualHeight < 2) return;
        var host = target as Panel ?? (target as Border)?.Child as Panel;
        if (host == null) return;
        var rtb = new RenderTargetBitmap();
        try { await rtb.RenderAsync(target); }
        catch (Exception) { return; }
        if (_frozen.ContainsKey(key)) return;
        var image = new Image
        {
            Source = rtb, Stretch = Stretch.Fill, IsHitTestVisible = false, Width = target.ActualWidth, Height = target.ActualHeight,
            HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
        };
        if (target is Border b) image.Margin = new Thickness(-b.Padding.Left - b.BorderThickness.Left, -b.Padding.Top - b.BorderThickness.Top, 0, 0);
        Grid.SetRowSpan(image, 99);
        Grid.SetColumnSpan(image, 99);
        host.Children.Add(image);
        _frozen[key] = (target, image);
        _waits[key] = waits;
    }

    // One of the things a snapshot waits for is in; the last one lets it fade.
    private void Ready(string key)
    {
        if (!_waits.TryGetValue(key, out int n)) return;
        if (n > 1) _waits[key] = n - 1;
        else Thaw(key);
    }

    private void ThawAllBut(string? keep)
    {
        foreach (var key in _frozen.Keys.ToList())
            if (key != keep) Thaw(key);
    }

    // The snapshot fades out over the new look (ease out, 420 ms), then goes.
    private void Thaw(string key)
    {
        _waits.Remove(key);
        if (!_frozen.Remove(key, out var f)) return;
        var image = f.Snapshot;
        var fade = new DoubleAnimation { To = 0, Duration = new Duration(TimeSpan.FromMilliseconds(420)), EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } };
        Storyboard.SetTarget(fade, image);
        Storyboard.SetTargetProperty(fade, "Opacity");
        var story = new Storyboard { Children = { fade } };
        story.Completed += (_, _) => (VisualTreeHelper.GetParent(image) as Panel)?.Children.Remove(image);
        story.Begin();
    }
}
