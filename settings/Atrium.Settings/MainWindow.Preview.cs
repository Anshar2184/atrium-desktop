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
        if (_page != 1) return;
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
        _readout[1].Note.Text = s.CornerRadius > r ? $"设为 {s.CornerRadius:0}，不超过高度的一半" : capsule ? "两端为半圆" : "连续曲率圆角（G3）";
        _readout[2].Value.Text = s.AutoWidth ? "自动" : $"{s.Width:0} px";
        _readout[2].Note.Text = s.AutoWidth ? $"最宽 {s.Width:0} px" : "固定宽度";
        _readout[3].Value.Text = capsule ? "胶囊形" : r < 1 ? "直角" : "圆角矩形";
        _readout[3].Note.Text = $"距底边 {s.Margin:0} px";
    }

    // ---- Notifications: the popup's stack over the picture, as the core paints it ----
    // (native/core/ui/notifications.cpp: cards 384 x 112, each its own piece of the bar's glass; the newest
    // in front with its text, the one behind peeking 10 DIP above, 12 DIP narrower on each side.)

    private const double CardW = 384, CardH = 112, CardPeek = 10, CardInset = 12;
    private bool _notifyDark;

    private void ShowNotifyPreview(bool animate)
    {
        if (NotifyStage.Background is not ImageBrush) NotifyStage.Background = PhotoBrush();
        var s = _current;
        double radius = Math.Min(Math.Clamp(s.CornerRadius, 8, Math.Max(8, s.Height / 2)), CardH / 2);
        double W = NotifyStage.ActualWidth, H = NotifyStage.ActualHeight;
        if (W < CardW + 48 || H < CardH + 60) { W = Math.Max(W, CardW + 48); H = Math.Max(H, CardH + 60); }
        var front = new GlassTile(W - 24 - CardW, H - 28 - CardH, CardW, CardH, radius);
        var back = new GlassTile(front.X + CardInset, front.Y - CardPeek, CardW - 2 * CardInset, CardH, radius);

        NotifyCards.Children.Clear();
        NotifyCards.Width = W;
        NotifyCards.Height = H;
        var backCard = new Border { Width = back.W, Height = back.H, CornerRadius = new CornerRadius(radius) };
        var frontCard = new Border { Width = front.W, Height = front.H, CornerRadius = new CornerRadius(radius), Child = CardText(s, _notifyDark) };
        Canvas.SetLeft(backCard, back.X); Canvas.SetTop(backCard, back.Y);
        Canvas.SetLeft(frontCard, front.X); Canvas.SetTop(frontCard, front.Y);
        NotifyCards.Children.Add(backCard);
        NotifyCards.Children.Add(frontCard);
        if (animate) { Rise(backCard, 0); Rise(frontCard, 70); }
        NotifyCaption.Text = $"{s.NotifyTimeout:0} 秒后收起" + (s.NotifyHideBody ? " · 正文已隐藏" : "");

        // Their glass: rendered over the picture where they rest; each card carries its own piece.
        PostTiles(NotifyCards, W, H, 1, new[] { back, front }, null, f =>
        {
            backCard.Background = new ImageBrush { ImageSource = f.Crop(back), Stretch = Stretch.Fill };
            frontCard.Background = new ImageBrush { ImageSource = f.Crop(front), Stretch = Stretch.Fill };
            bool dark = _notifyDark ? f.Lumas[1] >= 0.42f : f.Lumas[1] > 0.50f;
            if (dark != _notifyDark)
            {
                _notifyDark = dark;
                frontCard.Child = CardText(s, dark);
            }
        });
    }

    // PaintCard: the app (12 semibold, secondary ink) after the unread dot, when (12, faint) on the right,
    // the title (14 semibold), the body (13, secondary ink, two lines) or "content hidden".
    private static FrameworkElement CardText(BarSettings s, bool dark)
    {
        Color ink = dark ? Color.FromArgb(255, 0x11, 0x13, 0x17) : Colors.White;
        SolidColorBrush A(double a) => new(Color.FromArgb((byte)Math.Round(255 * a), ink.R, ink.G, ink.B));
        var text = A(dark ? 0.92 : 0.96);
        var sub = A(dark ? 0.66 : 0.76);
        var faint = A(dark ? 0.46 : 0.56);
        var c = new Canvas { IsHitTestVisible = false };
        TextBlock T(string str, double size, string font, Brush brush, double x, double y, double w, bool wrap = false)
        {
            var t = new TextBlock
            {
                Text = str, FontSize = size, Foreground = brush, Width = w, FontFamily = (FontFamily)Application.Current.Resources[font],
                TextWrapping = wrap ? TextWrapping.Wrap : TextWrapping.NoWrap, TextTrimming = TextTrimming.CharacterEllipsis,
                MaxLines = wrap ? 2 : 1, IsTextScaleFactorEnabled = false,
            };
            Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(t, Microsoft.UI.Xaml.FontNumeralAlignment.Tabular);
            Canvas.SetLeft(t, x); Canvas.SetTop(t, y);
            c.Children.Add(t);
            return t;
        }
        double tx = 20, ty = 16;
        var dot = new Ellipse { Width = 7, Height = 7, Fill = new SolidColorBrush(Color.FromArgb(255, 0x0A, 0x84, 0xFF)) };
        Canvas.SetLeft(dot, tx); Canvas.SetTop(dot, ty + 4.5);
        c.Children.Add(dot);
        T("微信", 12, "TitleFont", sub, tx + 12, ty - 1, CardW - 170 - tx - 12);
        var when = T("刚刚", 12, "EmphasisFont", faint, CardW - 170, ty - 1, 120);
        when.TextAlignment = TextAlignment.Right;
        T("林晨", 14, "TitleFont", text, tx, ty + 21, CardW - 20 - tx);
        if (s.NotifyHideBody) T("内容已隐藏", 13, "TextFont", faint, tx, ty + 45, CardW - 20 - tx);
        else T("明天的活动时间定了吗？我这边上午都有空，下午三点以后也可以。", 13, "TextFont", sub, tx, ty + 45, CardW - 20 - tx, wrap: true);
        return c;
    }

    private void OnNotifyStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_page == 3 && Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) > 0.5) ShowNotifyPreview(false);
    }

    private void OnReplayNotify(object sender, RoutedEventArgs e) => ShowNotifyPreview(true);

    // Up from a little below, fading in (the cards' own motion is a spring the core plays; an eased rise stands in).
    private void Rise(UIElement e, int delayMs)
    {
        e.Opacity = 0;
        e.Translation = new Vector3(0, 18, 0);
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(30 + delayMs);
        timer.IsRepeating = false;
        timer.Tick += (_, _) =>
        {
            e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) };
            e.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(360) };
            e.Opacity = 1;
            e.Translation = Vector3.Zero;
        };
        timer.Start();
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

    private readonly List<(string Kind, string Size, bool Dark, long Stamp, Viewbox View)> _mapFaces = new();

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

        // The tiles where the core puts them, then the bar at the bottom for scale; all glass.
        var tiles = new List<GlassTile>();
        var faces = new List<(string Kind, string Size)>();
        foreach (var (kind, size, col, row) in ReadWidgets())
        {
            int wc = size == "small" ? 1 : 2, wr = size == "large" ? 2 : 1;
            if (col < 0 || row < 0 || col + wc > cols || row + wr > rows) continue; // the core keeps it aside on this screen
            double w = wc * WCell + (wc - 1) * WGap, h = wr * WCell + (wr - 1) * WGap;
            int visualCol = _current.WidgetsOnRight ? cols - col - wc : col;
            bool fromRight = 2 * visualCol + wc >= cols;
            double left = fromRight ? W - WMargin - (cols - visualCol - wc) * (WCell + WGap) - w : WMargin + visualCol * (WCell + WGap);
            double top = WMargin + row * (WCell + WGap);
            tiles.Add(new GlassTile(left * k, top * kh, w * k, h * kh, 22 * Math.Min(k, kh)));
            faces.Add((kind, size));
        }
        double barW = 560 * k, barH = Math.Max(4, _current.Height * kh);
        var bar = new GlassTile((stageW - barW) / 2, WidgetStage.Height - barH - Math.Max(2, _current.Margin * kh), barW, barH, barH / 2);
        int shown = tiles.Count;
        double stageH = WidgetStage.Height;
        PostTiles(WidgetMap, stageW, stageH, Math.Min(k, kh), tiles, bar, frame =>
        {
            frame.Apply(WidgetStage);
            WidgetMap.Clip = new RectangleGeometry { Rect = new Rect(0, 0, stageW, stageH) };
            while (_mapFaces.Count > tiles.Count)
            {
                WidgetMap.Children.Remove(_mapFaces[^1].View);
                _mapFaces.RemoveAt(_mapFaces.Count - 1);
            }
            for (int i = 0; i < tiles.Count; i++)
            {
                var t = tiles[i];
                var (kind, size) = faces[i];
                bool dark = frame.Lumas[i] > 0.5f;
                long stamp = WidgetFaces.Stamp(kind);
                bool added = i == _mapFaces.Count;
                var face = added ? new Viewbox { Stretch = Stretch.Fill } : _mapFaces[i].View;
                if (added || _mapFaces[i].Kind != kind || _mapFaces[i].Size != size || _mapFaces[i].Dark != dark || _mapFaces[i].Stamp != stamp)
                    face.Child = WidgetFaces.Face(kind, size, dark, PhotoBrush());
                if (added) { WidgetMap.Children.Insert(i, face); _mapFaces.Add((kind, size, dark, stamp, face)); }
                else _mapFaces[i] = (kind, size, dark, stamp, face);
                face.Width = t.W;
                face.Height = t.H;
                Canvas.SetLeft(face, t.X);
                Canvas.SetTop(face, t.Y);
            }
            Ready("widgets");
        });

        WidgetMapCaption.Text = !_current.Widgets ? "桌面小组件已关闭"
            : shown == 0 ? "暂无小组件"
            : $"{shown} 个小组件";
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
