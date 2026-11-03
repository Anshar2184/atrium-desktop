using System.Numerics;
using System.Text.Json.Nodes;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The workspace previews other than the bar: the shape's numbers, the notification cards as they
// pop up, and the desktop's widget layout (read from the core's widgets.json, laid out by its rules).
public sealed partial class MainWindow
{
    // ---- Shape: the geometry, read out ----

    private readonly List<(TextBlock Value, TextBlock Note)> _readout = new();

    private void UpdateReadout()
    {
        if (_readout.Count == 0)
            foreach (var name in new[] { "高度", "圆角", "宽度", "轮廓" })
            {
                var value = new TextBlock { Style = StyleOf("TitleStyle") };
                Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(value, Microsoft.UI.Xaml.FontNumeralAlignment.Tabular);
                var note = Text("", "FootnoteStyle");
                var cell = new StackPanel { Spacing = 2 };
                cell.Children.Add(Text(name, "RowNoteStyle"));
                cell.Children.Add(value);
                cell.Children.Add(note);
                ShapeReadout.Children.Add(cell);
                _readout.Add((value, note));
            }
        var s = _current;
        double r = Math.Min(s.CornerRadius, s.Height / 2);
        bool capsule = s.CornerRadius >= s.Height / 2;
        _readout[0].Value.Text = $"{s.Height:0} px";
        _readout[0].Note.Text = $"图标 {s.IconSize:0} px";
        _readout[1].Value.Text = $"{r:0} px";
        _readout[1].Note.Text = s.CornerRadius > r ? $"设为 {s.CornerRadius:0}，不超过高度的一半" : "圆弧角";
        _readout[2].Value.Text = s.AutoWidth ? "自动" : $"{s.Width:0} px";
        _readout[2].Note.Text = s.AutoWidth ? $"最宽 {s.Width:0} px" : "固定宽度";
        _readout[3].Value.Text = capsule ? "胶囊形" : r < 1 ? "直角" : "圆角矩形";
        _readout[3].Note.Text = $"距底边 {s.Margin:0} px";
        HeroCaption.Text = _page switch
        {
            0 => "实时预览：与桌面上的玻璃栏使用同一组参数",
            1 => $"{s.Height:0} × {(s.AutoWidth ? "自动" : s.Width.ToString("0"))} · 圆角 {r:0}",
            _ => $"图标 {s.IconSize:0} px · 放大 {s.Magnify * 100:0}% · 距底边 {s.Margin:0} px",
        };
    }

    // ---- Notifications: two cards over the photo, as the core lays them out ----

