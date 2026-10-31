using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// Tiny vector pictures of what each parameter does (48 x 40 canvas). Update(t) takes the
// slider position normalised to 0..1 and redraws, so the picture moves with the slider.
internal sealed record Illustration(FrameworkElement Root, Action<double> Update);

internal static class Illustrations
{
    private static Style Style(string key) => (Style)Application.Current.Resources[key];

    private static Canvas NewCanvas() => new() { Width = 48, Height = 40 };

    private static T At<T>(T e, double x, double y) where T : UIElement
    {
        Canvas.SetLeft(e, x);
        Canvas.SetTop(e, y);
        return e;
    }

    private static Rectangle Rect(string style, double x, double y, double w, double h, double r = 0, double opacity = 1)
    {
        var e = new Rectangle { Style = Style(style), Width = w, Height = h, RadiusX = r, RadiusY = r, Opacity = opacity };
        return At(e, x, y);
    }

    private static void Place(Rectangle e, double x, double y, double w, double h, double r)
    {
        Canvas.SetLeft(e, x); Canvas.SetTop(e, y);
        e.Width = Math.Max(0, w); e.Height = Math.Max(0, h);
        e.RadiusX = e.RadiusY = Math.Max(0, r);
    }

    private static Path PathOf(string style, string data, double x = 0, double y = 0)
    {
        var p = (Path)XamlReader.Load($"<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Data='{data}'/>");
        p.Style = Style(style);
        return At(p, x, y);
    }

    private static Illustration Wrap(Canvas c, Action<double> update) => new(c, update);

    public static Illustration Transparency()
    {
        var c = NewCanvas();
        c.Children.Add(Rect("GlyphAccentFillStyle", 12, 6, 6, 28, 3, 0.55));
        c.Children.Add(Rect("GlyphAccentFillStyle", 30, 6, 6, 28, 3, 0.55));
        var fill = Rect("GlyphInkFillStyle", 8, 9, 32, 22, 8);
        c.Children.Add(fill);
        c.Children.Add(Rect("GlyphInkStyle", 8, 9, 32, 22, 8));
        return Wrap(c, t => fill.Opacity = 0.04 + 0.5 * (1 - t));
    }

    public static Illustration Blur()
    {
        var c = NewCanvas();
        var outer = Rect("GlyphAccentFillStyle", 0, 0, 10, 10, 20, 0.14);
        var mid = Rect("GlyphAccentFillStyle", 0, 0, 10, 10, 20, 0.26);
        c.Children.Add(outer); c.Children.Add(mid);
        c.Children.Add(Rect("GlyphAccentFillStyle", 19, 15, 10, 10, 5));
        return Wrap(c, t =>
        {
            double a = 16 + t * 28, b = 12 + t * 16;
            Place(outer, 24 - a / 2, 20 - a / 2, a, a, a / 2);
            Place(mid, 24 - b / 2, 20 - b / 2, b, b, b / 2);
        });
    }

    public static Illustration Refraction()
    {
        var c = NewCanvas();
        c.Children.Add(Rect("GlyphFaintStyle", 4, 12, 40, 16, 8));
        var lines = new List<(Polyline Line, double X)>();
        foreach (double x in new[] { 12.0, 24.0, 36.0 })
        {
            var pl = new Polyline { Style = Style("GlyphInkStyle") };
            c.Children.Add(pl);
            lines.Add((pl, x));
        }
        return Wrap(c, t =>
        {
            foreach (var (pl, x) in lines)
            {
                var pts = new PointCollection();
                double dir = Math.Sign(x - 24);
                for (double y = 3; y <= 37; y += 2)
                {
                    double k = (y - 20) / 7;
                    double bump = Math.Exp(-k * k);
                    pts.Add(new Point(x + dir * t * 7 * bump, y));
                }
                pl.Points = pts;
            }
        });
    }

    public static Illustration Edge()
    {
        var c = NewCanvas();
        c.Children.Add(Rect("GlyphInkStyle", 4, 5, 40, 30, 13));
        var inner = Rect("GlyphAccentStyle", 8, 9, 32, 22, 9);
        inner.StrokeDashArray = new DoubleCollection { 2, 2 };
        c.Children.Add(inner);
        return Wrap(c, t =>
        {
            double d = 3 + t * 9;
            Place(inner, 4 + d, 5 + d, 40 - 2 * d, 30 - 2 * d, Math.Max(2, 13 - d));
        });
    }

