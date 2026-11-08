using System.Diagnostics;
using System.Numerics;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The design language page, in two layers. First what Atrium looks and moves like, to look at and
// touch: the two glasses side by side, corners and curvature, type, spacing, colour, the dock and a
// notification, the curves. Then, behind "查看详细参数", the numbers: every one of them taken from
// the code (the curves use the core's own formulas, native/core/motion.h; the materials are
// BarSettings' presets; the colours are this app's resources).
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

        // The launch hop (render/bar_painter.cpp): sin², no speed at either end.
        public static double Hop(double t, double seconds)
        {
            if (t <= 0 || t >= seconds) return 0;
            double s = Math.Sin(Math.PI * t / seconds);
            return s * s;
        }
    }

    private sealed record Curve(string Name, string Spec, string Formula, string Use, double Seconds, Func<double, double> At);

    private static readonly Curve[] Curves =
    {
        new("kPop · 弹簧", "时长 0.36 s · 回弹 0.38", "闭式弹簧，阻尼比 0.62；切成 12 段 Hermite 曲线交给 DirectComposition", "弹出面板出现：缩放 0.92 → 1，峰值 1.0067", 0.6, t => Motion.Spring(t, 0.36, 0.62)),
        new("kSmooth · 弹簧", "时长 0.48 s · 回弹 0.23", "闭式弹簧，阻尼比 0.77", "状态圆环：电量、音量与亮度", 0.8, t => Motion.Spring(t, 0.48, 0.77)),
        new("堆叠 · 临界阻尼弹簧", "时长 0.60 s · 回弹 0", "闭式弹簧，阻尼比 1", "滚轮翻看通知后吸附到一张", 1.0, t => Motion.Spring(t, 0.60, 1.0)),
        new("EaseOut · 三次缓出", "1 − (1 − t)³ · 320 ms", "1 − (1 − t)³", "换页、淡入、列表依次出现、字色渐变", 0.32, t => Motion.EaseOut(t / 0.32)),
        new("EaseOutBack · 回弹缓出", "s = 1.5 · 260 ms", "1 + (s + 1)(t − 1)³ + s(t − 1)²", "开始菜单的格子出现", 0.26, t => Motion.EaseOutBack(t / 0.26, 1.5)),
        new("Approach · 指数渐近", "进 20/s · 出 11/s", "1 − e^(−rate · t)，每帧从当前值继续", "悬停与按下的底板", 0.3, t => Motion.Approach(t, 20)),
        new("启动 · 一次轻跳", "sin² · 480 ms · 高 0.32 倍图标", "sin²(πt / 0.48)，起跳与落地都没有速度突变", "点开任务栏上的应用：只跳一次", 0.48, t => Motion.Hop(t, 0.48)),
    };

    // The type ramp (DESIGN.md §1.2): name, size, MiSans file, its weight, macOS counterpart, where.
    private static readonly (string Name, double Size, string Font, string Weight, string Mac, string Use)[] TypeRamp =
    {
        ("Large Title", 30, "TitleFont", "Semibold 520", "Large Title · 26", "页标题、产品名"),
        ("Title", 20, "TitleFont", "Semibold 520", "Title 1 · 22", "空状态标题、日历的月份"),
        ("Value", 15, "EmphasisFont", "Demibold 450", "—", "滑块的数值（等宽数字）"),
        ("Row", 15, "TextFont", "Medium 380", "Body · 13", "设置中心的行标题"),
        ("Body", 13.5, "TextFont", "Medium 380", "Body · 13", "面板正文、通知内容"),
        ("Section", 13, "EmphasisFont", "Demibold 450", "—", "分组标题"),
        ("Note", 13, "TextFont", "Medium 380", "Callout · 12", "行下方的说明"),
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

    // The spacing scale (DESIGN.md §4.1) and the corner ladder (§2.1).
    private static readonly (string Name, int Value, string Use)[] Spacing =
    {
        ("2xs", 4, "图标与角标、紧贴的元素"), ("xs", 8, "同一组内相邻的元素"), ("s", 10, "面板内卡片之间（kGap）"),
        ("m", 12, "弹出面板与栏之间（kPopupGapDip）"), ("l", 14, "面板内边距（kPad）、玻璃阴影"), ("xl", 16, "小组件之间的间隙"),
        ("2xl", 24, "小组件到屏幕边缘、区块之间"),
    };

    private static readonly (string Element, string Radius, string Rule)[] Corners =
    {
        ("栏", "cornerRadius，默认 30", "不超过栏高的一半：默认是胶囊形"),
        ("弹出面板", "与栏相同，8 到栏高 / 2", "面板与栏是同一种材质、同一种形状"),
        ("面板内的模块", "min(面板圆角, 22)", "面板很圆时，模块不跟着变成胶囊"),
        ("托盘图标底板", "clamp(R − m − 2, 6, 12)", "同心：外层圆角减去边距"),
        ("桌面小组件", "22（格子 164）", "约为边长的 13%，与 macOS 小组件相同"),
        ("胶囊控件", "高度 / 2", "G3 连续端部；圆形旋钮保持正圆"),
        ("悬停与按下的底板", "8–12", "不超过所在容器圆角减去边距"),
    };

    private bool _designBuilt;
    private readonly List<(double Start, Curve Curve, Action<double> Show)> _playing = new();
    private bool _designTicking;
    private static readonly Stopwatch DesignClock = Stopwatch.StartNew();
    private StackPanel? _designDetails;

    private static Style StyleOf(string key) => (Style)Application.Current.Resources[key];

    private static TextBlock Text(string text, string style, double? size = null)
    {
        var t = new TextBlock { Text = text, Style = StyleOf(style) };
        if (size is double s) t.FontSize = s;
        return t;
    }

    private static TextBlock Tabular(TextBlock t)
    {
        Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(t, FontNumeralAlignment.Tabular);
        return t;
    }

    private static Border Card(UIElement child) => new() { Style = StyleOf("TileStyle"), Padding = new Thickness(20, 16, 20, 16), Child = child };

    private static StackPanel Section(string label, string? note, UIElement body)
    {
        var s = new StackPanel { Spacing = 12 };
        var title = Text(label, "TitleStyle");
        title.Margin = new Thickness(4, 0, 0, 0);
        s.Children.Add(title);
        if (note != null)
        {
            var n = Text(note, "RowNoteStyle");
            n.Margin = new Thickness(4, -4, 0, 2);
            n.MaxWidth = 760;
            n.HorizontalAlignment = HorizontalAlignment.Left;
            s.Children.Add(n);
        }
        s.Children.Add(body);
        return s;
    }

    private void BuildDesignPage()
    {
        if (_designBuilt) return;
        _designBuilt = true;
        // ---- First layer: to look at and to touch ----
        DesignStack.Children.Add(Principles());
        DesignStack.Children.Add(Section("材质", "同一个着色器、两组参数：液态玻璃只在边缘折射、中间几乎透明；磨砂玻璃把背景磨成色块、几乎不折射。左右拖动分界线对比。",
            MaterialCompare()));
        DesignStack.Children.Add(Section("几何", "统一 G3 连续圆角，保持大圆角与胶囊比例。",
            GeometryCard()));
        DesignStack.Children.Add(Section("排版", "MiSans 三个字重。一个界面最多三档字号；主次先用透明度区分，再用字号，最后才用字重；数字一律等宽。", TypeCard(false)));
        DesignStack.Children.Add(Section("间距与色彩", "间距只用一组刻度；颜色只有一个强调色，其余都是中性灰。玻璃上的字色由背后的亮度决定。", SpaceAndColour()));
        DesignStack.Children.Add(Section("交互", "把指针移到任务栏上：图标像波浪一样依次变大（倍数取当前设置）；点一个图标，看启动时的一次轻跳。点通知卡片，重新弹出。", InteractionCard()));
        DesignStack.Children.Add(Section("动效", "动画可从当前位置改向，并按实际时间推进；弹簧保留速度，小组件缓出目前会重启。点卡片播放。", CurveGrid()));

        // ---- Second layer: the numbers ----
        var toggle = new Button { HorizontalAlignment = HorizontalAlignment.Left, Padding = new Thickness(16, 7, 14, 8) };
        var label = new TextBlock { Text = "查看详细参数" };
        var chevron = new FontIcon { Glyph = "\uE70D", FontSize = 11, Margin = new Thickness(10, 1, 0, 0) };
        chevron.RenderTransformOrigin = new Point(0.5, 0.5);
        chevron.RenderTransform = new RotateTransform();
        toggle.Content = new StackPanel { Orientation = Orientation.Horizontal, Children = { label, chevron } };
        AutomationProperties.SetName(toggle, "查看详细参数");
        _designDetails = new StackPanel { Spacing = 36, Visibility = Visibility.Collapsed };
        toggle.Click += (_, _) =>
        {
            RefreshDesignDetails(true);
            bool open = _designDetails.Visibility != Visibility.Visible;
            _designDetails.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            label.Text = open ? "收起详细参数" : "查看详细参数";
            ((RotateTransform)chevron.RenderTransform).Angle = open ? 180 : 0;
            if (open) AnimateIn(_designDetails);
        };
        var foot = new StackPanel { Spacing = 8 };
        foot.Children.Add(Text("技术细节", "TitleStyle"));
        var why = Text("动画的曲线与时长、材质预设的每个参数、字号与字重、圆角与间距：与 Atrium 运行时使用的是同一份数值。", "RowNoteStyle");
        why.Margin = new Thickness(0, 0, 0, 6);
        foot.Children.Add(why);
        foot.Children.Add(toggle);
        foot.Margin = new Thickness(4, 0, 0, 0);
        DesignStack.Children.Add(foot);
        DesignStack.Children.Add(_designDetails);
    }

    // The details are built when they are opened, and again with the current settings each time the page shows.
    private void RefreshDesignDetails(bool opening = false)
    {
        if (_designDetails == null || (!opening && _designDetails.Visibility != Visibility.Visible)) return;
        _designDetails.Children.Clear();
        BuildDesignDetails(_designDetails);
        ContinuousCorners.Install(_designDetails);
    }

    private void BuildDesignDetails(StackPanel into)
    {
        into.Children.Add(Section("动效参数", "弹簧按 Apple 的方式给出：时长与回弹（= 1 − 阻尼比）。弹出动画由 DirectComposition 在合成器里播放，其余由渲染线程按合成时钟逐帧计算。",
            Table(new[] { "曲线", "参数", "形式", "用于" }, new[] { 1.2, 1.2, 2.0, 1.6 },
                Curves.Select(c => new[] { c.Name, c.Spec, c.Formula, c.Use }))));
        var liquid = BarSettings.Liquid();
        var frosted = BarSettings.Frosted();
        string P(double v) => $"{v * 100:0}%";
        string Hex(uint c) => "#" + c.ToString("X6");
        into.Children.Add(Section("材质参数", "两个预设与当前设置。栏、弹出面板与小组件用同一个着色器（glass.hlsl）和同一组参数。",
            Table(new[] { "参数", "液态玻璃", "磨砂玻璃", "当前" }, new[] { 1.4, 1.0, 1.0, 1.0 }, new[]
            {
                new[] { "透明度", P(liquid.Transparency), P(frosted.Transparency), P(_current.Transparency) },
                new[] { "模糊", P(liquid.Blur), P(frosted.Blur), P(_current.Blur) },
                new[] { "折射强度", $"{liquid.Refraction:0} px", $"{frosted.Refraction:0} px", $"{_current.Refraction:0} px" },
                new[] { "折射边缘宽度", $"{liquid.Edge:0} px", $"{frosted.Edge:0} px", $"{_current.Edge:0} px" },
                new[] { "色散", P(liquid.Dispersion), P(frosted.Dispersion), P(_current.Dispersion) },
                new[] { "高光", P(liquid.Highlight), P(frosted.Highlight), P(_current.Highlight) },
                new[] { "阴影", P(liquid.Shadow), P(frosted.Shadow), P(_current.Shadow) },
                new[] { "染色", Hex(liquid.Tint), Hex(frosted.Tint), Hex(_current.Tint) },
            })));
        into.Children.Add(Section("排版参数", "字重从不合成：样式选择字体文件。MiSans 的字重比名称细一档：Medium 380 当作常规，Demibold 450 当作中等，Semibold 520 当作半粗。",
            TypeCard(true)));
        into.Children.Add(Section("圆角", "统一解析 G3 轮廓：直边接点的曲率、曲率变化率均为零。卡片 24，按钮按半高取值；正圆保留恒定曲率。",
            Table(new[] { "元素", "圆角", "规则" }, new[] { 1.1, 1.3, 2.2 }, Corners.Select(c => new[] { c.Element, c.Radius, c.Rule }))));
        into.Children.Add(RadiusCard());
        into.Children.Add(Section("间距", null,
            Table(new[] { "名称", "值", "用途" }, new[] { 0.6, 0.5, 2.6 }, Spacing.Select(s => new[] { s.Name, s.Value.ToString(), s.Use }))));
        into.Children.Add(Section("尺寸", "先把分辨率换算成逻辑宽度（像素 ÷ 缩放），再查栏的尺寸。图标是栏高的一半，栏高约为逻辑屏幕高度的 6%。", SizeCard()));
        into.Children.Add(Section("字色", "由玻璃背后的亮度决定：高于 0.50 用深色字，低于 0.42 换回浅色字，两者之间不切换；切换时 450 ms 缓出渐变。字色分三档透明度。", InkCard()));
        var link = new HyperlinkButton
        {
            Content = "完整规范：docs/DESIGN.md",
            NavigateUri = new Uri("https://github.com/wynter-yaxley/atrium-desktop/blob/main/docs/DESIGN.md"),
            Padding = new Thickness(0),
        };
        into.Children.Add(link);
    }

    // A plain table on a card: a header in footnote ink, rows split by hairlines, numbers tabular.
    private static Border Table(string[] head, double[] widths, IEnumerable<string[]> rows)
    {
        var g = new Grid { ColumnSpacing = 16 };
        foreach (var w in widths) g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w, GridUnitType.Star) });
        int r = 0;
        void Row(string[] cells, bool header)
        {
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            if (!header && r > 1)
            {
                var line = new Rectangle { Height = 1, Fill = Tokens.Brush("SeparatorBrush"), VerticalAlignment = VerticalAlignment.Top };
                Grid.SetRow(line, r);
                Grid.SetColumnSpan(line, head.Length);
                g.Children.Add(line);
            }
            for (int c = 0; c < cells.Length && c < head.Length; c++)
            {
                var t = Tabular(Text(cells[c], header ? "FootnoteStyle" : c == 0 ? "RowTitleStyle" : "RowNoteStyle"));
                if (!header && c == 0) t.FontSize = 14;
                t.TextWrapping = TextWrapping.Wrap;
                t.Margin = new Thickness(0, header ? 0 : 9, 0, header ? 6 : 9);
                Grid.SetRow(t, r);
                Grid.SetColumn(t, c);
                g.Children.Add(t);
            }
            r++;
        }
        Row(head, true);
        foreach (var row in rows) Row(row, false);
        return Card(g);
    }

    private static UIElement Principles()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 240, Gap = 16, StretchLastRow = true };
        (string N, string Title, string Body)[] items =
        {
            ("01", "让位于内容", "玻璃是承载内容的介质。字色跟随背后的亮度，装饰的强度以文字始终清晰为限。"),
            ("02", "运动像真实物体", "可以中途改向，保持位置连续；弹簧保留速度，小组件缓出目前会重启。"),
            ("03", "同类事物，同一种处理", "同一类元素用同一条曲线、同一个圆角、同一档字号。"),
        };
        foreach (var (n, title, body) in items)
        {
            var s = new StackPanel { Spacing = 8 };
            var num = Text(n, "ValueStyle");
            num.HorizontalAlignment = HorizontalAlignment.Left;
            num.Foreground = Tokens.Brush("AccentBrush");
            s.Children.Add(num);
            s.Children.Add(Text(title, "RowTitleStyle"));
            s.Children.Add(Text(body, "RowNoteStyle"));
            grid.Children.Add(Card(s));
        }
        return Section("原则", null, grid);
    }

    // ---- Material: the two presets on one photo, split by a handle the pointer moves ----

    private UIElement MaterialCompare()
    {
        const double H = 240;
        var frosted = new Border { CornerRadius = new CornerRadius(20), Background = PhotoBrush() };
        var liquid = new Border { CornerRadius = new CornerRadius(20), Background = PhotoBrush() };
        // Keep the split's XAML rectangle clip on a separate, unrounded host.
        // XAML otherwise replaces the same visual Clip used for the photo's
        // continuous boundary, and attempts to treat it as its own inset clip.
        var reveal = new Grid { Children = { liquid } };
        var split = new Rectangle { Width = 2, Fill = new SolidColorBrush(Color.FromArgb(230, 255, 255, 255)), HorizontalAlignment = HorizontalAlignment.Left };
        var knob = new Border
        {
            Width = 30, Height = 30, CornerRadius = new CornerRadius(15), Background = new SolidColorBrush(Color.FromArgb(240, 255, 255, 255)),
            HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Center,
            Child = new FontIcon { Glyph = "\uE76F", FontSize = 12, Foreground = new SolidColorBrush(Color.FromArgb(200, 0x1D, 0x1D, 0x1F)) },
            Shadow = new ThemeShadow(), Translation = new Vector3(0, 0, 8),
        };
        Border Tag(string text, HorizontalAlignment side) => new()
        {
            HorizontalAlignment = side, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(14), CornerRadius = new CornerRadius(8),
            Padding = new Thickness(8, 3, 8, 4), Background = new SolidColorBrush(Color.FromArgb(150, 0, 0, 0)),
            Child = new TextBlock { Text = text, FontSize = 12, Foreground = new SolidColorBrush(Colors.White) },
        };
        var stage = new Grid { Height = H, Background = new SolidColorBrush(Colors.Transparent) };
        stage.Children.Add(frosted);
        stage.Children.Add(reveal);
        stage.Children.Add(split);
        stage.Children.Add(knob);
        stage.Children.Add(Tag("液态玻璃", HorizontalAlignment.Left));
        stage.Children.Add(Tag("磨砂玻璃", HorizontalAlignment.Right));
        stage.Children.Add(new Border { CornerRadius = new CornerRadius(20), BorderThickness = new Thickness(1), BorderBrush = new SolidColorBrush(Color.FromArgb(31, 0, 0, 0)), IsHitTestVisible = false });
        AutomationProperties.SetName(stage, "材质对比");

        double at = 0.5, width = 0;
        void Place()
        {
            double x = Math.Round(width * at);
            reveal.Clip = new RectangleGeometry { Rect = new Rect(0, 0, Math.Max(0, x), H) };
            split.Translation = new Vector3((float)(x - 1), 0, 0);
            knob.Translation = new Vector3((float)(x - 15), 0, 8);
        }
        bool dragging = false;
        stage.PointerPressed += (_, e) => { dragging = stage.CapturePointer(e.Pointer); at = Math.Clamp(e.GetCurrentPoint(stage).Position.X / Math.Max(1, width), 0.04, 0.96); Place(); };
        stage.PointerMoved += (_, e) => { if (!dragging) return; at = Math.Clamp(e.GetCurrentPoint(stage).Position.X / Math.Max(1, width), 0.04, 0.96); Place(); };
        stage.PointerReleased += (_, e) => { dragging = false; stage.ReleasePointerCapture(e.Pointer); };
        stage.PointerCaptureLost += (_, _) => dragging = false;

        // Both renders at the stage's size, on the thread pool; redone when the width changes much,
        // and over a new picture when another is picked.
        double rendered = 0;
        int request = 0; // the newest render: an older one finishing later is dropped
        void Render()
        {
            rendered = width;
            double rs = Math.Clamp(stage.XamlRoot?.RasterizationScale ?? 1, 1, 2), w = width;
            int mine = ++request;
            int photoGeneration = Photo.Generation;
            var photo = Photo.Capture();
            Task.Run(() =>
            {
                var a = RenderMaterial(BarSettings.Liquid(), w, H, rs, photo.Value);
                var b = RenderMaterial(BarSettings.Frosted(), w, H, rs, photo.Value);
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (mine != request || photoGeneration != Photo.Generation) return;
                    liquid.Background = a.Brush();
                    frosted.Background = b.Brush();
                    Ready("design", photoGeneration);
                });
            });
        }
        stage.SizeChanged += (_, e) =>
        {
            width = e.NewSize.Width;
            Place();
            if (Math.Abs(width - rendered) >= 48) Render();
        };
        PhotoChanged += () => { if (width > 0) Render(); };
        _compareBuilt = true;
        return stage;
    }

    private sealed record Rendered(int W, int H, byte[] Pixels)
    {
        public ImageBrush Brush()
        {
            var bmp = new WriteableBitmap(W, H);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(Pixels, 0, Pixels.Length);
            bmp.Invalidate();
            return new ImageBrush { ImageSource = bmp, Stretch = Stretch.Fill };
        }
    }

    // A long capsule of the material across the photo, 84 DIP high (the bar at 1.4×).
    private static Rendered RenderMaterial(BarSettings material, double w, double h, double rs, Photo photo)
    {
        int pw = (int)Math.Round(w * rs), ph = (int)Math.Round(h * rs);
        var bg = Backdrop.Create(pw, ph, w, h, 0, 0, rs, photo);
        var s = material.Clone();
        s.Height = 60;
        s.CornerRadius = 30;
        double k = 1.4;
        var spec = GlassSpec.For(s, k * rs);
        int inset = (int)spec.Inset;
        int bw = (int)Math.Round((w - 80) * rs), bh = (int)Math.Round(60 * k * rs);
        int bx = (pw - bw) / 2, by = (ph - bh) / 2;
        var pixels = new byte[bg.Base.Length];
        Buffer.BlockCopy(bg.Base, 0, pixels, 0, pixels.Length);
        GlassRenderer.Draw(bg, pixels, bx - inset, by - inset, bw + 2 * inset, bh + 2 * inset, spec);
        return new Rendered(pw, ph, pixels);
    }

    // ---- Geometry: the same continuous corner profile used by the application ----

    private UIElement GeometryCard()
    {
        var outline = new Rectangle
        {
            Width = 176, Height = 176, RadiusX = 46, RadiusY = 46,
            Style = StyleOf("GlyphAccentStyle"), HorizontalAlignment = HorizontalAlignment.Center,
        };
        var face = new StackPanel { Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top };
        face.Children.Add(outline);
        var caption = Text("G3 大圆角", "RowTitleStyle");
        caption.HorizontalAlignment = HorizontalAlignment.Center;
        caption.MinHeight = caption.LineHeight = 22;
        face.Children.Add(caption);
        var grid = new ResponsiveGrid { MinCellWidth = 300, MaxColumns = 2, Gap = 28 };
        grid.Children.Add(face);
        grid.Children.Add(RadiusDemo());
        return Card(grid);
    }
    // The concentric rule in one picture: the bar's capsule radius, a 12 inset, the inner radius.
    private static UIElement RadiusDemo()
    {
        const double R = 30, P = 12, Ww = 220, Hh = 176;
        var demo = new Canvas { Width = Ww, Height = Hh };
        demo.Children.Add(new Rectangle { Width = Ww, Height = Hh, RadiusX = R, RadiusY = R, Style = StyleOf("GlyphInkStyle") });
        var inner = new Rectangle { Width = Ww - 2 * P, Height = Hh - 2 * P, RadiusX = R - P, RadiusY = R - P, Style = StyleOf("GlyphAccentStyle") };
        Canvas.SetLeft(inner, P);
        Canvas.SetTop(inner, P);
        demo.Children.Add(inner);
        var label = Tabular(Text($"R {R} − 边距 {P} = r {R - P}", "RowTitleStyle"));
        label.Width = Ww - 2 * P;
        label.TextAlignment = TextAlignment.Center;
        Canvas.SetLeft(label, P);
        Canvas.SetTop(label, Hh / 2 - 10);
        demo.Children.Add(label);
        var s = new StackPanel { Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top };
        s.Children.Add(demo);
        var why = Text("嵌套：内圆角随边距缩小，保持一致的轮廓。", "RowNoteStyle");
        why.HorizontalAlignment = HorizontalAlignment.Center;
        why.MinHeight = why.LineHeight = 22;
        s.Children.Add(why);
        return s;
    }

    // ---- Spacing and colour ----

    private static UIElement SpaceAndColour()
    {
        var space = new ResponsiveGrid { MinCellWidth = 280, MaxColumns = 1, Gap = 10, DistributeRows = true };
        foreach (var (name, value, _) in Spacing)
        {
            var row = new Grid { ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.Children.Add(Tabular(Text(name, "RowNoteStyle")));
            var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            bar.Children.Add(new Rectangle { Width = value * 6, Height = 10, RadiusX = 3, RadiusY = 3, Fill = Tokens.Brush("AccentBrush"), Opacity = 0.35 + 0.65 * value / 24.0, VerticalAlignment = VerticalAlignment.Center });
            bar.Children.Add(Tabular(Text(value.ToString(), "FootnoteStyle")));
            Grid.SetColumn(bar, 1);
            row.Children.Add(bar);
            space.Children.Add(row);
        }

        var colours = new ResponsiveGrid { MinCellWidth = 112, MaxColumns = 4, Gap = 12, DistributeRows = true };
        (string Key, string Name)[] tokens =
        {
            ("AccentBrush", "强调色"), ("InkBrush", "主文字"), ("InkSecondaryBrush", "次文字"), ("InkTertiaryBrush", "弱文字"),
            ("FillBrush", "填充"), ("SeparatorBrush", "分隔线"), ("CardBrush", "卡片"), ("PageBrush", "页面"),
        };
        foreach (var (key, name) in tokens) colours.Children.Add(Chip(Tokens.Brush(key), name));
        // The few colours widgets use (WidgetFaces.cs, widget_faces.cpp), as on macOS.
        foreach (var (c, name) in new[] { (WidgetFaces.Green, "正常 · 绿"), (WidgetFaces.Orange, "提醒 · 橙"), (WidgetFaces.Red, "警示 · 红"), (WidgetFaces.Blue, "日历 · 蓝") })
            colours.Children.Add(Chip(new SolidColorBrush(c), name));

        var grid = new ResponsiveGrid { MinCellWidth = 300, Gap = 28, StretchLastRow = true };
        grid.Children.Add(space);
        // Bring the outer colour rows in towards xs and xl, leaving room above and below.
        grid.Children.Add(new Border { Padding = new Thickness(0, 24, 0, 24), Child = colours });
        return Card(grid);
    }

    private static UIElement Chip(Brush brush, string name)
    {
        var row = new Grid { ColumnSpacing = 10 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(28) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.Children.Add(new Border { Width = 28, Height = 28, VerticalAlignment = VerticalAlignment.Center, CornerRadius = new CornerRadius(8),
                                      Background = brush, BorderBrush = Tokens.Brush("CardStrokeBrush"), BorderThickness = new Thickness(1) });
        var labels = new StackPanel();
        labels.Children.Add(Text(name, "RowNoteStyle"));
        string hex = brush is SolidColorBrush b ? (b.Color.A == 255 ? $"#{b.Color.R:X2}{b.Color.G:X2}{b.Color.B:X2}" : $"#{b.Color.A:X2}{b.Color.R:X2}{b.Color.G:X2}{b.Color.B:X2}") : "";
        labels.Children.Add(Tabular(Text(hex, "FootnoteStyle")));
        Grid.SetColumn(labels, 1);
        row.Children.Add(labels);
        return row;
    }

    // ---- Interaction: a dock that magnifies under the pointer and hops once, a card that pops ----

    private UIElement InteractionCard()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 340, Gap = 16, StretchLastRow = true };
        grid.Children.Add(DockDemo());
        grid.Children.Add(PopDemo());
        return grid;
    }

    private static Border InteractionFrame(Grid stage, UIElement content)
    {
        var frame = Card(content);
        frame.Padding = new Thickness(16);
        // The border stroke is part of the inset too: the photo follows the same corner centre.
        double inset = frame.BorderThickness.Left + frame.Padding.Left;
        stage.CornerRadius = new CornerRadius(ContinuousCorners.RadiusOf(frame) - inset);
        return frame;
    }

    private FrameworkElement DockDemo()
    {
        const double Icon = 34, Gap = 9, StageH = 190, BarH = 54;
        var stage = new Grid { Height = StageH, Background = PhotoBrush() };
        var lane = new Canvas { Height = StageH };
        stage.Children.Add(lane);
        var bar = new Border
        {
            Height = BarH, CornerRadius = new CornerRadius(BarH / 2), VerticalAlignment = VerticalAlignment.Bottom, HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 0, 0, 16), Background = new SolidColorBrush(Color.FromArgb(120, 255, 255, 255)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(170, 255, 255, 255)), BorderThickness = new Thickness(1),
        };
        stage.Children.Insert(0, bar);
        var apps = _previewPins.Append(_previewRunning).ToArray();
        int n = apps.Length;
        bar.Width = n * Icon + (n - 1) * Gap + 2 * 14;
        var holders = new List<(Border Holder, Image Icon)>();
        for (int i = 0; i < n; i++)
        {
            var icon = new Image
            {
                Width = Icon, Height = Icon, Stretch = Stretch.Uniform, DataContext = apps[i],
            };
            icon.SetBinding(Image.SourceProperty, new Microsoft.UI.Xaml.Data.Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });
            var holder = new Border { Width = Icon, Height = Icon, Child = icon };
            holder.CenterPoint = new Vector3((float)(Icon / 2), (float)Icon, 0);
            Canvas.SetTop(holder, StageH - 16 - BarH / 2 - Icon / 2);
            lane.Children.Add(holder);
            holders.Add((holder, icon));
            var hop = new Curve("hop", "", "", "", 0.48, t => Motion.Hop(t, 0.48));
            Action<double> show = t => icon.Translation = new Vector3(0, (float)(-Motion.Hop(t, 0.48) * 0.32 * Icon * 1.6), 0);
            holder.PointerPressed += (_, _) => Play(hop, show);
            AutomationProperties.SetName(holder, apps[i].Title);
        }
        double width = 0, x0 = 0, row = n * Icon + (n - 1) * Gap;
        // As the bar does (BarPainter): the pointer drives the wave, its strength follows the pointer
        // in and out exponentially (20/s in, 11/s out), all on the frame clock with render transforms
        // only (no layout but the bar's width, no restarted transitions).
        double? pointer = null;
        double amount = 0, lastX = 0;
        long last = 0;
        bool ticking = false;
        void Apply()
        {
            double mag = Math.Max(1, _current.Magnify), k = (mag - 1) * amount, range = (Icon + Gap) * 2.3;
            double centre = pointer ?? lastX;
            double Map(double u)
            {
                if (k <= 0) return u;
                double x = (u - centre) / range;
                return u + k * range * (x >= 1 ? 0.5 : x <= -1 ? -0.5 : x / 2 + Math.Sin(Math.PI * x) / (2 * Math.PI));
            }
            for (int i = 0; i < n; i++)
            {
                double left = x0 + i * (Icon + Gap), bx = left + Icon / 2;
                double grow = (Map(bx + Icon / 2) - Map(bx - Icon / 2)) / Icon;
                holders[i].Holder.Translation = new Vector3((float)(Map(bx) - bx), 0, 0);
                holders[i].Holder.Scale = new Vector3((float)grow, (float)grow, 1);
            }
            bar.Width = Map(x0 + row + 14) - Map(x0 - 14);
            bar.Translation = new Vector3((float)((Map(x0 - 14) + Map(x0 + row + 14)) / 2 - (x0 + row / 2)), 0, 0);
        }
        void Tick(object? sender, object e)
        {
            long now = Environment.TickCount64;
            double dt = Math.Clamp((now - last) / 1000.0, 0, 0.05);
            last = now;
            double target = pointer.HasValue ? 1 : 0, rate = pointer.HasValue ? 20 : 11;
            amount += (target - amount) * (1 - Math.Exp(-dt * rate));
            if (Math.Abs(target - amount) < 0.002)
            {
                amount = target;
                CompositionTarget.Rendering -= Tick;
                ticking = false;
            }
            Apply();
        }
        void Wake()
        {
            if (ticking) { Apply(); return; }
            ticking = true;
            last = Environment.TickCount64;
            CompositionTarget.Rendering += Tick;
        }
        stage.SizeChanged += (_, e) =>
        {
            width = e.NewSize.Width;
            x0 = (width - row) / 2;
            for (int i = 0; i < n; i++) Canvas.SetLeft(holders[i].Holder, x0 + i * (Icon + Gap));
            Apply();
        };
        stage.PointerMoved += (_, e) => { pointer = lastX = e.GetCurrentPoint(lane).Position.X; Wake(); };
        stage.PointerExited += (_, _) => { pointer = null; Wake(); };
        stage.Unloaded += (_, _) => { if (ticking) { CompositionTarget.Rendering -= Tick; ticking = false; } };

        var s = new StackPanel { Spacing = 10 };
        s.Children.Add(stage);
        s.Children.Add(Text("悬停放大与启动轻跳", "RowTitleStyle"));
        s.Children.Add(Text("波形：(1 + cos πd) / 2，d 为指针到图标的距离（以 2.3 个图标为半径）；放大倍数取“任务栏”页的设置。", "RowNoteStyle"));
        return InteractionFrame(stage, s);
    }

    private FrameworkElement PopDemo()
    {
        var stage = new Grid { Height = 190, Background = PhotoBrush() };
        var card = new Border
        {
            Width = 300, Padding = new Thickness(14, 12, 16, 13), CornerRadius = new CornerRadius(22), HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center, Background = new SolidColorBrush(Color.FromArgb(205, 250, 250, 252)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(150, 255, 255, 255)), BorderThickness = new Thickness(1),
        };
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock { Text = "日历 · 现在", FontSize = 12, Foreground = new SolidColorBrush(Color.FromArgb(150, 0x1D, 0x1D, 0x1F)) });
        text.Children.Add(new TextBlock { Text = "产品评审", FontSize = 14, FontFamily = (FontFamily)Application.Current.Resources["EmphasisFont"], Foreground = new SolidColorBrush(Color.FromArgb(240, 0x1D, 0x1D, 0x1F)) });
        text.Children.Add(new TextBlock { Text = "点我，再弹出一次。", FontSize = 13, Foreground = new SolidColorBrush(Color.FromArgb(190, 0x1D, 0x1D, 0x1F)) });
        card.Child = text;
        var popup = new Grid { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        popup.Children.Add(card);
        stage.Children.Add(popup);
        stage.PointerPressed += (_, _) => ReplayNotification(popup,
            new Vector2((float)(popup.ActualWidth - _current.Height / 2 - 4), (float)popup.ActualHeight), card);
        AutomationProperties.SetName(stage, "通知弹出示例");

        var s = new StackPanel { Spacing = 10 };
        s.Children.Add(stage);
        s.Children.Add(Text("通知弹出", "RowTitleStyle"));
        s.Children.Add(Text("弹簧 kPop：从 0.92 倍放大到 1，只越过一点点（峰值 1.0067），同时在 160 ms 内淡入。没有来回弹跳。", "RowNoteStyle"));
        return InteractionFrame(stage, s);
    }

    // ---- Motion ----

    private UIElement CurveGrid()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 280, Gap = 16 };
        foreach (var c in Curves) grid.Children.Add(CurveCard(c));
        return grid;
    }

    // ---- Type: the samples (and in the details, the numbers) ----

    private static UIElement TypeCard(bool detailed)
    {
        var rows = new StackPanel { Spacing = 0 };
        for (int i = 0; i < TypeRamp.Length; i++)
        {
            var (name, size, font, weight, mac, use) = TypeRamp[i];
            var g = new Grid { ColumnSpacing = 16, Padding = new Thickness(0, 10, 0, 10) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(detailed ? 170 : 120) });
            if (detailed) g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(200) });
            var sample = new TextBlock
            {
                Text = detailed ? name : "玻璃之下，内容优先 Aa 0123",
                FontFamily = (FontFamily)Application.Current.Resources[font],
                FontSize = detailed ? 15 : size,
                VerticalAlignment = VerticalAlignment.Center,
                TextTrimming = TextTrimming.CharacterEllipsis,
            };
            var spec = Tabular(Text(detailed ? $"{size} · {weight}" : name, "RowNoteStyle"));
            spec.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(spec, 1);
            g.Children.Add(sample);
            g.Children.Add(spec);
            if (detailed)
            {
                var where = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                where.Children.Add(Text("macOS " + mac, "FootnoteStyle"));
                where.Children.Add(Text(use, "RowNoteStyle"));
                Grid.SetColumn(where, 2);
                g.Children.Add(where);
            }
            if (i > 0) rows.Children.Add(new Rectangle { Height = 1, Fill = Tokens.Brush("SeparatorBrush") });
            rows.Children.Add(g);
        }
        return Card(rows);
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

    private void StopDesignAnimations()
    {
        if (!_designTicking) return;
        CompositionTarget.Rendering -= OnDesignFrame;
        _designTicking = false;
        foreach (var (_, curve, show) in _playing) show(curve.Seconds);
        _playing.Clear();
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
        var why = Text("内圆角随边距缩小，直边间距一致。", "RowNoteStyle");
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
        var grid = new ResponsiveGrid { MinCellWidth = 280, Gap = 16, StretchLastRow = true };
        grid.Children.Add(Panel(Color.FromArgb(255, 0x24, 0x2A, 0x3A), Colors.White, new[] { ("主文字", 0.96), ("次文字", 0.76), ("弱文字", 0.56) },
            Color.FromArgb(255, 0xFF, 0x45, 0x3A), "浅色字 · 暗背景"));
        grid.Children.Add(Panel(Color.FromArgb(255, 0xEC, 0xEE, 0xF3), Color.FromArgb(255, 0x11, 0x13, 0x17), new[] { ("主文字", 0.92), ("次文字", 0.66), ("弱文字", 0.46) },
            Color.FromArgb(255, 0xE0, 0x35, 0x2B), "深色字 · 亮背景"));
        return grid;
    }
}