    private void ShowNotifyPreview(bool animate)
    {
        if (NotifyStage.Background is not ImageBrush) NotifyStage.Background = PhotoBrush();
        var s = _current;
        // A card's corner follows the bar's (D26: between 8 and half the bar's height).
        double radius = Math.Clamp(s.CornerRadius, 8, Math.Max(8, s.Height / 2));
        (string App, uint C1, uint C2, string Glyph, string Title, string Body, string When)[] cards =
        {
            ("日历", 0xFF6B6B, 0xE0315A, "\uE787", "产品评审", "10:30 · 三号会议室，带上新版设计稿。", "5 分钟前"),
            ("微信", 0x5BE37D, 0x1AAD19, "\uE8BD", "林晨", "明天的活动时间定了吗？我这边上午都有空。", "现在"),
        };
        NotifyCards.Children.Clear();
        for (int i = 0; i < cards.Length; i++)
        {
            var c = cards[i];
            var icon = new Border
            {
                Width = 36, Height = 36, CornerRadius = new CornerRadius(9), VerticalAlignment = VerticalAlignment.Top,
                Background = Gradient(c.C1, c.C2),
                Child = new FontIcon { Glyph = c.Glyph, FontSize = 17, Foreground = new SolidColorBrush(Colors.White) },
            };
            var head = new Grid();
            head.Children.Add(new TextBlock { Text = c.App, FontSize = 12, Foreground = new SolidColorBrush(Color.FromArgb(150, 0x1D, 0x1D, 0x1F)) });
            head.Children.Add(new TextBlock { Text = c.When, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Right, Foreground = new SolidColorBrush(Color.FromArgb(130, 0x1D, 0x1D, 0x1F)) });
            var text = new StackPanel { Spacing = 2 };
            text.Children.Add(head);
            text.Children.Add(new TextBlock { Text = c.Title, FontSize = 14, FontFamily = (FontFamily)Application.Current.Resources["EmphasisFont"], Foreground = new SolidColorBrush(Color.FromArgb(240, 0x1D, 0x1D, 0x1F)) });
            if (!s.NotifyHideBody)
                text.Children.Add(new TextBlock { Text = c.Body, FontSize = 13, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis,
                                                  Foreground = new SolidColorBrush(Color.FromArgb(190, 0x1D, 0x1D, 0x1F)) });
            var row = new Grid { ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Grid.SetColumn(text, 1);
            row.Children.Add(icon);
            row.Children.Add(text);
            var card = new Border
            {
                Child = row, Padding = new Thickness(14, 12, 16, 13), CornerRadius = new CornerRadius(radius),
                Background = new SolidColorBrush(Color.FromArgb(205, 250, 250, 252)), BorderBrush = new SolidColorBrush(Color.FromArgb(150, 255, 255, 255)),
                BorderThickness = new Thickness(1), Shadow = new ThemeShadow(), Translation = new Vector3(0, 0, 16),
            };
            NotifyCards.Children.Add(card);
            if (animate) Rise(card, i * 90);
        }
        NotifyCaption.Text = $"新通知停留 {s.NotifyTimeout:0} 秒，指针停在卡片上时不会消失" + (s.NotifyHideBody ? "；正文已隐藏" : "");
    }

    private void OnReplayNotify(object sender, RoutedEventArgs e) => ShowNotifyPreview(true);

    // Up from a little below, fading in (the cards' own motion is a spring the core plays; an eased rise stands in).
    private void Rise(UIElement e, int delayMs)
    {
        e.Opacity = 0;
        e.Translation = new Vector3(0, 18, 16);
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(30 + delayMs);
        timer.IsRepeating = false;
        timer.Tick += (_, _) =>
        {
            e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) };
            e.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(360) };
            e.Opacity = 1;
            e.Translation = new Vector3(0, 0, 16);
        };
        timer.Start();
    }

    private static LinearGradientBrush Gradient(uint c1, uint c2)
    {
        var g = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 1) };
        g.GradientStops.Add(new GradientStop { Color = RgbColor(c1), Offset = 0 });
        g.GradientStops.Add(new GradientStop { Color = RgbColor(c2), Offset = 1 });
        return g;
    }

    // ---- Widgets: the desktop, scaled down, with the widgets where the core puts them ----

    // The core's grid (native/core/ui/desktop_widgets.cpp): cells 164, gaps 16, margins 24, a row's room for the bar.
    private const double WCell = 164, WGap = 16, WMargin = 24;
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern IntPtr MonitorFromPoint(System.Drawing.Point pt, uint flags);
    [System.Runtime.InteropServices.DllImport("shcore.dll")] private static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint dpiX, out uint dpiY);
    private FileSystemWatcher? _widgetWatcher;

    private static string WidgetsPath => System.IO.Path.Combine(System.IO.Path.GetDirectoryName(CoreClient.SettingsPath)!, "widgets.json");

    private void OnWidgetStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (Math.Abs(e.NewSize.Width - e.PreviousSize.Width) > 0.5) ShowWidgetMap();
    }

    private void ShowWidgetMap()
    {
        if (_widgetWatcher == null && Directory.Exists(System.IO.Path.GetDirectoryName(WidgetsPath)))
        {
            // The core rewrites the file when a widget is moved, resized, added or removed.
            _widgetWatcher = new FileSystemWatcher(System.IO.Path.GetDirectoryName(WidgetsPath)!, "widgets.json")
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size, EnableRaisingEvents = true,
            };
            FileSystemEventHandler changed = (_, _) => DispatcherQueue.TryEnqueue(() => { if (_page == 4) ShowWidgetMap(); });
            _widgetWatcher.Changed += changed;
            _widgetWatcher.Created += changed;
            _widgetWatcher.Renamed += (_, _) => DispatcherQueue.TryEnqueue(() => { if (_page == 4) ShowWidgetMap(); });
        }

        // The work area of the bar's (primary) monitor, in DIP at that monitor's own scale.
        double scale = GetDpiForMonitor(MonitorFromPoint(default, 1 /* MONITOR_DEFAULTTOPRIMARY */), 0, out uint dpi, out _) == 0 ? dpi / 96.0 : 1;
        var wa = DisplayArea.Primary.WorkArea;
        double W = wa.Width / scale, H = wa.Height / scale;
        int cols = Math.Max(1, (int)((W - 2 * WMargin + WGap) / (WCell + WGap)));
        int rows = Math.Max(1, (int)((H - 2 * WMargin - 90 + WGap) / (WCell + WGap)));
        double stageW = WidgetStage.ActualWidth > 0 ? WidgetStage.ActualWidth : 760;
        WidgetStage.Height = Math.Clamp(Math.Round(stageW * H / W), 220, 560);
        double k = stageW / W, kh = WidgetStage.Height / H;
        if (WidgetStage.Background is not ImageBrush) WidgetStage.Background = PhotoBrush();

        WidgetMap.Children.Clear();
        WidgetMap.Clip = new RectangleGeometry { Rect = new Rect(0, 0, stageW, WidgetStage.Height) };
        int shown = 0;
        foreach (var (kind, size, col, row) in ReadWidgets())
        {
            int wc = size == "small" ? 1 : 2, wr = size == "large" ? 2 : 1;
            if (col < 0 || row < 0 || col + wc > cols || row + wr > rows) continue; // the core keeps it aside on this screen
            double w = wc * WCell + (wc - 1) * WGap, h = wr * WCell + (wr - 1) * WGap;
            bool fromRight = 2 * col + wc >= cols; // each half of the grid hugs its own edge (D44)
            double left = fromRight ? W - WMargin - (cols - col - wc) * (WCell + WGap) - w : WMargin + col * (WCell + WGap);
            double top = WMargin + row * (WCell + WGap);
            var choice = WidgetChoices.FirstOrDefault(c => c.Kind == kind && c.Size == size) ?? new WidgetChoice(kind, size, "", "");
            var face = new Viewbox { Width = w * k, Height = h * kh, Stretch = Stretch.Uniform, Child = Face(choice, PhotoBrush()) };
            Canvas.SetLeft(face, left * k);
            Canvas.SetTop(face, top * kh);
            WidgetMap.Children.Add(face);
            shown++;
        }
        // The bar at the bottom, for scale.
        double barW = 560 * k, barH = Math.Max(4, _current.Height * kh);
        var bar = new Rectangle
        {
            Width = barW, Height = barH, RadiusX = barH / 2, RadiusY = barH / 2, Fill = new SolidColorBrush(Color.FromArgb(150, 255, 255, 255)),
            Stroke = new SolidColorBrush(Color.FromArgb(170, 255, 255, 255)), StrokeThickness = 1,
        };
        Canvas.SetLeft(bar, (stageW - barW) / 2);
        Canvas.SetTop(bar, WidgetStage.Height - barH - Math.Max(2, _current.Margin * kh));
        WidgetMap.Children.Add(bar);

        WidgetMapCaption.Text = !_current.Widgets ? "桌面小组件已关闭；打开后按这里的位置显示。"
            : shown == 0 ? "桌面上还没有小组件，从下面点按一个即可添加。"
            : $"桌面上的 {shown} 个小组件 · 网格 {cols} × {rows} · 在桌面上拖动、拖下角或右键来调整";
        WidgetMap.Opacity = _current.Widgets ? 1 : 0.5;
    }

    private static IEnumerable<(string Kind, string Size, int Col, int Row)> ReadWidgets()
    {
        JsonNode? root = null;
        try { root = JsonNode.Parse(File.ReadAllText(WidgetsPath)); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        catch (System.Text.Json.JsonException) { }
        if (root?["widgets"] is not JsonArray list) yield break;
        foreach (var e in list)
        {
            if (e is not JsonObject o) continue;
            string kind = (string?)o["kind"] ?? "", size = (string?)o["size"] ?? "small";
            int col = (int?)(double?)o["col"] ?? 0, row = (int?)(double?)o["row"] ?? 0;
            if (kind.Length > 0) yield return (kind, size, col, row);
        }
    }
}