    public static Illustration Dispersion()
    {
        var c = NewCanvas();
        const string arc = "M 6 4 Q 0 20 6 36";
        var strokes = new List<Path>();
        foreach (var col in new[] { Color.FromArgb(230, 255, 82, 82), Color.FromArgb(230, 70, 214, 120), Color.FromArgb(230, 70, 130, 255) })
        {
            var p = PathOf("GlyphInkStyle", arc);
            p.Stroke = new SolidColorBrush(col);
            p.StrokeThickness = 2;
            strokes.Add(p);
            c.Children.Add(p);
        }
        var rim = Rect("GlyphFaintStyle", 30, 6, 12, 28, 6);
        c.Children.Add(rim);
        return Wrap(c, t =>
        {
            double o = t * 14; // 0 .. 14 px between the colour channels
            Canvas.SetLeft(strokes[0], 21 - o * 0.5);
            Canvas.SetLeft(strokes[1], 21 + o * 0.15);
            Canvas.SetLeft(strokes[2], 21 + o * 0.6);
        });
    }

    public static Illustration Highlight()
    {
        var c = NewCanvas();
        c.Children.Add(Rect("GlyphFaintStyle", 6, 7, 36, 26, 13));
        var arc = PathOf("GlyphAccentStyle", "M 7 20 A 13 13 0 0 1 20 7");
        var arc2 = PathOf("GlyphAccentStyle", "M 41 20 A 13 13 0 0 1 28 33");
        arc2.Opacity = 0.35;
        c.Children.Add(arc); c.Children.Add(arc2);
        return Wrap(c, t =>
        {
            arc.StrokeThickness = 1.25 + t * 3;
            arc.Opacity = 0.3 + 0.7 * t;
            arc2.StrokeThickness = 1 + t * 1.5;
            arc2.Opacity = 0.1 + 0.3 * t;
        });
    }

    public static Illustration Shadow()
    {
        var c = NewCanvas();
        var shade = Rect("GlyphInkFillStyle", 11, 13, 26, 17, 6, 0.2);
        c.Children.Add(shade);
        c.Children.Add(Rect("GlyphInkStyle", 10, 7, 28, 18, 7));
        return Wrap(c, t =>
        {
            shade.Opacity = 0.05 + 0.4 * t;
            Place(shade, 11, 10 + t * 5, 26, 17 + t * 2, 7);
        });
    }

    public static Illustration Corner()
    {
        var c = NewCanvas();
        var r = Rect("GlyphInkStyle", 6, 8, 36, 24, 0);
        c.Children.Add(r);
        return Wrap(c, t => r.RadiusX = r.RadiusY = 12 * Math.Min(1, t * 3));
    }

    public static Illustration Width()
    {
        var c = NewCanvas();
        var bar = Rect("GlyphInkStyle", 0, 9, 20, 14, 7);
        var dim = new Line { Style = Style("GlyphAccentStyle"), Y1 = 31, Y2 = 31 };
        var tickL = new Line { Style = Style("GlyphAccentStyle"), Y1 = 28, Y2 = 34 };
        var tickR = new Line { Style = Style("GlyphAccentStyle"), Y1 = 28, Y2 = 34 };
        foreach (var e in new UIElement[] { bar, dim, tickL, tickR }) c.Children.Add(e);
        return Wrap(c, t =>
        {
            double w = 14 + t * 30, x0 = 24 - w / 2, x1 = 24 + w / 2;
            Place(bar, x0, 9, w, 14, 7);
            dim.X1 = x0; dim.X2 = x1;
            tickL.X1 = tickL.X2 = x0;
            tickR.X1 = tickR.X2 = x1;
        });
    }

    public static Illustration AutoWidth()
    {
        var c = NewCanvas();
        c.Children.Add(Rect("GlyphFaintStyle", 3, 10, 42, 20, 10));
        var bar = Rect("GlyphAccentStyle", 10, 10, 28, 20, 10);
        c.Children.Add(bar);
        for (int i = 0; i < 3; i++) c.Children.Add(Rect("GlyphInkFillStyle", 16 + i * 8, 16, 5, 8, 2, 0.6));
        return Wrap(c, on => Place(bar, on > 0.5 ? 10 : 3, 10, on > 0.5 ? 28 : 42, 20, 10));
    }

