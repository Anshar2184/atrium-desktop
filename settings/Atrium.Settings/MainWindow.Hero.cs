using System.Diagnostics;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The hero specimen: the glass bar rendered on the CPU (GlassPreview.cs) over the specimen photo
// (Photo.cs), with mock icons and a clock laid out by the same rules as the real bar, a loupe on
// the right end of the bar, and dimension marks for the parameter that is being edited. The photo
// is uploaded once per size; a frame repaints only the glass, as a transparent layer over it.
public sealed partial class MainWindow
{
    private const double LoupeW = 232, LoupeH = 148, LoupeZoom = 3;
    // The bar's width the preview is scaled for: a default bar (seven apps and the clock) at full size
    // once the preview is this wide (DIP), smaller below it.
    private const double HeroReferenceW = 860;

    // Everything in hero DIP (real DIP * S).
    private sealed record HeroLayout(
        double S, double BarX, double BarY, double BarW, double BarH, double Radius, double MaxW,
        int N, double Icon, double P, double PlateR, double[] PlateX, double PlateY,
        bool Clock, double ClockX, double ClockW, double TimePx, double DatePx, double SepX,
        double LoupeX0, double LoupeY0);

    private sealed record RenderJob(BarSettings S, HeroLayout L, double W, double H, double Rs, bool Loupe);

    private static readonly (string Glyph, uint C1, uint C2, bool Running)[] MockApps =
    {
        ("", 0xF8BC4A, 0xE8862B, false), ("", 0x45C4FB, 0x2F6BEB, true), ("", 0xFB7F96, 0xE0315A, false),
        ("", 0xB69BFF, 0x7A45EA, true), ("", 0x56627A, 0x1B2233, false), ("", 0x3FDBA5, 0x0A9B6C, true),
        ("", 0xA9B6C9, 0x56657C, true),
    };

    private double _heroW, _heroH;
    private readonly ImageBrush _heroBrush = new() { Stretch = Stretch.Fill };
    private readonly ImageBrush _loupeBrush = new() { Stretch = Stretch.Fill };
    private readonly Image _glass = new() { Stretch = Stretch.Fill, IsHitTestVisible = false };
    private WriteableBitmap? _photoBmp, _glassBmp, _loupeBmp;
    private Backdrop? _photoPending; // a new backdrop for the hero's background (Interlocked)
    private volatile FrameBuffers? _heroFrames, _loupeFrames;
    private LatestWorker<RenderJob>? _renderWorker;
    private LatestWorker<BarSettings>? _swatchWorker;
    private int _uiPending;
    private float _luma = 0.5f;
    private string? _engaged;
    private bool _heroSized;

    // Render-thread caches.
    private Backdrop? _mainBg, _loupeBg;
    private string _mainKey = "", _loupeKey = "";

    private readonly List<Border> _mockIcons = new();
    private readonly List<FontIcon> _mockGlyphs = new();
    private readonly List<Ellipse> _mockDots = new();
    private Border _plate = null!;
    private Rectangle _sep = null!;
    private TextBlock _time = null!, _date = null!, _lpTime = null!, _lpDate = null!;
    private readonly List<UIElement> _annotations = new();

