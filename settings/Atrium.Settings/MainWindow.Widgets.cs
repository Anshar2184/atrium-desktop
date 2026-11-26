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
    private sealed record WidgetChoice(string Kind, string Size, string Name, string Note, string Category);

    // The gallery's groups, in order; a segmented control above shows all of them or one.
    private static readonly string[] WidgetCategoryNames = { "时钟", "天气", "日历", "效率", "照片", "文件", "系统" };

    private static WidgetChoice[] WidgetChoices => new WidgetChoice[]
    {
        new("clock", "small", Loc.T("时钟"), Loc.T("本地时间的表盘"), "时钟"),
        new("clock", "medium", Loc.T("世界时钟"), Loc.T("四座城市的表盘"), "时钟"),
        new("digital", "small", Loc.T("数字时钟"), Loc.T("时间、星期与日期"), "时钟"),
        new("digital", "medium", Loc.T("数字时钟"), Loc.T("再加三座城市"), "时钟"),
        new("weather", "small", Loc.T("天气"), Loc.T("气温与天气"), "天气"),
        new("weather", "medium", Loc.T("天气"), Loc.T("再加逐小时预报"), "天气"),
        new("weather", "large", Loc.T("天气"), Loc.T("再加五天预报"), "天气"),
        new("calendar", "small", Loc.T("日历"), Loc.T("今天与农历"), "日历"),
        new("calendar", "medium", Loc.T("日历"), Loc.T("再加一个月历"), "日历"),
        new("calendar", "large", Loc.T("农历月历"), Loc.T("每天下面写农历或节日"), "日历"),
        new("notes", "small", Loc.T("备忘录"), Loc.T("点按即可输入"), "效率"),
        new("reminders", "medium", Loc.T("提醒事项"), Loc.T("勾选完成的事"), "效率"),
        new("photos", "small", Loc.T("照片"), Loc.T("「图片」文件夹里的一张"), "照片"),
        new("photos", "medium", Loc.T("照片"), Loc.T("每 10 分钟换一张"), "照片"),
        new("filebox", "small", Loc.T("文件盒子"), Loc.T("映射一个文件夹，拖文件进来"), "文件"),
        new("filebox", "medium", Loc.T("文件盒子"), Loc.T("8 个格子的收纳盒"), "文件"),
        new("filebox", "large", Loc.T("文件盒子"), Loc.T("16 个格子的大盒子"), "文件"),
        new("battery", "small", Loc.T("电池"), Loc.T("电量圆环"), "系统"),
        new("system", "small", Loc.T("系统监视"), Loc.T("CPU、内存与磁盘"), "系统"),
        new("system", "medium", Loc.T("系统监视"), Loc.T("三个圆环"), "系统"),
        new("bluetooth", "small", Loc.T("蓝牙设备"), Loc.T("四个设备的电量"), "系统"),
        new("bluetooth", "medium", Loc.T("蓝牙设备"), Loc.T("名称与电量"), "系统"),
        new("network", "small", Loc.T("网络"), Loc.T("上下行速率与一分钟走势"), "系统"),
        new("network", "medium", Loc.T("网络"), Loc.T("速率、走势与本次流量"), "系统"),
    };

    private readonly Dictionary<string, FrameworkElement> _widgetSections = new();

    // Each group under its name; the control above narrows the gallery to one group.
    private void BuildWidgetGallery()
    {
        foreach (string category in WidgetCategoryNames)
        {
            var grid = new ResponsiveGrid { MinCellWidth = 180, Gap = 12 };
            foreach (var choice in WidgetChoices.Where(c => c.Category == category))
                grid.Children.Add(GalleryCard(choice));
            var section = new StackPanel { Spacing = 8 };
            section.Children.Add(new TextBlock { Text = Loc.T(category), Style = (Style)Application.Current.Resources["RowTitleStyle"], Margin = new Thickness(4, 0, 0, 0) });
            section.Children.Add(grid);
            _widgetSections[category] = section;
            WidgetGallery.Children.Add(section);
        }
    }

    private void OnWidgetCategoryChecked(object sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton { Tag: string category }) return;
        foreach (var (name, section) in _widgetSections)
            section.Visibility = category == "全部" || category == name ? Visibility.Visible : Visibility.Collapsed;
    }

    // The gallery only adds while the widgets are on.
    private void UpdateGallery()
    {
        WidgetGallery.Opacity = WidgetsSwitch.IsOn ? 1 : 0.45;
        WidgetGallery.IsHitTestVisible = WidgetsSwitch.IsOn;
        // Only why the gallery does not answer: the widgets are off.
        WidgetHint.Text = Loc.T("先打开“桌面小组件”。");
        WidgetHint.Visibility = WidgetsSwitch.IsOn ? Visibility.Collapsed : Visibility.Visible;
    }

    // A gallery card: the widget on a piece of the bar's glass over the picture, rendered like the hero.
    private sealed record GalleryPreview(WidgetChoice Choice, Border Stage, Viewbox Face, Canvas Viewport)
    {
        public bool? DarkInk;
        public long Stamp;
        public string? Material;
    }

    private readonly List<GalleryPreview> _galleryStages = new();
    private BarSettings? _galleryMaterial;
    private int _galleryGen = -1;

    private FrameworkElement GalleryCard(WidgetChoice c)
    {
        var stage = new Border { Height = 118, CornerRadius = new CornerRadius(14), Background = PhotoBrush() };
        var face = new Viewbox { Stretch = Stretch.Fill };
        // The bitmap and the face share the last completed viewport. While a
        // resize render is pending, XAML stretches both by the same amount.
        var holder = new Canvas { Width = 1, Height = 1, IsHitTestVisible = false, Children = { face } };
        stage.Child = new Viewbox { Stretch = Stretch.Fill, Child = holder };
        var preview = new GalleryPreview(c, stage, face, holder);
        _galleryStages.Add(preview);
        stage.SizeChanged += (_, _) => RenderGalleryCard(preview);
        var name = new TextBlock
        {
            Text = c.Name + " · " + Loc.T(c.Size switch { "small" => "小", "medium" => "中", _ => "大" }),
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
        AutomationProperties.SetName(button, c.Size == "small" ? Loc.F("添加 {0}（小）", c.Name)
            : c.Size == "medium" ? Loc.F("添加 {0}（中）", c.Name) : Loc.F("添加 {0}（大）", c.Name));
        ToolTipService.SetToolTip(button, c.Note);
        button.Click += async (_, _) =>
        {
            var error = await _client.AddWidgetAsync(c.Kind, c.Size);
            WidgetHint.Text = error == null ? Loc.F("已把“{0}”放到桌面上。", c.Name) : error.Contains("room") ? Loc.T("桌面上没有空位了：先移除或缩小一个小组件。") : Loc.F("没有添加：{0}", error);
            WidgetHint.Visibility = Visibility.Visible;
        };
        return button;
    }

    // The gallery again when the picture or the material changed since it was drawn.
    private void RefreshGallery()
    {
        if (_galleryMaterial != null && _galleryMaterial.SameMaterial(_current) && _galleryMaterial.WidgetMaterial == _current.WidgetMaterial && _galleryGen == Photo.Generation) return;
        foreach (var preview in _galleryStages) RenderGalleryCard(preview);
    }

    private void RenderGalleryCard(GalleryPreview preview)
    {
        var (c, stage, face, viewport) = preview;
        double sw = stage.ActualWidth, sh = stage.ActualHeight;
        if (sw < 8 || sh < 8) return;
        _galleryMaterial = _current.Clone();
        _galleryGen = Photo.Generation;
        // The widget fitted into the card, 14 DIP of air around.
        var (w, h) = WidgetFaces.Size(c.Size);
        double k = Math.Min((sw - 28) / w, (sh - 28) / h);
        var tile = new GlassTile((sw - w * k) / 2, (sh - h * k) / 2, w * k, h * k, 22 * k);
        PostTiles(stage, sw, sh, k, new[] { tile }, null, frame =>
        {
            frame.Apply(stage);
            viewport.Width = sw;
            viewport.Height = sh;
            bool dark = frame.Lumas[0] > 0.5f;
            long stamp = WidgetFaces.Stamp(c.Kind);
            string material = _current.WidgetMaterial;
            if (preview.DarkInk != dark || preview.Stamp != stamp || preview.Material != material)
            {
                face.Child = WidgetFaces.Face(c.Kind, c.Size, dark, PhotoBrush(), material);
                ContinuousCorners.Install(face);
                preview.DarkInk = dark;
                preview.Stamp = stamp;
                preview.Material = material;
            }
            face.Width = tile.W;
            face.Height = tile.H;
            Canvas.SetLeft(face, tile.X);
            Canvas.SetTop(face, tile.Y);
        });
    }

    // ---- The widgets' glass on the CPU: the picture, the tiles of glass, their mean brightness ----

    // One drain for all tile previews, with at most two tiles rendering at once. Pending requests
    // replace only the same target; equal viewports share a backdrop. No full-size map or obsolete
    // photo is kept alive after the batch.
    private sealed record TileJob(object Target, int Id, double W, double H, double K, double Rs,
                                  IReadOnlyList<GlassTile> Tiles, GlassTile? Bar, BarSettings Material,
                                  Lazy<Photo> Photo, Action<TileFrame> Done, bool Overlays, bool Scaled);
    private readonly object _tileLock = new();
    private readonly Dictionary<object, TileJob> _pendingTiles = new();
    private bool _tilesBusy, _tilesStopped;
    private readonly Dictionary<object, int> _tileRequests = new();

    // A rendered stage: its pixels (BGRA), and each tile's brightness.
    private sealed record TileFrame(byte[] Px, int PW, int PH, double Rs, float[] Lumas, GlassOverlay[]? Overlays = null)
    {
        public WriteableBitmap Overlay(int index)
        {
            var image = Overlays![index];
            var bmp = new WriteableBitmap(image.Width, image.Height);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(image.Pixels);
            bmp.Invalidate();
            return bmp;
        }
        public void Apply(Border stage)
        {
            var brush = ContinuousCorners.Original(stage) as ImageBrush;
            var bmp = brush?.ImageSource as WriteableBitmap;
            if (bmp == null || bmp.PixelWidth != PW || bmp.PixelHeight != PH)
            {
                bmp = new WriteableBitmap(PW, PH);
                stage.Background = new ImageBrush { ImageSource = bmp, Stretch = Stretch.Fill };
            }
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(Px, 0, Px.Length);
            bmp.Invalidate();
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
                           Action<TileFrame> done, bool overlays = false, bool scaled = false)
    {
        double rs = Math.Clamp(Content.XamlRoot?.RasterizationScale ?? 1, 1, 2);
        int mine = _tileRequests[target] = (_tileRequests.TryGetValue(target, out int n) ? n : 0) + 1;
        lock (_tileLock)
        {
            if (_tilesStopped) return;
            _pendingTiles[target] = new TileJob(target, mine, w, h, k, rs, tiles, bar, _current.Clone(), Photo.Capture(), done, overlays, scaled);
            if (_tilesBusy) return;
            _tilesBusy = true;
        }
        Task.Run(RenderTiles);
    }

    private void StopTiles()
    {
        lock (_tileLock) { _tilesStopped = true; _pendingTiles.Clear(); }
    }

    private void RenderTiles()
    {
        Backdrop? bg = null;
        (double W, double H, double Rs, Lazy<Photo> Photo)? backdrop = null;
        while (true)
        {
            TileJob[] jobs;
            lock (_tileLock)
            {
                if (_pendingTiles.Count == 0) { _tilesBusy = false; return; }
                jobs = _pendingTiles.Values.ToArray();
                _pendingTiles.Clear();
            }
            foreach (var group in jobs.GroupBy(j => (j.W, j.H, j.Rs, j.Photo)))
            {
                var key = group.Key;
                int pw = (int)Math.Round(key.W * key.Rs), ph = (int)Math.Round(key.H * key.Rs);
                try
                {
                    if (bg == null || backdrop != key)
                    {
                        bg = Backdrop.Create(pw, ph, key.W, key.H, 0, 0, key.Rs, key.Photo.Value);
                        backdrop = key;
                    }
                    Parallel.ForEach(group, new ParallelOptions { MaxDegreeOfParallelism = 2 }, job =>
                    {
                        lock (_tileLock)
                            if (_tilesStopped || _pendingTiles.ContainsKey(job.Target)) return;
                        TileFrame frame;
                        if (job.Overlays)
                        {
                            // Scaled: the widget map, whose picture comes along (its cards move over it).
                            var cards = GlassRenderer.CardOverlays(bg, job.Material, job.Rs, job.Tiles, job.Scaled ? job.K : 0);
                            frame = new TileFrame(job.Scaled ? bg.Base : Array.Empty<byte>(), pw, ph, job.Rs, cards.Select(c => c.Luma).ToArray(), cards);
                        }
                        else
                        {
                            var px = new byte[bg.Base.Length];
                            var lumas = GlassRenderer.DrawTiles(bg, px, job.Material, job.K, job.Rs, job.Tiles, job.Bar);
                            frame = new TileFrame(px, pw, ph, job.Rs, lumas);
                        }
                        DispatcherQueue.TryEnqueue(() =>
                        {
                            if (!_tilesStopped && _tileRequests[job.Target] == job.Id) job.Done(frame);
                        });
                    });
                }
                catch (Exception e)
                {
                    System.Diagnostics.Debug.WriteLine("tile preview render failed: " + e);
                }
            }
        }
    }
}