    public static Illustration Height()
    {
        var c = NewCanvas();
        var bar = Rect("GlyphInkStyle", 6, 10, 30, 20, 9);
        var dim = new Line { Style = Style("GlyphAccentStyle"), X1 = 42, X2 = 42 };
        var tickT = new Line { Style = Style("GlyphAccentStyle"), X1 = 39, X2 = 45 };
        var tickB = new Line { Style = Style("GlyphAccentStyle"), X1 = 39, X2 = 45 };
        foreach (var e in new UIElement[] { bar, dim, tickT, tickB }) c.Children.Add(e);
        return Wrap(c, t =>
        {
            double h = 8 + t * 28, y0 = 20 - h / 2;
            Place(bar, 6, y0, 30, h, Math.Min(9, h / 2));
            dim.Y1 = y0; dim.Y2 = y0 + h;
            tickT.Y1 = tickT.Y2 = y0;
            tickB.Y1 = tickB.Y2 = y0 + h;
        });
    }

    public static Illustration Margin()
    {
        var c = NewCanvas();
        var floor = new Line { Style = Style("GlyphFaintStyle"), X1 = 4, X2 = 44, Y1 = 35, Y2 = 35 };
        var bar = Rect("GlyphInkStyle", 8, 12, 28, 9, 4.5);
        var gap = new Line { Style = Style("GlyphAccentStyle"), X1 = 41, X2 = 41 };
        c.Children.Add(floor); c.Children.Add(bar); c.Children.Add(gap);
        return Wrap(c, t =>
        {
            double g = 2 + t * 18, bottom = 35 - g;
            Place(bar, 8, bottom - 9, 28, 9, 4.5);
            gap.Y1 = bottom; gap.Y2 = 35;
        });
    }

    public static Illustration IconSize()
    {
        var c = NewCanvas();
        var sq = Rect("GlyphAccentStyle", 14, 10, 20, 20, 6);
        var inner = Rect("GlyphInkFillStyle", 20, 16, 8, 8, 3, 0.25);
        c.Children.Add(sq); c.Children.Add(inner);
        return Wrap(c, t =>
        {
            double s = 8 + t * 28;
            Place(sq, 24 - s / 2, 20 - s / 2, s, s, Math.Max(2, s * 0.3));
            Place(inner, 24 - s * 0.2, 20 - s * 0.2, s * 0.4, s * 0.4, Math.Max(1, s * 0.12));
        });
    }

    // The droplet of the tint card, filled with the chosen colour.
    public static (FrameworkElement Root, Action<Color> SetColor) Tint()
    {
        var c = NewCanvas();
        var drop = PathOf("GlyphInkStyle", "M 24 6 C 24 6 13 18 13 26 A 11 11 0 0 0 35 26 C 35 18 24 6 24 6 Z");
        drop.Fill = new SolidColorBrush(Colors.White);
        c.Children.Add(drop);
        return (c, col => drop.Fill = new SolidColorBrush(col));
    }

    public static Illustration ClockToggle()
    {
        var c = NewCanvas();
        c.Children.Add(Rect("GlyphInkStyle", 3, 10, 42, 20, 10));
        for (int i = 0; i < 2; i++) c.Children.Add(Rect("GlyphInkFillStyle", 10 + i * 8, 16, 5, 8, 2, 0.5));
        var time = Rect("GlyphAccentFillStyle", 28, 15, 11, 3, 1.5);
        var date = Rect("GlyphAccentFillStyle", 28, 21, 11, 3, 1.5, 0.6);
        c.Children.Add(time); c.Children.Add(date);
        return Wrap(c, on =>
        {
            time.Opacity = on > 0.5 ? 1 : 0.1;
            date.Opacity = on > 0.5 ? 0.6 : 0.06;
        });
    }

    public static Illustration RunningToggle()
    {
        var c = NewCanvas();
        c.Children.Add(Rect("GlyphInkStyle", 3, 10, 42, 20, 10));
        var shown = new List<UIElement>();
        for (int i = 0; i < 4; i++)
        {
            var sq = Rect(i < 2 ? "GlyphInkFillStyle" : "GlyphAccentFillStyle", 9 + i * 8.5, 15, 6, 6, 2, i < 2 ? 0.5 : 1);
            c.Children.Add(sq);
            if (i >= 2)
            {
                var dot = Rect("GlyphAccentFillStyle", 9 + i * 8.5 + 2, 24, 2, 2, 1);
                c.Children.Add(dot);
                shown.Add(sq); shown.Add(dot);
            }
        }
        return Wrap(c, on => { foreach (var e in shown) e.Opacity = on > 0.5 ? 1 : 0.1; });
    }
}
