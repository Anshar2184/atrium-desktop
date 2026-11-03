using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace Atrium.Settings;

// The widget gallery (D43): every widget the core can show, as a small
// picture of it on a piece of glass over the specimen photo; a click puts one on the desktop.
public sealed partial class MainWindow
{
    private sealed record WidgetChoice(string Kind, string Size, string Name, string Note);

    private static readonly WidgetChoice[] WidgetChoices =
    {
        new("clock", "small", "时钟", "本地时间的表盘"),
        new("clock", "medium", "世界时钟", "四座城市的表盘"),
        new("digital", "small", "数字时钟", "时间、星期与日期"),
        new("digital", "medium", "数字时钟", "再加三座城市"),
        new("calendar", "small", "日历", "今天与农历"),
        new("calendar", "medium", "日历", "再加一个月历"),
        new("calendar", "large", "农历月历", "每天下面写农历或节日"),
        new("battery", "small", "电池", "电量圆环"),
        new("system", "small", "系统监视", "CPU、内存与磁盘"),
        new("system", "medium", "系统监视", "三个圆环"),
        new("notes", "small", "备忘录", "点按即可输入"),
        new("reminders", "medium", "提醒事项", "勾选完成的事"),
        new("photos", "small", "照片", "「图片」文件夹里的一张"),
        new("photos", "medium", "照片", "每 10 分钟换一张"),
    };

    private void BuildWidgetGallery()
    {
        foreach (var choice in WidgetChoices)
            WidgetGallery.Children.Add(GalleryCard(choice));
    }

    // The gallery only adds while the widgets are on.
    private void UpdateGallery()
    {
        WidgetGallery.Opacity = WidgetsSwitch.IsOn ? 1 : 0.45;
        WidgetGallery.IsHitTestVisible = WidgetsSwitch.IsOn;
        if (!WidgetsSwitch.IsOn) WidgetHint.Text = "先打开“桌面小组件”。";
        else if (WidgetHint.Text == "先打开“桌面小组件”。") WidgetHint.Text = "点按添加到桌面";
    }

    // A gallery card: the widget on a piece of the bar's glass over the picture, rendered like the hero.
    private readonly List<(WidgetChoice Choice, Border Stage)> _galleryStages = new();
    private BarSettings? _galleryMaterial;
    private int _galleryGen = -1;

    private FrameworkElement GalleryCard(WidgetChoice c)
    {
        var stage = new Border { Height = 118, CornerRadius = new CornerRadius(14), Background = PhotoBrush() };
        _galleryStages.Add((c, stage));
        double rendered = 0;
        stage.SizeChanged += (_, e) =>
        {
            if (Math.Abs(e.NewSize.Width - rendered) < 16) return;
            rendered = e.NewSize.Width;
            RenderGalleryCard(c, stage);
        };
        var name = new TextBlock
        {
            Text = c.Name + " · " + (c.Size switch { "small" => "小", "medium" => "中", _ => "大" }),
            Style = (Style)Application.Current.Resources["RowTitleStyle"], Margin = new Thickness(4, 0, 4, 2),
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var body = new StackPanel { Spacing = 10, Children = { stage, name } };
        var button = new Button
        {
            Content = body, Padding = new Thickness(10), CornerRadius = new CornerRadius(24), HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch, Background = Tokens.Brush("CardBrush"),
            BorderBrush = Tokens.Brush("CardStrokeBrush"), BorderThickness = new Thickness(1),
        };
        // Pointer over: the card rises a little.
        button.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(180) };
        button.PointerEntered += (_, _) => button.Translation = new Vector3(0, -2, 0);
        button.PointerExited += (_, _) => button.Translation = Vector3.Zero;
        AutomationProperties.SetName(button, "添加 " + c.Name + (c.Size == "small" ? "（小）" : c.Size == "medium" ? "（中）" : "（大）"));
        ToolTipService.SetToolTip(button, c.Note);
        button.Click += async (_, _) =>
        {
            var error = await _client.AddWidgetAsync(c.Kind, c.Size);
            WidgetHint.Text = error == null ? $"已把“{c.Name}”放到桌面上。" : error.Contains("room") ? "桌面上没有空位了：先移除或缩小一个小组件。" : "没有添加：" + error;
        };
        return button;
    }

    // The gallery again when the picture or the material changed since it was drawn.
    private void RefreshGallery()
    {
        if (_galleryMaterial != null && _galleryMaterial.SameMaterial(_current) && _galleryGen == Photo.Generation) return;
        foreach (var (c, stage) in _galleryStages) RenderGalleryCard(c, stage);
    }

