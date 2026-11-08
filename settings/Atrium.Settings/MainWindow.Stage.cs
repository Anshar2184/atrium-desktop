using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.System;
using Windows.UI;

namespace Atrium.Settings;

// Stage Manager's page: the desktop over the shared picture, the app in front in the middle and the
// others waiting at the side on cards of the bar's glass (rendered as the core renders it). A click
// on a card swaps it with the stage, Shift and a click adds it to the stage, as on the desktop
// (ui/stage_manager.cpp); the cards glide on the same spring (0.42 s, damping 0.86).
public sealed partial class MainWindow
{
    private sealed record StageApp(string Name, Color Accent, bool Dark);

    private static readonly StageApp[] StageApps =
    {
        new("备忘录", Color.FromArgb(255, 0xFF, 0xB3, 0x40), false),
        new("邮件", Color.FromArgb(255, 0x0A, 0x84, 0xFF), false),
        new("日历", Color.FromArgb(255, 0xFF, 0x45, 0x3A), false),
        new("照片", Color.FromArgb(255, 0x30, 0xD1, 0x58), false),
        new("终端", Color.FromArgb(255, 0x2C, 0x2C, 0x2E), true),
    };

    // Preview DIP. The cards are the desktop's (168 x 138, 18 corners) at 0.62.
    private const double StageCardW = 104, StageCardH = 86, StageCardGap = 9, StageEdge = 12, StageCardR = 11;
    private const double StageBarH = 34, StageBarRoom = 56, StageWinGap = 26;

    private List<int> _stageFront = new() { 0 };
    private readonly List<List<int>> _stageStrip = new() { new() { 1, 2 }, new() { 3 }, new() { 4 } };
    private readonly Dictionary<string, Grid> _stageCards = new();
    private readonly HashSet<string> _stageAppearing = new(); // shown once their glass is in
    private readonly Dictionary<string, Image> _stageGlass = new();
    private Grid? _stageCenter, _stageBar;
    private Image? _stageBarGlass;
    private bool _stageBuilt;

    private static string StageKey(List<int> set) => string.Join(",", set);

