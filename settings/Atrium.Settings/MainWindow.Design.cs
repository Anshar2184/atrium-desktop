using System.Diagnostics;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The design page: docs/DESIGN.md in pictures. The curves are computed with the core's own
// formulas (native/core/motion.h), so what the page draws and plays is the motion that runs.
public sealed partial class MainWindow
{
    private static class Motion
    {
        public static double EaseOut(double t)
        {
            t = Math.Clamp(t, 0, 1);
            double u = 1 - t;
            return 1 - u * u * u;
        }

        public static double EaseOutBack(double t, double s)
        {
            t = Math.Clamp(t, 0, 1) - 1;
            return 1 + (s + 1) * t * t * t + s * t * t;
        }

        // A spring from 0 at rest towards 1, at time t (closed form, as motion::Spring::Step).
        public static double Spring(double t, double response, double damping)
        {
            double w0 = 2 * Math.PI / response, z = damping, y;
            if (Math.Abs(z - 1) < 1e-3)
                y = -Math.Exp(-w0 * t) * (1 + w0 * t);
            else if (z < 1)
            {
                double a = z * w0, wd = w0 * Math.Sqrt(1 - z * z);
                y = -Math.Exp(-a * t) * (Math.Cos(wd * t) + a / wd * Math.Sin(wd * t));
            }
            else
            {
                double q = w0 * Math.Sqrt(z * z - 1), r1 = -z * w0 + q, r2 = -z * w0 - q, c2 = r1 / (r2 - r1);
                y = (-1 - c2) * Math.Exp(r1 * t) + c2 * Math.Exp(r2 * t);
            }
            return 1 + y;
        }

        public static double Approach(double t, double rate) => 1 - Math.Exp(-rate * t);
    }

    private sealed record Curve(string Name, string Spec, string Use, double Seconds, Func<double, double> At);

    private static readonly Curve[] Curves =
    {
        new("kPop · 弹簧", "时长 0.36 s · 回弹 0.38", "弹出面板出现：缩放 0.92 → 1，峰值 1.0067", 0.6, t => Motion.Spring(t, 0.36, 0.62)),
        new("kSmooth · 弹簧", "时长 0.48 s · 回弹 0.23", "状态圆环：电量、音量与亮度", 0.8, t => Motion.Spring(t, 0.48, 0.77)),
        new("堆叠 · 临界阻尼弹簧", "时长 0.60 s · 回弹 0", "滚轮翻看通知后吸附到一张", 1.0, t => Motion.Spring(t, 0.60, 1.0)),
        new("EaseOut · 三次缓出", "1 − (1 − t)³ · 320 ms", "换页、淡入、列表依次出现、字色渐变", 0.32, t => Motion.EaseOut(t / 0.32)),
        new("EaseOutBack · 回弹缓出", "s = 1.5 · 260 ms", "开始菜单的格子出现", 0.26, t => Motion.EaseOutBack(t / 0.26, 1.5)),
        new("Approach · 指数渐近", "进 20/s · 出 11/s", "悬停与按下的底板", 0.3, t => Motion.Approach(t, 20)),
    };

    // The type ramp (DESIGN.md §1.2): name, size, MiSans file, its weight, macOS counterpart, where.
    private static readonly (string Name, double Size, string Font, string Weight, string Mac, string Use)[] TypeRamp =
    {
        ("Large Title", 30, "TitleFont", "Semibold 520", "Large Title · 26", "页标题、产品名"),
        ("Title", 20, "TitleFont", "Semibold 520", "Title 1 · 22", "空状态标题、日历的月份"),
        ("Row", 15, "TextFont", "Medium 380", "Body · 13", "设置中心的行标题"),
        ("Value", 15, "EmphasisFont", "Demibold 450", "—", "滑块的数值（等宽数字）"),
        ("Body", 13.5, "TextFont", "Medium 380", "Body · 13", "面板正文、通知内容"),
        ("Note", 13, "TextFont", "Medium 380", "Callout · 12", "行下方的说明"),
        ("Section", 13, "EmphasisFont", "Demibold 450", "—", "分组标题"),
        ("Footnote", 12, "TextFont", "Medium 380", "Footnote · 10", "补充说明；玻璃上升为 Demibold"),
    };