    private void RenderGalleryCard(WidgetChoice c, Border stage)
    {
        double sw = stage.ActualWidth, sh = stage.ActualHeight;
        if (sw < 8 || sh < 8) return;
        _galleryMaterial = _current.Clone();
        _galleryGen = Photo.Generation;
        // The widget fitted into the card, 14 DIP of air around.
        var (w, h) = WidgetFaces.Size(c.Size);
        double k = Math.Min((sw - 28) / w, (sh - 28) / h);
        var tile = new GlassTile((sw - w * k) / 2, (sh - h * k) / 2, w * k, h * k, 22 * k);
        PostTiles(stage, sw, sh, k, new[] { tile }, null, (bmp, lumas) =>
        {
            stage.Background = new ImageBrush { ImageSource = bmp, Stretch = Stretch.Fill };
            var face = new Viewbox { Width = tile.W, Height = tile.H, Stretch = Stretch.Fill, Child = WidgetFaces.Face(c.Kind, c.Size, lumas[0] > 0.5f, PhotoBrush()) };
            var holder = new Canvas { IsHitTestVisible = false };
            Canvas.SetLeft(face, tile.X);
            Canvas.SetTop(face, tile.Y);
            holder.Children.Add(face);
            stage.Child = holder;
        });
    }

    // ---- The widgets' glass on the CPU: the picture, the tiles of glass, their mean brightness ----

    private sealed record GlassTile(double X, double Y, double W, double H, double Radius); // stage DIP

    // Renders on the thread pool and hands the bitmap and each tile's brightness (for its ink) to `done`
    // on the UI thread; a newer request for the same target drops an older result.
    private readonly Dictionary<object, int> _tileRequests = new();

    // A rendered stage: its pixels (BGRA), and each tile's brightness.
    private sealed record TileFrame(byte[] Px, int PW, int PH, double Rs, float[] Lumas)
    {
        public WriteableBitmap Bitmap()
        {
            var bmp = new WriteableBitmap(PW, PH);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(Px, 0, Px.Length);
            bmp.Invalidate();
            return bmp;
        }

        // The pixels under one tile, for a card that moves with its own piece of glass.
        public WriteableBitmap Crop(GlassTile t)
        {
            int x0 = Math.Clamp((int)Math.Round(t.X * Rs), 0, PW - 1), y0 = Math.Clamp((int)Math.Round(t.Y * Rs), 0, PH - 1);
            int w = Math.Clamp((int)Math.Round(t.W * Rs), 1, PW - x0), h = Math.Clamp((int)Math.Round(t.H * Rs), 1, PH - y0);
            var bmp = new WriteableBitmap(w, h);
            using (var stream = bmp.PixelBuffer.AsStream())
                for (int y = 0; y < h; y++) stream.Write(Px, ((y0 + y) * PW + x0) * 4, w * 4);
            bmp.Invalidate();
            return bmp;
        }
    }

    private void PostTiles(object target, double w, double h, double k, IReadOnlyList<GlassTile> tiles, GlassTile? bar,
                           Action<WriteableBitmap, float[]> done) =>
        PostTiles(target, w, h, k, tiles, bar, f => done(f.Bitmap(), f.Lumas));

    private void PostTiles(object target, double w, double h, double k, IReadOnlyList<GlassTile> tiles, GlassTile? bar,
                           Action<TileFrame> done)
    {
        double rs = Math.Clamp(Content.XamlRoot?.RasterizationScale ?? 1, 1, 2);
        int mine = _tileRequests[target] = (_tileRequests.TryGetValue(target, out int n) ? n : 0) + 1;
        var material = _current.Clone();
        var photo = Photo.Capture();
        Task.Run(() =>
        {
            int pw = (int)Math.Round(w * rs), ph = (int)Math.Round(h * rs);
            var bg = Backdrop.Create(pw, ph, w, h, 0, 0, rs, photo.Value);
            var px = new byte[bg.Base.Length];
            Buffer.BlockCopy(bg.Base, 0, px, 0, px.Length);
            var lumas = new float[tiles.Count];
            var all = bar == null ? tiles : tiles.Append(bar).ToList();
            for (int i = 0; i < all.Count; i++)
            {
                var t = all[i];
                var s = material.Clone();
                s.CornerRadius = t.Radius / k;
                var spec = GlassSpec.For(s, k * rs);
                int inset = (int)spec.Inset;
                int x = (int)Math.Round(t.X * rs), y = (int)Math.Round(t.Y * rs), tw = (int)Math.Round(t.W * rs), th = (int)Math.Round(t.H * rs);
                GlassRenderer.Draw(bg, px, x - inset, y - inset, tw + 2 * inset, th + 2 * inset, spec);
                if (i < tiles.Count)
                    lumas[i] = GlassRenderer.MeanLuma(px, pw, Math.Clamp(x, 0, pw - 1), Math.Clamp(y, 0, ph - 1),
                                                      Math.Clamp(x + tw, 1, pw), Math.Clamp(y + th, 1, ph));
            }
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_tileRequests.TryGetValue(target, out int latest) && latest != mine) return;
                done(new TileFrame(px, pw, ph, rs, lumas));
            });
        });
    }
}
