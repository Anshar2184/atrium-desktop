using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
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

    private BitmapImage? _galleryPhoto;

    // The specimen photo, decoded small (cards are about 200 DIP wide), shared by every card.
    private Brush PhotoBrush()
    {
        if (Photo.Path == null) return new SolidColorBrush(Color.FromArgb(255, 0xCB, 0xD3, 0xDD));
        _galleryPhoto ??= new BitmapImage(new Uri(Photo.Path)) { DecodePixelWidth = 640 };
        return new ImageBrush { ImageSource = _galleryPhoto, Stretch = Stretch.UniformToFill };
    }

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

    private FrameworkElement GalleryCard(WidgetChoice c)
    {
        // The preview: the widget's own proportions (small 1:1, medium 344:164), scaled into the card.
        var stage = new Border
        {
            Height = 118, CornerRadius = new CornerRadius(14), Padding = new Thickness(14),
            Background = PhotoBrush(),
            Child = new Viewbox { Stretch = Stretch.Uniform, Child = Face(c, PhotoBrush()) },
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

    // ---- The pictures: a white glass tile at the widget's proportions, its content in ink ----

    private static readonly Color Ink = Color.FromArgb(255, 0x1D, 0x1D, 0x1F);
    private static readonly Color Soft = Color.FromArgb(150, 0x1D, 0x1D, 0x1F);
    private static readonly Color Red = Color.FromArgb(255, 0xFF, 0x3B, 0x30);
    private static readonly Color Orange = Color.FromArgb(255, 0xFF, 0x9F, 0x0A);
    private static readonly Color Green = Color.FromArgb(255, 0x30, 0xD1, 0x58);
    private static readonly Color Blue = Color.FromArgb(255, 0x0A, 0x84, 0xFF);

    private static FrameworkElement Face(WidgetChoice c, Brush photo)
    {
        double w = c.Size == "medium" ? 344 : 164, h = c.Size == "small" ? 164 : c.Size == "medium" ? 164 : 344;
        if (c.Size == "large") w = 344;
        var canvas = new Canvas { Width = w, Height = h };
        canvas.Children.Add(new Rectangle
        {
            Width = w, Height = h, RadiusX = 30, RadiusY = 30, Fill = new SolidColorBrush(Color.FromArgb(200, 255, 255, 255)),
            Stroke = new SolidColorBrush(Color.FromArgb(160, 255, 255, 255)), StrokeThickness = 1.5,
        });
        switch (c.Kind, c.Size)
        {
            case ("clock", "small"): Dial(canvas, w / 2, h / 2, 62, true); break;
            case ("clock", _):
                for (int i = 0; i < 4; i++) Dial(canvas, 52 + i * 80, 70, 32, false);
                for (int i = 0; i < 4; i++) Label(canvas, new[] { "北京", "东京", "伦敦", "纽约" }[i], 52 + i * 80 - 30, 112, 60, 15, Ink, true);
                break;
            case ("digital", var s):
                Label(canvas, "星期一", 22, 22, 120, 16, Red, false);
                Label(canvas, "10:24", 18, 44, 140, 52, Ink, false, "TitleFont");
                Label(canvas, "11月2日", 22, 112, 120, 15, Soft, false);
                if (s == "medium")
                    for (int i = 0; i < 3; i++)
                    {
                        Label(canvas, new[] { "东京", "伦敦", "纽约" }[i], 196, 34 + i * 36, 70, 15, Soft, false);
                        Label(canvas, new[] { "11:24", "02:24", "21:24" }[i], 250, 30 + i * 36, 74, 20, Ink, false, "EmphasisFont", TextAlignment.Right);
                    }
                break;
            case ("calendar", "small"):
                Label(canvas, "星期一", 22, 20, 120, 16, Red, false);
                Label(canvas, "2", 20, 38, 120, 64, Ink, false, "TextFont");
                Label(canvas, "农历九月廿四", 22, 118, 130, 14, Soft, false);
                break;
            case ("calendar", "medium"):
                Label(canvas, "星期一", 22, 20, 120, 16, Red, false);
                Label(canvas, "2", 20, 38, 120, 64, Ink, false, "TextFont");
                MonthGrid(canvas, 176, 26, 22, 20, 5);
                break;
            case ("calendar", _):
                Label(canvas, "十一月", 24, 20, 200, 18, Red, false);
                MonthGrid(canvas, 26, 58, 42, 52, 5);
                break;
            case ("battery", _):
                Ring(canvas, w / 2, 72, 42, 10, 1.0, Green);
                Label(canvas, "100%", 0, 122, w, 22, Ink, false, "TitleFont", TextAlignment.Center);
                break;
            case ("system", "small"):
                Label(canvas, "系统", 22, 16, 100, 14, Soft, false);
                for (int i = 0; i < 3; i++)
                {
                    Label(canvas, new[] { "CPU", "内存", "磁盘" }[i], 22, 38 + i * 38, 60, 15, Ink, false, "EmphasisFont");
                    Bar(canvas, 22, 60 + i * 38, 120, new[] { 0.12, 0.66, 0.7 }[i], i == 2 ? Orange : Green);
                }
                break;
            case ("system", _):
                for (int i = 0; i < 3; i++)
                {
                    Ring(canvas, 66 + i * 106, 70, 34, 8, new[] { 0.12, 0.66, 0.7 }[i], i == 2 ? Orange : Green);
                    Label(canvas, new[] { "CPU", "内存", "磁盘" }[i], 26 + i * 106, 118, 80, 15, Ink, true);
                }
                break;
            case ("notes", _):
                Label(canvas, "备忘录", 22, 18, 120, 15, Orange, false, "EmphasisFont");
                for (int i = 0; i < 3; i++) Bar(canvas, 22, 56 + i * 26, i == 2 ? 70 : 118, 0, Soft, 6);
                break;
            case ("reminders", _):
                Label(canvas, "提醒事项", 22, 16, 140, 15, Blue, false, "EmphasisFont");
                Label(canvas, "2", 290, 10, 32, 30, Blue, false, "TitleFont", TextAlignment.Right);
                for (int i = 0; i < 3; i++)
                {
                    var dot = new Ellipse { Width = 16, Height = 16, Stroke = new SolidColorBrush(i == 2 ? Blue : Soft), StrokeThickness = 1.5,
                                            Fill = i == 2 ? new SolidColorBrush(Blue) : null };
                    Canvas.SetLeft(dot, 22); Canvas.SetTop(dot, 52 + i * 34);
                    canvas.Children.Add(dot);
                    Bar(canvas, 50, 58 + i * 34, i == 1 ? 150 : 200, 0, Soft, 5);
                }
                break;
            case ("photos", _):
                canvas.Children.Clear();
                canvas.Children.Add(new Rectangle { Width = w, Height = h, RadiusX = 30, RadiusY = 30, Fill = photo });
                break;
        }
        return canvas;
    }

    private static void Label(Canvas c, string text, double x, double y, double w, double size, Color ink, bool center,
                              string font = "EmphasisFont", TextAlignment align = TextAlignment.Left)
    {
        var t = new TextBlock
        {
            Text = text, Width = w, FontSize = size, Foreground = new SolidColorBrush(ink),
            FontFamily = (FontFamily)Application.Current.Resources[font], TextAlignment = center ? TextAlignment.Center : align,
        };
        Canvas.SetLeft(t, x);
        Canvas.SetTop(t, y);
        c.Children.Add(t);
    }

    private static void Dial(Canvas c, double cx, double cy, double r, bool numbers)
    {
        var face = new Ellipse { Width = 2 * r, Height = 2 * r, Fill = new SolidColorBrush(Colors.White) };
        Canvas.SetLeft(face, cx - r); Canvas.SetTop(face, cy - r);
        c.Children.Add(face);
        for (int i = 0; i < 12; i++)
        {
            double a = i * Math.PI / 6, r0 = r * (numbers ? 0.86 : 0.8), r1 = r * 0.94;
            c.Children.Add(new Line
            {
                X1 = cx + r0 * Math.Sin(a), Y1 = cy - r0 * Math.Cos(a), X2 = cx + r1 * Math.Sin(a), Y2 = cy - r1 * Math.Cos(a),
                Stroke = new SolidColorBrush(Soft), StrokeThickness = i % 3 == 0 ? 2.5 : 1.2,
            });
        }
        void Hand(double turns, double len, double width, Color col)
        {
            double a = turns * 2 * Math.PI;
            c.Children.Add(new Line
            {
                X1 = cx, Y1 = cy, X2 = cx + len * Math.Sin(a), Y2 = cy - len * Math.Cos(a), Stroke = new SolidColorBrush(col),
                StrokeThickness = width, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
            });
        }
        Hand(10.4 / 12, r * 0.5, r * 0.09, Ink);
        Hand(24.0 / 60, r * 0.74, r * 0.07, Ink);
        Hand(41.0 / 60, r * 0.84, r * 0.025, Orange);
    }

    private static void Ring(Canvas c, double cx, double cy, double r, double width, double level, Color col)
    {
        var track = new Ellipse { Width = 2 * r, Height = 2 * r, Stroke = new SolidColorBrush(Color.FromArgb(40, 0, 0, 0)), StrokeThickness = width };
        Canvas.SetLeft(track, cx - r); Canvas.SetTop(track, cy - r);
        c.Children.Add(track);
        double a = Math.Min(level, 0.999) * 2 * Math.PI, rr = r - width / 2;
        var fig = new PathFigure { StartPoint = new Windows.Foundation.Point(cx, cy - rr) };
        fig.Segments.Add(new ArcSegment
        {
            Point = new Windows.Foundation.Point(cx + rr * Math.Sin(a), cy - rr * Math.Cos(a)), Size = new Windows.Foundation.Size(rr, rr),
            SweepDirection = SweepDirection.Clockwise, IsLargeArc = a > Math.PI,
        });
        c.Children.Add(new Microsoft.UI.Xaml.Shapes.Path
        {
            Data = new PathGeometry { Figures = { fig } }, Stroke = new SolidColorBrush(col), StrokeThickness = width,
            StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
        });
    }

    private static void Bar(Canvas c, double x, double y, double w, double level, Color col, double h = 6)
    {
        var track = new Rectangle { Width = w, Height = h, RadiusX = h / 2, RadiusY = h / 2, Fill = new SolidColorBrush(Color.FromArgb(level > 0 ? (byte)36 : (byte)70, col.R, col.G, col.B)) };
        Canvas.SetLeft(track, x); Canvas.SetTop(track, y);
        c.Children.Add(track);
        if (level <= 0) return;
        var fill = new Rectangle { Width = Math.Max(h, w * level), Height = h, RadiusX = h / 2, RadiusY = h / 2, Fill = new SolidColorBrush(col) };
        Canvas.SetLeft(fill, x); Canvas.SetTop(fill, y);
        c.Children.Add(fill);
    }

    // A month: today (the 2nd) on a red disc.
    private static void MonthGrid(Canvas c, double x, double y, double cell, double row, int rows)
    {
        for (int r = 0; r < rows; r++)
            for (int d = 0; d < 7; d++)
            {
                int day = r * 7 + d - 5;
                if (day < 1 || day > 30) continue;
                if (day == 2)
                {
                    var disc = new Ellipse { Width = row, Height = row, Fill = new SolidColorBrush(Red) };
                    Canvas.SetLeft(disc, x + d * cell + (cell - row) / 2); Canvas.SetTop(disc, y + r * row);
                    c.Children.Add(disc);
                }
                // MiSans' line box is 1.326 em with the figures centred in it: centre the box on the disc.
                double size = row * 0.6;
                Label(c, day.ToString(), x + d * cell, y + r * row + (row - 1.326 * size) / 2, cell, size, day == 2 ? Colors.White : Ink, true);
            }
    }
}