    // Logical width from, typical screens, bar height, icon, bottom margin, magnification.
    private static readonly (double From, string Screens, int Height, int Icon, int Margin, double Magnify)[] SizeClasses =
    {
        (0, "1366×768 @100%、1920×1080 @150%", 52, 26, 8, 1.5),
        (1280, "1920×1080 @125%、2560×1600 @175%、3200×2000 @200%", 60, 30, 10, 1.7),
        (1680, "1920×1080 @100%、2560×1440 @125%", 64, 32, 12, 1.7),
        (2400, "2560×1440 @100%、3840×2160 @150%", 68, 34, 14, 1.6),
    };

    private bool _designBuilt;
    private readonly List<(double Start, Curve Curve, Action<double> Show)> _playing = new();
    private bool _designTicking;
    private static readonly Stopwatch DesignClock = Stopwatch.StartNew();

    private static Style StyleOf(string key) => (Style)Application.Current.Resources[key];
    private static Brush BrushOf(string key) => (Brush)Application.Current.Resources[key];

    private static TextBlock Text(string text, string style, double? size = null)
    {
        var t = new TextBlock { Text = text, Style = StyleOf(style) };
        if (size is double s) t.FontSize = s;
        return t;
    }

    private static Border Card(UIElement child) => new() { Style = StyleOf("GroupStyle"), Padding = new Thickness(20, 16, 20, 16), Child = child };

    private static StackPanel Section(string label, string? note, UIElement body)
    {
        var s = new StackPanel { Spacing = 12 };
        s.Children.Add(Text(label, "SectionStyle"));
        if (note != null)
        {
            var n = Text(note, "RowNoteStyle");
            n.Margin = new Thickness(18, -4, 0, 0);
            s.Children.Add(n);
        }
        s.Children.Add(body);
        return s;
    }

    private void BuildDesignPage()
    {
        if (_designBuilt) return;
        _designBuilt = true;
        DesignStack.Children.Add(Principles());
        DesignStack.Children.Add(Section("动效", "弹簧参数按 Apple 的方式给出：时长与回弹（= 1 − 阻尼比）。所有动画都可以中途打断，从当前位置和速度继续；与帧率无关。点卡片播放。",
            CurveGrid()));
        DesignStack.Children.Add(Section("排版", "MiSans 三个字重（Medium 380、Demibold 450、Semibold 520），字号与 macOS 的文本样式一一对应；数字一律等宽。一个界面最多三档。",
            TypeCard()));
        DesignStack.Children.Add(Section("圆角", "同心：内层圆角 = 外层圆角 − 边距（至少 6）。优先使用胶囊形，圆角为高度的一半。", RadiusCard()));
        DesignStack.Children.Add(Section("尺寸", "先把分辨率换算成逻辑宽度（像素 ÷ 缩放），再查栏的尺寸。图标是栏高的一半，栏高约为逻辑屏幕高度的 6%。", SizeCard()));
        DesignStack.Children.Add(Section("字色", "由玻璃背后的亮度决定：高于 0.50 用深色字，低于 0.42 换回浅色字，两者之间不切换；切换时 450 ms 缓出渐变。", InkCard()));
        var link = new HyperlinkButton
        {
            Content = "完整规范：docs/DESIGN.md",
            NavigateUri = new Uri("https://github.com/wynter-yaxley/atrium-desktop/blob/main/docs/DESIGN.md"),
            Padding = new Thickness(0),
        };
        DesignStack.Children.Add(link);
    }