    private void InitHero()
    {
        HeroFrame.Background = _heroBrush;
        Loupe.Background = _loupeBrush;
        _renderWorker = new LatestWorker<RenderJob>(RenderFrame);
        _swatchWorker = new LatestWorker<BarSettings>(job => RenderSwatch(job, SwatchCustom));

        HeroCanvas.Children.Add(_glass);
        _plate = new Border { Background = new SolidColorBrush(Color.FromArgb(46, 255, 255, 255)), BorderBrush = new SolidColorBrush(Color.FromArgb(36, 255, 255, 255)), BorderThickness = new Thickness(1) };
        HeroCanvas.Children.Add(_plate);
        for (int i = 0; i < MockApps.Length; i++)
        {
            var app = MockApps[i];
            var fill = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 1) };
            fill.GradientStops.Add(new GradientStop { Color = RgbColor(app.C1), Offset = 0 });
            fill.GradientStops.Add(new GradientStop { Color = RgbColor(app.C2), Offset = 1 });
            var glyph = new FontIcon { Glyph = app.Glyph, Foreground = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255)) };
            var icon = new Border { Background = fill, Child = glyph, BorderBrush = new SolidColorBrush(Color.FromArgb(40, 255, 255, 255)), BorderThickness = new Thickness(0.75) };
            _mockIcons.Add(icon);
            _mockGlyphs.Add(glyph);
            HeroCanvas.Children.Add(icon);
        }
        for (int i = 0; i < MockApps.Length; i++)
        {
            var dot = new Ellipse { Fill = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255)) };
            _mockDots.Add(dot);
            HeroCanvas.Children.Add(dot);
        }
        _sep = new Rectangle { Width = 1, Fill = new SolidColorBrush(Color.FromArgb(70, 255, 255, 255)) };
        HeroCanvas.Children.Add(_sep);
        _time = ClockText("10:24", true);
        _date = ClockText("2026/10/31", false);
        _lpTime = ClockText("10:24", true);
        _lpDate = ClockText("2026/10/31", false);
        HeroCanvas.Children.Add(_time);
        HeroCanvas.Children.Add(_date);
        LoupeCanvas.Children.Add(_lpTime);
        LoupeCanvas.Children.Add(_lpDate);
        LoupeCanvas.Clip = new RectangleGeometry { Rect = new Rect(0, 0, LoupeW, LoupeH) };

        // The two presets are fixed swatches; the custom one follows the current material.
        var liquid = BarSettings.Liquid();
        var frosted = BarSettings.Frosted();
        Task.Run(() => { RenderSwatch(liquid, SwatchLiquid); RenderSwatch(frosted, SwatchFrosted); });
    }

    // The bar's clock face (D40): Semibold for the time, Medium for the date.
    private static TextBlock ClockText(string text, bool strong) =>
        new() { Text = text, TextAlignment = TextAlignment.Center, IsHitTestVisible = false, IsTextScaleFactorEnabled = false,
                FontFamily = (FontFamily)Application.Current.Resources[strong ? "TitleFont" : "TextFont"] };

    private static Color RgbColor(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    private double RasterScale => Math.Clamp(HeroFrame.XamlRoot?.RasterizationScale ?? 1.0, 1.0, 2.0);

    private void OnHeroSizeChanged(object sender, SizeChangedEventArgs e)
    {
        _heroW = e.NewSize.Width;
        _heroH = e.NewSize.Height;
        HeroCanvas.Clip = new RectangleGeometry { Rect = new Rect(0, 0, _heroW, _heroH) };
        if (!_heroSized && HeroFrame.XamlRoot != null)
        {
            _heroSized = true;
            HeroFrame.XamlRoot.Changed += (_, _) => UpdateHero();
        }
        UpdateHero();
    }

    // A card was focused or let go: remember which one so the hero can point at it.
    private void OnEngage(string title, bool on)
    {
        _engaged = on ? title : (_engaged == title ? null : _engaged);
        bool edge = _engaged is "折射强度" or "折射边缘宽度" or "色散" or "高光" or "圆角" or "阴影";
        Loupe.BorderBrush = edge ? Tokens.Brush("AccentBrush") : new SolidColorBrush(Color.FromArgb(179, 255, 255, 255));
        UpdateHero();
    }

    // ---- Layout: the same rules as the real bar, scaled to the specimen ----

    private HeroLayout? ComputeLayout(BarSettings s)
    {
        if (_heroW < 80 || _heroH < 80) return null;
        double S = Math.Min(1.0, (_heroW - 48) / (HeroReferenceW + 2 * GlassSpec.InsetDip));
        double H = s.Height, I = Math.Min(s.IconSize, H - 10);
        double P = Math.Max(8, Math.Min(H - 12, Math.Round(s.IconSize * 1.55)));
        double v = (H - P) / 2, pad = v + 4, gap = Math.Max(4, Math.Round(s.IconSize * 0.18));
        double timePx = H * 0.25, datePx = H * 0.19, sepGap = 10;
        double clockW = s.ShowClock ? Math.Max(timePx * 2.75, datePx * 5.7) + 20 : 0;
        double Content(int n) => Math.Max(2 * H, 2 * pad + n * P + Math.Max(0, n - 1) * gap + (clockW > 0 ? (n > 0 ? 2 * sepGap : 0) + clockW : 0));
        double maxW = Math.Max(2 * H, Math.Min(s.Width, (_heroW - 16) / S));
        int n = s.ShowRunning ? 7 : 6;
        while (n > 0 && Content(n) > maxW) n--;
        double barW = s.AutoWidth ? Math.Min(Content(n), maxW) : maxW;

        double clockRight = barW - pad, clockLeft = clockRight - clockW;
        double appsSpace = clockW > 0 ? clockLeft - (n > 0 ? 2 * sepGap : 0) - pad : barW - 2 * pad;
        double appsLeft = pad + Math.Max(0, (appsSpace - (n * P + Math.Max(0, n - 1) * gap)) / 2);
        double reff = Math.Min(s.CornerRadius, Math.Min(H, barW) / 2);

        double bx = (_heroW - barW * S) / 2;
        double by = Math.Max(10, _heroH - s.Margin * S - H * S);
        var plateX = new double[n];
        for (int i = 0; i < n; i++) plateX[i] = bx + (appsLeft + i * (P + gap)) * S;

        double lw = LoupeW / LoupeZoom, lh = LoupeH / LoupeZoom;
        return new HeroLayout(S, bx, by, barW * S, H * S, reff * S, maxW * S,
            n, I * S, P * S, Math.Max(4, Math.Min(P / 2, reff - v)) * S, plateX, by + v * S,
            clockW > 0, bx + clockLeft * S, clockW * S, timePx * S, datePx * S, bx + (clockLeft - sepGap) * S,
            bx + barW * S - 0.72 * lw, by + H * S / 2 - lh / 2);
    }

    private void UpdateHero()
    {
        UpdateReadout();
        if (HeroBlock.Visibility != Visibility.Visible || _renderWorker == null) return;
        var L = ComputeLayout(_current);
        if (L == null) return;
        PositionMock(L);
        DrawAnnotations(L);
        _renderWorker.Post(new RenderJob(_current.Clone(), L, _heroW, _heroH, RasterScale, Loupe.Visibility == Visibility.Visible));
    }

    private Brush InkBrush()
    {
        bool dark = _current.TextColor == "dark" || (_current.TextColor == "auto" && _luma > 0.6f);
        return new SolidColorBrush(dark ? Color.FromArgb(235, 22, 24, 29) : Color.FromArgb(245, 255, 255, 255));
    }

    private void PositionMock(HeroLayout L)
    {
        for (int i = 0; i < MockApps.Length; i++)
        {
            bool shown = i < L.N;
            var icon = _mockIcons[i];
            icon.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
            _mockDots[i].Visibility = shown && MockApps[i].Running ? Visibility.Visible : Visibility.Collapsed;
            if (!shown) continue;
            double cx = L.PlateX[i] + L.P / 2, cy = L.PlateY + L.P / 2;
            // As if the pointer rested on the second app: with magnification on,
            // it and its neighbours grow from their bottom edge like a wave.
            double dist = Math.Abs(i - 1) / 2.3, wave = dist < 1 ? (1 + Math.Cos(Math.PI * dist)) / 2 : 0;
            double size = L.Icon * (1 + (_current.Magnify - 1) * wave);
            icon.Width = icon.Height = size;
            icon.CornerRadius = new CornerRadius(size * 0.26);
            Canvas.SetLeft(icon, cx - size / 2);
            Canvas.SetTop(icon, cy + L.Icon / 2 - size);
            _mockGlyphs[i].FontSize = Math.Max(6, size * 0.5);
            double d = Math.Max(2.5, L.S * 5);
            _mockDots[i].Width = _mockDots[i].Height = d;
            Canvas.SetLeft(_mockDots[i], cx - d / 2);
            Canvas.SetTop(_mockDots[i], L.PlateY + L.P - d * 1.9);
        }
        // Without magnification the hovered app shows its plate instead.
        bool plate = L.N > 1 && _current.Magnify <= 1.001;
        _plate.Visibility = plate ? Visibility.Visible : Visibility.Collapsed;
        if (plate)
        {
            _plate.Width = _plate.Height = L.P;
            _plate.CornerRadius = new CornerRadius(L.PlateR);
            Canvas.SetLeft(_plate, L.PlateX[1]);
            Canvas.SetTop(_plate, L.PlateY);
        }

        _sep.Visibility = L.Clock && L.N > 0 ? Visibility.Visible : Visibility.Collapsed;
        _sep.Height = L.BarH * 0.5;
        Canvas.SetLeft(_sep, L.SepX);
        Canvas.SetTop(_sep, L.BarY + L.BarH * 0.25);

        var ink = InkBrush();
        PlaceClock(_time, _date, L, 1, L.ClockX, L.BarY, ink);
        // The loupe shows only the glass rim; enlarged clock text would cover it.
        _lpTime.Visibility = _lpDate.Visibility = Visibility.Collapsed;
    }

    private void PlaceClock(TextBlock time, TextBlock date, HeroLayout L, double zoom, double x, double barTop, Brush ink)
    {
        time.Visibility = date.Visibility = L.Clock ? Visibility.Visible : Visibility.Collapsed;
        if (!L.Clock) return;
        double tp = Math.Max(6, L.TimePx * zoom), dp = Math.Max(5, L.DatePx * zoom);
        double w = L.ClockW * zoom, total = (tp + dp) * 1.3;
        double top = barTop + (L.BarH * zoom - total) / 2;
        time.FontSize = tp; date.FontSize = dp;
        time.Width = date.Width = w;
        time.Foreground = ink;
        date.Foreground = ink;
        date.Opacity = 0.78;
        Canvas.SetLeft(time, x); Canvas.SetTop(time, top);
        Canvas.SetLeft(date, x); Canvas.SetTop(date, top + tp * 1.3);
    }

    // ---- Dimension marks for the card in use ----

    private void DrawAnnotations(HeroLayout L)
    {
        foreach (var a in _annotations) HeroCanvas.Children.Remove(a);
        _annotations.Clear();
        if (_engaged == null) return;
        var white = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255));
        void Line(double x1, double y1, double x2, double y2, bool dashed = false)
        {
            var l = new Line { X1 = x1, Y1 = y1, X2 = x2, Y2 = y2, Stroke = white, StrokeThickness = 1.5, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round, IsHitTestVisible = false };
            if (dashed) l.StrokeDashArray = new DoubleCollection { 3, 3 };
            _annotations.Add(l);
            HeroCanvas.Children.Add(l);
        }
        void Chip(string text, double cx, double top)
        {
            var chip = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255)), CornerRadius = new CornerRadius(6), Padding = new Thickness(7, 2, 7, 3),
                Child = new TextBlock { Text = text, FontSize = 11, FontFamily = (FontFamily)Application.Current.Resources["EmphasisFont"], Foreground = new SolidColorBrush(Color.FromArgb(255, 22, 24, 29)) },
                IsHitTestVisible = false,
            };
            Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment((TextBlock)chip.Child, FontNumeralAlignment.Tabular);
            chip.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            Canvas.SetLeft(chip, Math.Clamp(cx - chip.DesiredSize.Width / 2, 4, Math.Max(4, _heroW - chip.DesiredSize.Width - 4)));
            Canvas.SetTop(chip, Math.Clamp(top, 4, Math.Max(4, _heroH - chip.DesiredSize.Height - 4)));
            _annotations.Add(chip);
            HeroCanvas.Children.Add(chip);
        }

        double barBottom = L.BarY + L.BarH, barRight = L.BarX + L.BarW;
        switch (_engaged)
        {
            case "高度":
            {
                double x = L.BarX - 14;
                Line(x, L.BarY, x, barBottom); Line(x - 4, L.BarY, x + 4, L.BarY); Line(x - 4, barBottom, x + 4, barBottom);
                Chip($"{Math.Round(_current.Height)} px", x - 30, L.BarY + L.BarH / 2 - 10);
                break;
            }
            case "底部间距":
            {
                double x = L.BarX + L.BarW * 0.5;
                Line(x, barBottom, x, _heroH); Line(x - 4, barBottom, x + 4, barBottom); Line(x - 4, _heroH - 1, x + 4, _heroH - 1);
                Chip($"{Math.Round(_current.Margin)} px", x + 34, (barBottom + _heroH) / 2 - 10);
                break;
            }
            case "最大宽度":
            {
                double y = L.BarY - 12, x0 = (_heroW - L.MaxW) / 2, x1 = x0 + L.MaxW;
                Line(x0, y, x1, y, dashed: true); Line(x0, y - 4, x0, y + 4); Line(x1, y - 4, x1, y + 4);
                Chip($"{Math.Round(_current.Width)} px", _heroW / 2, y - 24);
                break;
            }
            case "圆角":
            {
                // The circle the corner is an arc of, and its radius.
                double r = Math.Max(L.Radius, 2);
                var circle = new Ellipse { Width = 2 * r, Height = 2 * r, Stroke = white, StrokeThickness = 1.25, StrokeDashArray = new DoubleCollection { 3, 3 }, IsHitTestVisible = false };
                Canvas.SetLeft(circle, L.BarX);
                Canvas.SetTop(circle, L.BarY);
                _annotations.Add(circle);
                HeroCanvas.Children.Add(circle);
                Line(L.BarX + r, L.BarY + r, L.BarX + r - r * 0.7071, L.BarY + r - r * 0.7071);
                Chip($"R {Math.Round(Math.Min(_current.CornerRadius, _current.Height / 2))} px", L.BarX + r, L.BarY - 30);
                break;
            }
            case "图标大小":
            {
                if (L.N == 0) break;
                double x = L.PlateX[0] + L.P / 2 - L.Icon / 2 - 4, y0 = L.PlateY + L.P / 2 - L.Icon / 2, y1 = y0 + L.Icon;
                Line(x, y0, x, y1); Line(x - 3, y0, x + 3, y0); Line(x - 3, y1, x + 3, y1);
                Chip($"{Math.Round(_current.IconSize)} px", L.PlateX[0] + L.P / 2, L.BarY - 28);
                break;
            }
        }
    }

    // ---- Rendering (thread pool) ----

    private void RenderFrame(RenderJob j)
    {
        var sw = Stopwatch.StartNew();
        int pw = (int)Math.Round(j.W * j.Rs), ph = (int)Math.Round(j.H * j.Rs);
        if (pw < 8 || ph < 8) return;
        var L = j.L;

        // Main view: the photo once per size, then the glass alone, over it.
        string key = $"{pw}x{ph}/{j.W:F1}x{j.H:F1}/{Photo.Generation}";
        if (_mainBg == null || key != _mainKey)
        {
            _mainBg = Backdrop.Create(pw, ph, j.W, j.H, 0, 0, j.Rs);
            _mainKey = key;
            Interlocked.Exchange(ref _photoPending, _mainBg);
        }
        var spec = GlassSpec.For(j.S, L.S * j.Rs);
        int inset = (int)spec.Inset;
        int bx = (int)Math.Round(L.BarX * j.Rs), by = (int)Math.Round(L.BarY * j.Rs);
        int bw = (int)Math.Round(L.BarW * j.Rs), bh = (int)Math.Round(L.BarH * j.Rs);
        int gw = bw + 2 * inset, gh = bh + 2 * inset;
        var frames = _heroFrames;
        if (frames == null || frames.Width != gw || frames.Height != gh)
            _heroFrames = frames = new FrameBuffers(gw, gh);
        Array.Clear(frames.Back);
        GlassRenderer.DrawOverlay(_mainBg, frames.Back, bx - inset, by - inset, gw, gh, spec);
        float luma = GlassRenderer.MeanLuma(frames.Back, gw, inset + Math.Clamp(bh / 2, 0, bw - 1), inset + bh / 4,
            inset + Math.Clamp(bw - bh / 2, 1, bw), inset + Math.Max(bh * 3 / 4, bh / 4 + 1));
        frames.Publish(luma, (bx - inset) / j.Rs, (by - inset) / j.Rs, j.Rs);

        // Only the material page with the loupe enabled needs its second render.
        int lw = j.Loupe ? (int)Math.Round(LoupeW * j.Rs) : 0, lh = j.Loupe ? (int)Math.Round(LoupeH * j.Rs) : 0;
        if (j.Loupe)
        {
            double ppd = LoupeZoom * j.Rs;
            string lkey = $"{lw}x{lh}/{j.W:F1}x{j.H:F1}/{L.LoupeX0:F2},{L.LoupeY0:F2}/{Photo.Generation}";
            if (_loupeBg == null || lkey != _loupeKey)
            {
                _loupeBg = Backdrop.Create(lw, lh, j.W, j.H, L.LoupeX0, L.LoupeY0, ppd);
                _loupeKey = lkey;
            }
            var lframes = _loupeFrames;
            if (lframes == null || lframes.Width != lw || lframes.Height != lh)
                _loupeFrames = lframes = new FrameBuffers(lw, lh);
            Buffer.BlockCopy(_loupeBg.Base, 0, lframes.Back, 0, _loupeBg.Base.Length);
            var lspec = GlassSpec.For(j.S, L.S * ppd);
            int linset = (int)lspec.Inset;
            int lbx = (int)Math.Round((L.BarX - L.LoupeX0) * ppd), lby = (int)Math.Round((L.BarY - L.LoupeY0) * ppd);
            int lbw = (int)Math.Round(L.BarW * ppd), lbh = (int)Math.Round(L.BarH * ppd);
            GlassRenderer.Draw(_loupeBg, lframes.Back, lbx - linset, lby - linset, lbw + 2 * linset, lbh + 2 * linset, lspec);
            lframes.Publish(luma);
        }
        else
        {
            _loupeBg = null;
            _loupeFrames = null;
            _loupeKey = "";
        }

        sw.Stop();
        LogRender(sw.Elapsed.TotalMilliseconds, pw, ph, lw, lh);
        if (Interlocked.Exchange(ref _uiPending, 1) == 0)
            DispatcherQueue.TryEnqueue(ApplyFrames);
    }

    // Development aid: ATRIUM_SETTINGS_PREVIEW_LOG=<file> appends the time of every frame.
    private static readonly string? PreviewLog = Environment.GetEnvironmentVariable("ATRIUM_SETTINGS_PREVIEW_LOG");

    private static void LogRender(double ms, int pw, int ph, int lw, int lh)
    {
        if (PreviewLog == null) return;
        try { File.AppendAllText(PreviewLog, $"{ms:F1} ms  hero {pw}x{ph}  loupe {lw}x{lh}\n"); } catch (IOException) { }
    }

    // ---- UI thread: frames to bitmaps ----

    private void ApplyFrames()
    {
        Interlocked.Exchange(ref _uiPending, 0);
        if (Interlocked.Exchange(ref _photoPending, null) is { } bg)
        {
            _photoBmp = new WriteableBitmap(bg.Width, bg.Height);
            using (var stream = _photoBmp.PixelBuffer.AsStream()) stream.Write(bg.Base, 0, bg.Base.Length);
            _photoBmp.Invalidate();
            _heroBrush.ImageSource = _photoBmp;
        }
        if (_heroFrames is { } hf)
        {
            if (_glassBmp == null || _glassBmp.PixelWidth != hf.Width || _glassBmp.PixelHeight != hf.Height)
            {
                _glassBmp = new WriteableBitmap(hf.Width, hf.Height);
                _glass.Source = _glassBmp;
            }
            FrameBuffers.Placement at;
            using (var stream = _glassBmp.PixelBuffer.AsStream()) at = hf.WriteFront(stream);
            _glassBmp.Invalidate();
            _glass.Width = hf.Width / at.Scale;
            _glass.Height = hf.Height / at.Scale;
            Canvas.SetLeft(_glass, at.X);
            Canvas.SetTop(_glass, at.Y);
            if (Math.Abs(at.Luma - _luma) > 0.001f)
            {
                bool before = _luma > 0.6f;
                _luma = at.Luma;
                if (_current.TextColor == "auto" && before != _luma > 0.6f) UpdateHero();
            }
        }
        if (Loupe.Visibility == Visibility.Visible && _loupeFrames is { } lf) Upload(lf, ref _loupeBmp, _loupeBrush);
    }

    private static void Upload(FrameBuffers f, ref WriteableBitmap? bmp, ImageBrush brush)
    {
        if (bmp == null || bmp.PixelWidth != f.Width || bmp.PixelHeight != f.Height)
        {
            bmp = new WriteableBitmap(f.Width, f.Height);
            brush.ImageSource = bmp;
        }
        using (var stream = bmp.PixelBuffer.AsStream()) f.WriteFront(stream);
        bmp.Invalidate();
    }

    // ---- Preset swatches: the same renderer on a small canvas ----

    private const double SwatchW = 240, SwatchH = 72;
    private static Backdrop? s_swatchBg;
    private static readonly object s_swatchLock = new();

    private BarSettings? _swatchSettings;

    private void UpdateCustomSwatch()
    {
        if (_page != 0 || (_swatchSettings?.SameMaterial(_current) ?? false)) return;
        _swatchSettings = _current.Clone();
        _swatchWorker?.Post(_swatchSettings);
    }

    // Swatches are tiny; rendering them at 2x keeps them crisp up to 200 % scale.
    private void RenderSwatch(BarSettings material, Border target)
    {
        const double rs = 2;
        int w = (int)Math.Round(SwatchW * rs), h = (int)Math.Round(SwatchH * rs);
        Backdrop bg;
        lock (s_swatchLock)
            bg = s_swatchBg ??= Backdrop.Create(w, h, SwatchW, SwatchH, 0, 0, rs);
        // A 60 DIP capsule at 60 %, centred.
        var s = material.Clone();
        s.Height = 60;
        s.CornerRadius = 30;
        var spec = GlassSpec.For(s, 0.6 * rs);
        int inset = (int)spec.Inset;
        int bw = (int)Math.Round(170 * rs), bh = (int)Math.Round(36 * rs);
        int bx = (w - bw) / 2, by = (h - bh) / 2;
        var pixels = new byte[bg.Base.Length];
        Buffer.BlockCopy(bg.Base, 0, pixels, 0, pixels.Length);
        GlassRenderer.Draw(bg, pixels, bx - inset, by - inset, bw + 2 * inset, bh + 2 * inset, spec);
        DispatcherQueue.TryEnqueue(() =>
        {
            var bmp = new WriteableBitmap(w, h);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(pixels, 0, pixels.Length);
            bmp.Invalidate();
            target.Background = new ImageBrush { ImageSource = bmp, Stretch = Stretch.UniformToFill };
        });
    }
}