    private void OnStagePreviewSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_page == 5 && Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) > 0.5)
            ShowStagePreview(false);
    }

    private void OnStageReplay(object sender, RoutedEventArgs e)
    {
        if (_stageStrip.Count > 0) StagePick(_stageStrip[0], false);
    }

    // Lays the desktop out again from the model; `animate`: the cards come in once their glass is there.
    private void ShowStagePreview(bool animate)
    {
        if (ContinuousCorners.Original(StagePreview) is not ImageBrush) StagePreview.Background = PhotoBrush();
        double W = Math.Max(StagePreview.ActualWidth, 420), H = Math.Max(StagePreview.ActualHeight, 300);
        StageCanvas.Width = W;
        StageCanvas.Height = H;
        bool on = _current.StageManager;
        string side = _current.StageOnRight ? "右侧" : "左侧";
        StageCaption.Text = on ? $"点按{side}的卡片换到前面；按住 Shift 点按，加入当前这一组" : "台前调度已关闭：窗口照常叠放在桌面上";
        StageReplay.IsEnabled = on;

        StageCanvas.Children.Clear();
        _stageCards.Clear();
        _stageGlass.Clear();
        _stageAppearing.Clear();
        _stageBuilt = true;

        // The bar along the bottom, a few apps on it.
        var bar = StageBarTile(W, H);
        _stageBarGlass = new Image { Stretch = Stretch.Fill };
        _stageBar = new Grid { Width = bar.W + 2 * GlassSpec.InsetDip, Height = bar.H + 2 * GlassSpec.InsetDip, IsHitTestVisible = false };
        _stageBar.Children.Add(_stageBarGlass);
        var icons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        foreach (var app in StageApps)
            icons.Children.Add(new Border { Width = 20, Height = 20, CornerRadius = new CornerRadius(5), Background = new SolidColorBrush(app.Accent) });
        _stageBar.Children.Add(icons);
        Canvas.SetLeft(_stageBar, bar.X - GlassSpec.InsetDip);
        Canvas.SetTop(_stageBar, bar.Y - GlassSpec.InsetDip);
        StageCanvas.Children.Add(_stageBar);

        if (on)
        {
            _stageCenter = StageCenter(_stageFront, W, H);
            StageCanvas.Children.Add(_stageCenter);
            for (int i = 0; i < _stageStrip.Count; i++)
            {
                var card = StageCard(_stageStrip[i], StageCardTile(i, _stageStrip.Count, W, H));
                StageCanvas.Children.Add(card);
                if (animate)
                {
                    ElementCompositionPreview.GetElementVisual(card).Opacity = 0;
                    _stageAppearing.Add(StageKey(_stageStrip[i]));
                }
            }
            if (animate) StageAppear(_stageCenter, 0.96f);
        }
        else
        {
            // Off: every window where it was, one over the other.
            _stageCenter = StageCascade(W, H);
            StageCanvas.Children.Add(_stageCenter);
            if (animate) StageAppear(_stageCenter, 0.98f);
        }
        StageRenderGlass(W, H);
    }

    private GlassTile StageBarTile(double W, double H)
    {
        double w = Math.Min(W * 0.52, 480);
        return new GlassTile((W - w) / 2, H - 10 - StageBarH, w, StageBarH, StageBarH / 2);
    }

    private GlassTile StageCardTile(int i, int n, double W, double H)
    {
        double used = n * StageCardH + (n - 1) * StageCardGap;
        double top = Math.Max(StageEdge, (H - StageBarRoom - used) / 2);
        double x = _current.StageOnRight ? W - StageEdge - StageCardW : StageEdge;
        return new GlassTile(x, top + i * (StageCardH + StageCardGap), StageCardW, StageCardH, StageCardR);
    }

    // The room of the app in front: beside the strip, above the bar.
    private Rect StageRoom(double W, double H)
    {
        double left = _current.StageOnRight ? StageWinGap : StageEdge + StageCardW + StageWinGap;
        double right = _current.StageOnRight ? W - StageEdge - StageCardW - StageWinGap : W - StageWinGap;
        return new Rect(left, 22, Math.Max(40, right - left), Math.Max(40, H - StageBarRoom - 22 - 12));
    }

    // ---- The glass, rendered where the cards and the bar rest ----

    private void StageRenderGlass(double W, double H)
    {
        var tiles = new List<GlassTile>();
        var keys = new List<string>();
        if (_current.StageManager)
            for (int i = 0; i < _stageStrip.Count; i++)
            {
                tiles.Add(StageCardTile(i, _stageStrip.Count, W, H));
                keys.Add(StageKey(_stageStrip[i]));
            }
        tiles.Add(StageBarTile(W, H));
        int photoGeneration = Photo.Generation;
        PostTiles(StageCanvas, W, H, 1, tiles, null, f =>
        {
            for (int i = 0; i < keys.Count; i++)
            {
                if (_stageGlass.TryGetValue(keys[i], out var image)) image.Source = f.Overlay(i);
                if (_stageCards.TryGetValue(keys[i], out var card)) StageInk(card, f.Lumas[i] > 0.5f);
                if (_stageAppearing.Remove(keys[i]) && _stageCards.TryGetValue(keys[i], out var shown)) StageAppear(shown, 0.94f);
            }
            if (_stageBarGlass != null) _stageBarGlass.Source = f.Overlay(keys.Count);
            Ready("stage", photoGeneration);
        }, overlays: true);
    }

    // ---- Cards ----

    private Grid StageCard(List<int> set, GlassTile tile)
    {
        string key = StageKey(set);
        double pad = GlassSpec.InsetDip;
        var glass = new Image { Stretch = Stretch.Fill };
        _stageGlass[key] = glass;
        var content = new Grid { Margin = new Thickness(pad), Width = tile.W, Height = tile.H };
        var wash = new Border { CornerRadius = new CornerRadius(tile.Radius), Background = new SolidColorBrush(Colors.White), Opacity = 0 };
        content.Children.Add(wash);
        // The windows, the most recent in front, each one behind a step up and to the right.
        double imgW = tile.W - 10, imgH = 58, off = 3 * (set.Count - 1);
        var pictures = new Canvas { Width = imgW, Height = imgH, Margin = new Thickness(5, 5, 5, 0), VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Left };
        double fw = Math.Min(imgW - off, (imgH - off) * 1.6), fh = fw / 1.6;
        for (int j = Math.Min(set.Count, 3) - 1; j >= 0; j--)
        {
            var w = StageWindow(StageApps[set[j]], fw, fh, 4);
            w.Opacity = j == 0 ? 1 : 0.72;
            Canvas.SetLeft(w, (imgW - off - fw) / 2 + 3 * j);
            Canvas.SetTop(w, off + (imgH - off - fh) / 2 - 3 * j);
            pictures.Children.Add(w);
        }
        content.Children.Add(pictures);
        var title = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, Margin = new Thickness(6, 0, 6, 5), VerticalAlignment = VerticalAlignment.Bottom };
        foreach (int a in set.Take(3))
            title.Children.Add(new Border { Width = 11, Height = 11, CornerRadius = new CornerRadius(3), Background = new SolidColorBrush(StageApps[a].Accent), VerticalAlignment = VerticalAlignment.Center });
        title.Children.Add(new TextBlock
        {
            Text = string.Join("、", set.Select(a => StageApps[a].Name)), FontSize = 9.5, Margin = new Thickness(2, 0, 0, 0),
            FontFamily = (FontFamily)Application.Current.Resources["EmphasisFont"], TextTrimming = TextTrimming.CharacterEllipsis,
            MaxWidth = tile.W - 16 - 15 * Math.Min(set.Count, 3), IsTextScaleFactorEnabled = false, VerticalAlignment = VerticalAlignment.Center,
        });
        content.Children.Add(title);
        var card = new Grid { Width = tile.W + 2 * pad, Height = tile.H + 2 * pad, Tag = key, Background = new SolidColorBrush(Colors.Transparent) };
        card.Children.Add(glass);
        card.Children.Add(content);
        Canvas.SetLeft(card, tile.X - pad);
        Canvas.SetTop(card, tile.Y - pad);
        ElementCompositionPreview.SetIsTranslationEnabled(card, true);
        card.PointerEntered += (_, _) => wash.Opacity = 0.14;
        card.PointerExited += (_, _) => wash.Opacity = 0;
        card.PointerPressed += (_, e) =>
        {
            bool shift = (e.KeyModifiers & VirtualKeyModifiers.Shift) != 0;
            var picked = _stageStrip.FirstOrDefault(s => StageKey(s) == key);
            if (picked != null) StagePick(picked, shift);
            e.Handled = true;
        };
        _stageCards[key] = card;
        StageInk(card, false);
        return card;
    }

    private static void StageInk(Grid card, bool dark)
    {
        var ink = new SolidColorBrush(dark ? Color.FromArgb(235, 0x11, 0x13, 0x17) : Color.FromArgb(245, 255, 255, 255));
        foreach (var t in Descendants(card).OfType<TextBlock>()) t.Foreground = ink;
    }

    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        if (root is Panel p)
            foreach (var c in p.Children)
            {
                yield return c;
                foreach (var d in Descendants(c)) yield return d;
            }
        else if (root is Border { Child: { } child })
        {
            yield return child;
            foreach (var d in Descendants(child)) yield return d;
        }
    }

    // ---- Windows ----

    // An app's window, drawn at 320 x 200 and scaled: title bar, a sidebar, a coloured block, lines.
    private static FrameworkElement StageWindow(StageApp app, double w, double h, double radius)
    {
        var bg = app.Dark ? Color.FromArgb(255, 0x1C, 0x1C, 0x1E) : Colors.White;
        var line = app.Dark ? Color.FromArgb(150, 0x9F, 0xE8, 0x9A) : Color.FromArgb(255, 0xE5, 0xE7, 0xEB);
        var faint = app.Dark ? Color.FromArgb(90, 255, 255, 255) : Color.FromArgb(255, 0x8E, 0x8E, 0x93);
        var body = new Grid { Width = 320, Height = 200 };
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(26) });
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var titleBar = new Grid { Padding = new Thickness(10, 0, 10, 0) };
        var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        name.Children.Add(new Border { Width = 10, Height = 10, CornerRadius = new CornerRadius(3), Background = new SolidColorBrush(app.Accent) });
        name.Children.Add(new TextBlock { Text = app.Name, FontSize = 10, Foreground = new SolidColorBrush(faint), IsTextScaleFactorEnabled = false });
        titleBar.Children.Add(name);
        titleBar.Children.Add(new TextBlock
        {
            Text = "\uE921   \uE922   \uE8BB", FontFamily = new FontFamily("Segoe Fluent Icons"), FontSize = 8, Foreground = new SolidColorBrush(faint),
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center, IsTextScaleFactorEnabled = false,
        });
        body.Children.Add(titleBar);
        var main = new Grid { Padding = new Thickness(10, 4, 12, 12), ColumnSpacing = 12 };
        Grid.SetRow(main, 1);
        main.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(app.Dark ? 0 : 64) });
        main.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        if (!app.Dark)
        {
            var sidebar = new StackPanel { Spacing = 8, Padding = new Thickness(8, 10, 8, 10), CornerRadius = new CornerRadius(6),
                                           Background = new SolidColorBrush(Color.FromArgb(28, app.Accent.R, app.Accent.G, app.Accent.B)) };
            foreach (double sw in new[] { 40.0, 32, 44, 28 })
                sidebar.Children.Add(new Border { Width = sw, Height = 6, CornerRadius = new CornerRadius(3), HorizontalAlignment = HorizontalAlignment.Left,
                                                  Background = new SolidColorBrush(line) });
            main.Children.Add(sidebar);
        }
        var content = new StackPanel { Spacing = 9 };
        Grid.SetColumn(content, 1);
        if (!app.Dark)
            content.Children.Add(new Border { Height = 64, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Color.FromArgb(225, app.Accent.R, app.Accent.G, app.Accent.B)) });
        foreach (double lw in app.Dark ? new[] { 0.7, 0.45, 0.8, 0.55, 0.65, 0.3 } : new[] { 0.9, 0.75, 0.82, 0.5 })
            content.Children.Add(new Border { Height = 6, CornerRadius = new CornerRadius(3), HorizontalAlignment = HorizontalAlignment.Left,
                                              Width = (app.Dark ? 296 : 210) * lw, Background = new SolidColorBrush(line) });
        main.Children.Add(content);
        body.Children.Add(main);
        var frame = new Border
        {
            Width = 320, Height = 200, CornerRadius = new CornerRadius(radius * 320 / w), Background = new SolidColorBrush(bg),
            BorderBrush = new SolidColorBrush(Color.FromArgb(40, 0, 0, 0)), BorderThickness = new Thickness(1), Child = body,
        };
        return new Viewbox { Width = w, Height = h, Stretch = Stretch.Fill, Child = frame, IsHitTestVisible = false };
    }

    // The set in front: its windows in the room beside the strip, the most recent in front.
    private Grid StageCenter(List<int> set, double W, double H)
    {
        var room = StageRoom(W, H);
        double step = 18, n = Math.Min(set.Count, 3);
        double fw = Math.Min(room.Width - step * (n - 1), (room.Height - step * (n - 1)) * 1.6), fh = fw / 1.6;
        var grid = new Grid { Width = room.Width, Height = room.Height, IsHitTestVisible = false };
        var canvas = new Canvas();
        double x0 = (room.Width - fw - step * (n - 1)) / 2, y0 = (room.Height - fh - step * (n - 1)) / 2;
        // Back to front: the first app of the set is the most recent.
        for (int j = (int)n - 1; j >= 0; j--)
        {
            var w = StageWindow(StageApps[set[j]], fw, fh, 8);
            Canvas.SetLeft(w, x0 + step * ((int)n - 1 - j));
            Canvas.SetTop(w, y0 + step * ((int)n - 1 - j));
            canvas.Children.Add(w);
        }
        grid.Children.Add(canvas);
        Canvas.SetLeft(grid, room.X);
        Canvas.SetTop(grid, room.Y);
        return grid;
    }

    // Stage Manager off: every app's window in a loose cascade.
    private Grid StageCascade(double W, double H)
    {
        var room = new Rect(40, 22, Math.Max(40, W - 80), Math.Max(40, H - StageBarRoom - 34));
        int n = StageApps.Length;
        double step = 26, fw = Math.Min(room.Width - step * (n - 1), (room.Height - 12 * (n - 1)) * 1.6), fh = fw / 1.6;
        var grid = new Grid { Width = room.Width, Height = room.Height, IsHitTestVisible = false };
        var canvas = new Canvas();
        double x0 = (room.Width - fw - step * (n - 1)) / 2;
        for (int j = n - 1; j >= 0; j--)
        {
            var w = StageWindow(StageApps[j], fw, fh, 8);
            Canvas.SetLeft(w, x0 + step * (n - 1 - j));
            Canvas.SetTop(w, 12 * (n - 1 - j));
            canvas.Children.Add(w);
        }
        grid.Children.Add(canvas);
        Canvas.SetLeft(grid, room.X);
        Canvas.SetTop(grid, room.Y);
        return grid;
    }

    // ---- The swap ----

    private void StagePick(List<int> picked, bool add)
    {
        if (!_current.StageManager || !_stageBuilt) return;
        double W = StageCanvas.Width, H = StageCanvas.Height;
        var before = _stageStrip.Select(StageKey).ToList();
        var oldFront = _stageFront;
        int at = _stageStrip.IndexOf(picked);
        if (at < 0) return;
        _stageStrip.RemoveAt(at);
        if (add)
            _stageFront = oldFront.Concat(picked).ToList();
        else
        {
            _stageStrip.Insert(0, oldFront);
            _stageFront = picked;
        }

        // The stage: the windows that leave go towards the strip, the new ones come forward.
        if (_stageCenter is { } old)
        {
            var room = StageRoom(W, H);
            StageLeave(old, add ? 1f : 0.9f, new Vector3((float)(_current.StageOnRight ? room.Width : 0), (float)(room.Height / 2), 0), add ? 0.0 : 1.0);
        }
        _stageCenter = StageCenter(_stageFront, W, H);
        StageCanvas.Children.Insert(Math.Min(1, StageCanvas.Children.Count), _stageCenter);
        StageAppear(_stageCenter, 0.96f);

        // The strip: the picked card goes; the others glide to their places; the stage's old set comes in at the top.
        string pickedKey = StageKey(picked);
        if (_stageCards.Remove(pickedKey, out var gone))
        {
            _stageGlass.Remove(pickedKey);
            StageLeave(gone, 0.94f, new Vector3((float)(gone.Width / 2), (float)(gone.Height / 2), 0), 1.0);
        }
        for (int i = 0; i < _stageStrip.Count; i++)
        {
            string key = StageKey(_stageStrip[i]);
            var tile = StageCardTile(i, _stageStrip.Count, W, H);
            if (_stageCards.TryGetValue(key, out var card))
            {
                double fromY = Canvas.GetTop(card), toY = tile.Y - GlassSpec.InsetDip;
                Canvas.SetTop(card, toY);
                StageGlide(card, (float)(fromY - toY));
            }
            else
            {
                card = StageCard(_stageStrip[i], tile);
                ElementCompositionPreview.GetElementVisual(card).Opacity = 0;
                _stageAppearing.Add(key);
                StageCanvas.Children.Add(card);
            }
        }
        StageRenderGlass(W, H);
    }

    // From where it was to where it now stands, on the strip's spring.
    private static void StageGlide(UIElement element, float fromY)
    {
        if (Math.Abs(fromY) < 0.5f) return;
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var compositor = visual.Compositor;
        visual.Properties.InsertVector3("Translation", new Vector3(0, fromY, 0));
        var spring = compositor.CreateSpringVector3Animation();
        spring.Target = "Translation";
        spring.FinalValue = Vector3.Zero;
        spring.DampingRatio = 0.86f;
        spring.Period = TimeSpan.FromSeconds(0.42 / (2 * Math.PI));
        visual.StartAnimation("Translation", spring);
    }

    // Fades in and grows from `scale` (a critically damped spring for the scale).
    private static void StageAppear(FrameworkElement element, float scale)
    {
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)(element.Width / 2), (float)(element.Height / 2), 0);
        visual.Scale = new Vector3(scale, scale, 1);
        visual.Opacity = 0;
        var grow = compositor.CreateSpringVector3Animation();
        grow.FinalValue = Vector3.One;
        grow.DampingRatio = 1f;
        grow.Period = TimeSpan.FromSeconds(0.30 / (2 * Math.PI));
        visual.StartAnimation("Scale", grow);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(240);
        fade.InsertKeyFrame(1, 1, compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Opacity", fade);
    }

    // Fades out and shrinks towards `center`, then leaves the canvas.
    private void StageLeave(FrameworkElement element, float scale, Vector3 center, double fadeShare)
    {
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var compositor = visual.Compositor;
        visual.CenterPoint = center;
        var batch = compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
        var shrink = compositor.CreateVector3KeyFrameAnimation();
        shrink.Duration = TimeSpan.FromMilliseconds(220);
        shrink.InsertKeyFrame(1, new Vector3(scale, scale, 1), compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Scale", shrink);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(fadeShare > 0 ? 200 : 260);
        fade.InsertKeyFrame(1, 0);
        visual.StartAnimation("Opacity", fade);
        batch.End();
        batch.Completed += (_, _) => StageCanvas.Children.Remove(element);
    }
}