    private static UIElement Principles()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 240, Gap = 16 };
        (string N, string Title, string Body)[] items =
        {
            ("01", "让位于内容", "玻璃是承载内容的介质。字色跟随背后的亮度，装饰的强度以文字始终清晰为限。"),
            ("02", "运动像真实物体", "可以中途打断，速度不突变，与帧率无关；弹出动画交给合成器播放。"),
            ("03", "同类事物，同一种处理", "同一类元素用同一条曲线、同一个圆角、同一档字号。"),
        };
        foreach (var (n, title, body) in items)
        {
            var s = new StackPanel { Spacing = 8 };
            var num = Text(n, "ValueStyle");
            num.Foreground = Tokens.Brush("AccentBrush");
            s.Children.Add(num);
            s.Children.Add(Text(title, "RowTitleStyle"));
            s.Children.Add(Text(body, "RowNoteStyle"));
            grid.Children.Add(Card(s));
        }
        return Section("原则", null, grid);
    }

    // ---- Motion ----

    private UIElement CurveGrid()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 280, Gap = 16 };
        foreach (var c in Curves) grid.Children.Add(CurveCard(c));
        return grid;
    }

    private FrameworkElement CurveCard(Curve c)
    {
        // Drawn again at the card's width: the curve, its 0 and 1 lines, a cursor on it, and below
        // the same motion on a moving chip, the way it looks on screen.
        const double H = 96, Y0 = 82, Y1 = 22, Chip = 22;
        double w = 240;
        double X(double t) => 4 + (w - 8) * t / c.Seconds;
        double Y(double v) => Y0 + (Y1 - Y0) * v;

        var plot = new Canvas { Height = H };
        var lines = new[] { new Line { Style = StyleOf("GlyphFaintStyle"), StrokeDashArray = new DoubleCollection { 2, 3 } },
                            new Line { Style = StyleOf("GlyphFaintStyle"), StrokeDashArray = new DoubleCollection { 2, 3 } } };
        var line = new Polyline { Style = StyleOf("GlyphAccentStyle"), Points = new PointCollection() };
        var cursor = new Ellipse { Width = 8, Height = 8, Style = StyleOf("GlyphAccentFillStyle") };
        foreach (var l in lines) plot.Children.Add(l);
        plot.Children.Add(line);
        plot.Children.Add(cursor);

        var track = new Grid { Height = 22 };
        track.Children.Add(new Border { CornerRadius = new CornerRadius(11), Background = Tokens.Brush("FillBrush") });
        var lane = new Canvas();
        var chip = new Rectangle { Width = Chip, Height = 14, RadiusX = 7, RadiusY = 7, Style = StyleOf("GlyphAccentFillStyle") };
        Canvas.SetTop(chip, 4);
        lane.Children.Add(chip);
        track.Children.Add(lane);

        double shown = c.Seconds;
        void Show(double t)
        {
            shown = Math.Min(t, c.Seconds);
            double v = c.At(shown);
            Canvas.SetLeft(cursor, X(shown) - 4);
            Canvas.SetTop(cursor, Y(v) - 4);
            Canvas.SetLeft(chip, 4 + (w - Chip - 8) * v);
        }
        void Draw(double width)
        {
            w = Math.Max(120, width);
            for (int k = 0; k < 2; k++)
            {
                lines[k].X2 = w;
                lines[k].Y1 = lines[k].Y2 = Y(k);
            }
            line.Points.Clear();
            for (int i = 0; i <= 120; i++)
            {
                double t = c.Seconds * i / 120;
                line.Points.Add(new Point(X(t), Y(c.At(t))));
            }
            Show(shown);
        }
        plot.SizeChanged += (_, e) => Draw(e.NewSize.Width);
        Draw(w);

        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(Text(c.Name, "RowTitleStyle"));
        var spec = Text(c.Spec, "RowNoteStyle");
        Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(spec, FontNumeralAlignment.Tabular);
        text.Children.Add(spec);
        text.Children.Add(Text(c.Use, "RowNoteStyle"));

        var body = new StackPanel { Spacing = 10 };
        body.Children.Add(text);
        body.Children.Add(plot);
        body.Children.Add(track);
        var card = Card(body);
        AutomationProperties.SetName(card, c.Name);
        card.PointerPressed += (_, _) => Play(c, Show);
        return card;
    }

    private void Play(Curve c, Action<double> show)
    {
        _playing.RemoveAll(p => p.Show == show);
        _playing.Add((DesignClock.Elapsed.TotalSeconds, c, show));
        show(0);
        if (_designTicking) return;
        _designTicking = true;
        CompositionTarget.Rendering += OnDesignFrame;
    }

    private void OnDesignFrame(object? sender, object e)
    {
        double now = DesignClock.Elapsed.TotalSeconds;
        for (int i = _playing.Count - 1; i >= 0; i--)
        {
            var (start, c, show) = _playing[i];
            show(now - start);
            if (now - start >= c.Seconds) _playing.RemoveAt(i);
        }
        if (_playing.Count > 0) return;
        CompositionTarget.Rendering -= OnDesignFrame;
        _designTicking = false;
    }

    // ---- Type ----

    private static UIElement TypeCard()
    {
        var rows = new StackPanel { Spacing = 0 };
        for (int i = 0; i < TypeRamp.Length; i++)
        {
            var (name, size, font, weight, mac, use) = TypeRamp[i];
            var g = new Grid { ColumnSpacing = 16, Padding = new Thickness(0, 10, 0, 10) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(150) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(190) });
            var sample = new TextBlock
            {
                Text = "玻璃之下，内容优先 Aa 0123",
                FontFamily = (FontFamily)Application.Current.Resources[font],
                FontSize = size,
                VerticalAlignment = VerticalAlignment.Center,
                TextTrimming = TextTrimming.CharacterEllipsis,
            };
            var spec = Text($"{name} · {size} · {weight}", "RowNoteStyle");
            spec.VerticalAlignment = VerticalAlignment.Center;
            var where = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            where.Children.Add(Text("macOS " + mac, "FootnoteStyle"));
            where.Children.Add(Text(use, "RowNoteStyle"));
            Grid.SetColumn(spec, 1);
            Grid.SetColumn(where, 2);
            g.Children.Add(sample);
            g.Children.Add(spec);
            g.Children.Add(where);
            if (i > 0) rows.Children.Add(new Rectangle { Height = 1, Fill = Tokens.Brush("SeparatorBrush") });
            rows.Children.Add(g);
        }
        return Card(rows);
    }

    // ---- Corners ----

    private static UIElement RadiusCard()
    {
        // Concentric: the bar's capsule radius, a 12 inset, the inner radius that keeps the gap even.
        const double R = 30, P = 12, Ww = 240, Hh = 132;
        var demo = new Canvas { Width = Ww, Height = Hh };
        var outer = new Rectangle { Width = Ww, Height = Hh, RadiusX = R, RadiusY = R, Style = StyleOf("GlyphInkStyle") };
        var inner = new Rectangle { Width = Ww - 2 * P, Height = Hh - 2 * P, RadiusX = R - P, RadiusY = R - P, Style = StyleOf("GlyphAccentStyle") };
        Canvas.SetLeft(inner, P);
        Canvas.SetTop(inner, P);
        demo.Children.Add(outer);
        demo.Children.Add(inner);
        var label = Text($"R {R} − 边距 {P} = r {R - P}", "RowTitleStyle");
        label.Width = Ww - 2 * P;
        label.TextAlignment = TextAlignment.Center;
        Canvas.SetLeft(label, P);
        Canvas.SetTop(label, Hh / 2 - 10);
        demo.Children.Add(label);
        var left = new StackPanel { Spacing = 10, HorizontalAlignment = HorizontalAlignment.Center };
        left.Children.Add(demo);
        var why = Text("两条弧共用一个圆心，四周边距处处相等。", "RowNoteStyle");
        why.HorizontalAlignment = HorizontalAlignment.Center;
        left.Children.Add(why);

        var ladder = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 20, VerticalAlignment = VerticalAlignment.Center };
        (double R, string Use)[] steps = { (6, "最小"), (10, "悬停底板"), (22, "模块 · 小组件"), (30, "栏与面板") };
        foreach (var (r, use) in steps)
        {
            var s = new StackPanel { Spacing = 6, HorizontalAlignment = HorizontalAlignment.Center };
            // The bar's radius is half its height: drawn as the capsule it makes.
            s.Children.Add(new Rectangle { Width = r >= 30 ? 100 : 60, Height = 60, RadiusX = r, RadiusY = r, Style = StyleOf("GlyphInkStyle"), HorizontalAlignment = HorizontalAlignment.Center });
            var v = Text(r.ToString(), "RowTitleStyle");
            v.HorizontalAlignment = HorizontalAlignment.Center;
            s.Children.Add(v);
            var u = Text(use, "FootnoteStyle");
            u.HorizontalAlignment = HorizontalAlignment.Center;
            s.Children.Add(u);
            ladder.Children.Add(s);
        }
        var grid = new ResponsiveGrid { MinCellWidth = 300, Gap = 24 };
        grid.Children.Add(left);
        grid.Children.Add(ladder);
        return Card(grid);
    }

    // ---- Sizes ----

    private UIElement SizeCard()
    {
        // This screen: physical pixels of the window's display over its scale.
        double scale = Content.XamlRoot?.RasterizationScale ?? 1;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).OuterBounds;
        double logical = area.Width / scale;
        int mine = 0;
        for (int i = 0; i < SizeClasses.Length; i++)
            if (logical >= SizeClasses[i].From) mine = i;

        var g = new Grid { ColumnSpacing = 12, RowSpacing = 0 };
        foreach (var w in new[] { 1.1, 2.6, 0.7, 0.7, 0.7, 0.8 })
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w, GridUnitType.Star) });
        string[] head = { "逻辑宽度", "典型屏幕", "栏高", "图标", "底边距", "放大" };
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (int c = 0; c < head.Length; c++)
        {
            var t = Text(head[c], "FootnoteStyle");
            t.Margin = new Thickness(c == 0 ? 10 : 0, 0, 0, 8);
            Grid.SetColumn(t, c);
            g.Children.Add(t);
        }
        for (int i = 0; i < SizeClasses.Length; i++)
        {
            var k = SizeClasses[i];
            int row = i + 1;
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            if (i == mine)
            {
                var band = new Border { CornerRadius = new CornerRadius(8), Background = Tokens.Brush("FillBrush") };
                Grid.SetRow(band, row);
                Grid.SetColumnSpan(band, head.Length);
                g.Children.Add(band);
            }
            string range = i + 1 < SizeClasses.Length ? $"{(k.From == 0 ? "< " : k.From + "–")}{SizeClasses[i + 1].From - (k.From == 0 ? 0 : 1)}" : $"≥ {k.From}";
            string[] cells = { range, k.Screens, k.Height.ToString(), k.Icon.ToString(), k.Margin.ToString(), k.Magnify.ToString("0.0") };
            for (int c = 0; c < cells.Length; c++)
            {
                var t = Text(cells[c], c == 0 ? "RowTitleStyle" : "RowNoteStyle");
                t.FontSize = c == 0 ? 13 : 12;
                t.Margin = new Thickness(c == 0 ? 10 : 0, 9, c == cells.Length - 1 ? 10 : 0, 9);
                t.VerticalAlignment = VerticalAlignment.Center;
                Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(t, FontNumeralAlignment.Tabular);
                Grid.SetRow(t, row);
                Grid.SetColumn(t, c);
                g.Children.Add(t);
            }
        }
        var k0 = SizeClasses[mine];
        var here = Text($"这块屏幕：{area.Width} × {area.Height} 像素，缩放 {scale * 100:0}% → 逻辑宽度 {logical:0}。建议栏高 {k0.Height}、图标 {k0.Icon}；当前设置为栏高 {_current.Height:0}、图标 {_current.IconSize:0}。",
            "RowNoteStyle");
        here.Margin = new Thickness(10, 12, 10, 0);
        var s = new StackPanel();
        s.Children.Add(g);
        s.Children.Add(here);
        return Card(s);
    }

    // ---- Ink ----

    private static UIElement InkCard()
    {
        Border Panel(Color bg, Color ink, (string Name, double A)[] steps, Color warn, string title)
        {
            var s = new StackPanel { Spacing = 6 };
            var head = new TextBlock { Text = title, FontSize = 11, Foreground = new SolidColorBrush(Color.FromArgb(150, ink.R, ink.G, ink.B)) };
            s.Children.Add(head);
            foreach (var (name, a) in steps)
                s.Children.Add(new TextBlock
                {
                    Text = $"{name} · {a * 100:0}%",
                    FontSize = 13.5,
                    FontFamily = (FontFamily)Application.Current.Resources[name == "主文字" ? "EmphasisFont" : "TextFont"],
                    Foreground = new SolidColorBrush(Color.FromArgb((byte)Math.Round(a * 255), ink.R, ink.G, ink.B)),
                });
            var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 6, 0, 0) };
            foreach (var (c, label) in new[] { (Color.FromArgb(255, 0x0A, 0x84, 0xFF), "#0A84FF"), (warn, $"#{warn.R:X2}{warn.G:X2}{warn.B:X2}") })
            {
                var chip = new Border { CornerRadius = new CornerRadius(10), Padding = new Thickness(10, 3, 10, 4), Background = new SolidColorBrush(c) };
                chip.Child = new TextBlock { Text = label, FontSize = 11, Foreground = new SolidColorBrush(Colors.White) };
                chips.Children.Add(chip);
            }
            s.Children.Add(chips);
            return new Border { Background = new SolidColorBrush(bg), CornerRadius = new CornerRadius(12), Padding = new Thickness(18, 14, 18, 16), Child = s };
        }
        var grid = new ResponsiveGrid { MinCellWidth = 280, Gap = 16 };
        grid.Children.Add(Panel(Color.FromArgb(255, 0x24, 0x2A, 0x3A), Colors.White, new[] { ("主文字", 0.96), ("次文字", 0.76), ("弱文字", 0.56) },
            Color.FromArgb(255, 0xFF, 0x45, 0x3A), "浅色字 · 暗背景"));
        grid.Children.Add(Panel(Color.FromArgb(255, 0xEC, 0xEE, 0xF3), Color.FromArgb(255, 0x11, 0x13, 0x17), new[] { ("主文字", 0.92), ("次文字", 0.66), ("弱文字", 0.46) },
            Color.FromArgb(255, 0xE0, 0x35, 0x2B), "深色字 · 亮背景"));
        return grid;
    }
}
