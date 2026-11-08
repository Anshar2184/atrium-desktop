using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;

namespace Atrium.Settings;

// One picture behind every preview: the bar's, the notifications', the widget map's and gallery's, Stage Manager's,
// the design page's. Picking another crossfades each visible preview: a snapshot of how it looked
// stays on top until the new picture is in (decoded, or the glass rendered over it), then fades out.
public sealed partial class MainWindow
{
    private readonly ImageBrush _sharedPhoto = new() { Stretch = Stretch.UniformToFill };
    private BitmapImage? _sharedSource;
    private string? _sharedPath = "\0";
    private readonly Dictionary<string, (FrameworkElement Target, Image Snapshot)> _frozen = new();
    private readonly Dictionary<string, (int Remaining, int Generation)> _waits = new();
    private bool _compareBuilt; // the design page's material comparison renders over the picture too
    private volatile int _heroPhotoGen = -1; // the photo generation of the newest hero background
    private int _photoSelection;
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _photoThawTimer;

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
        int generation = Photo.Generation;
        if (path == _sharedPath) return;
        _sharedPath = path;
        if (path == null)
        {
            _sharedSource = null;
            _sharedPhoto.ImageSource = null;
            Ready("notify", generation); Ready("design", generation);
            return;
        }
        var source = new BitmapImage { DecodePixelWidth = 1600 };
        source.ImageOpened += (_, _) => { if (source == _sharedSource) { Ready("notify", generation); Ready("design", generation); } };
        source.ImageFailed += (_, _) => { if (source == _sharedSource) { Ready("notify", generation); Ready("design", generation); } };
        source.UriSource = new Uri(path);
        _sharedSource = source;
        _sharedPhoto.ImageSource = source;
    }

    // The preview's photograph: a sample, or the user's own wallpaper (Photo.cs), everywhere at once.
    private async void OnHeroBackgroundChecked(object sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton { Tag: string tag }) return;
        int selection = ++_photoSelection;
        int expectedGeneration = Photo.Generation + 1;
        _photoThawTimer?.Stop();
        if (tag == Photo.Selected) { ThawAllBut(null); return; }
        await FreezeAsync("hero", HeroFrame, 1, selection, expectedGeneration);
        if (selection != _photoSelection) return;
        await FreezeAsync("notify", NotifyStage, 1, selection, expectedGeneration);
        if (selection != _photoSelection) return;
        await FreezeAsync("widgets", WidgetStage, 1, selection, expectedGeneration);
        if (selection != _photoSelection) return;
        await FreezeAsync("stage", StagePreview, 1, selection, expectedGeneration);
        if (selection != _photoSelection) return;
        await FreezeAsync("design", DesignStack, _compareBuilt ? 2 : 1, selection, expectedGeneration);
        if (selection != _photoSelection || _updatesClosed) return;
        if (!Photo.Select(tag)) { ThawAllBut(null); return; }
        EnsureSharedPhoto();
        PhotoChanged?.Invoke();
        UpdateHero();
        // Never keep a snapshot up for long, whatever happens to the decode.
        var timer = _photoThawTimer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromSeconds(8);
        timer.IsRepeating = false;
        timer.Tick += (_, _) => { if (selection == _photoSelection) ThawAllBut(null); };
        timer.Start();
    }

    // A snapshot of `target` over its own content (target is a Border with a Panel inside, or a Panel).
    private async Task FreezeAsync(string key, FrameworkElement target, int waits, int selection, int generation)
    {
        if (_frozen.ContainsKey(key)) { _waits[key] = (waits, generation); return; }
        if (target.Visibility != Visibility.Visible || target.ActualWidth < 2 || target.ActualHeight < 2) return;
        var host = target as Panel ?? (target as Border)?.Child as Panel;
        if (host == null) return;
        var rtb = new RenderTargetBitmap();
        try { await rtb.RenderAsync(target); }
        catch (Exception) { return; }
        if (selection != _photoSelection || _updatesClosed || _frozen.ContainsKey(key)) return;
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
        _waits[key] = (waits, generation);
    }

    // One of the things a snapshot waits for is in; the last one lets it fade.
    private void Ready(string key, int generation)
    {
        if (!_waits.TryGetValue(key, out var wait) || wait.Generation != generation) return;
        if (wait.Remaining > 1) _waits[key] = (wait.Remaining - 1, generation);
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
