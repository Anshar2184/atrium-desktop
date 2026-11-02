using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace Atrium.Settings;

// Design tokens from App.xaml, in the app's current theme.
public static class Tokens
{
    public static Microsoft.UI.Xaml.Media.Brush Brush(string key)
    {
        var res = Application.Current.Resources;
        if (res.TryGetValue(key, out var v) && v is Microsoft.UI.Xaml.Media.Brush b) return b;
        var theme = Application.Current.RequestedTheme == ApplicationTheme.Dark ? "Dark" : "Light";
        if (res.ThemeDictionaries.TryGetValue(theme, out var d) && d is ResourceDictionary rd && rd.TryGetValue(key, out var t)
            && t is Microsoft.UI.Xaml.Media.Brush tb) return tb;
        return new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Transparent);
    }
}

// A sidebar entry: a glyph and a name, in the item's ink (white when chosen).
public sealed class NavLabel : StackPanel
{
    private readonly FontIcon _icon = new() { FontSize = 16, Width = 22 };
    private readonly TextBlock _text = new() { FontSize = 15, VerticalAlignment = VerticalAlignment.Center };

    public NavLabel()
    {
        Orientation = Orientation.Horizontal;
        Spacing = 12;
        Children.Add(_icon);
        Children.Add(_text);
    }

    public string Glyph { get => _icon.Glyph; set => _icon.Glyph = value; }
    public string Text { get => _text.Text; set => _text.Text = value; }
}

// A wrapping grid of equal-width cells: as many columns as fit MinCellWidth (1 when narrow,
// 2 when wide), each row as tall as its tallest card. Unlike a virtualising ItemsRepeater every
// card stays in the visual tree, so UI Automation can always find its controls.
public sealed class ResponsiveGrid : Panel
{
    public static readonly DependencyProperty MinCellWidthProperty =
        DependencyProperty.Register(nameof(MinCellWidth), typeof(double), typeof(ResponsiveGrid), new PropertyMetadata(320.0, (d, _) => ((ResponsiveGrid)d).InvalidateMeasure()));
    public static readonly DependencyProperty GapProperty =
        DependencyProperty.Register(nameof(Gap), typeof(double), typeof(ResponsiveGrid), new PropertyMetadata(16.0, (d, _) => ((ResponsiveGrid)d).InvalidateMeasure()));

    public double MinCellWidth { get => (double)GetValue(MinCellWidthProperty); set => SetValue(MinCellWidthProperty, value); }
    public double Gap { get => (double)GetValue(GapProperty); set => SetValue(GapProperty, value); }

    private int Columns(double width)
    {
        if (double.IsInfinity(width) || width <= 0) return 1;
        return Math.Max(1, (int)Math.Floor((width + Gap) / (MinCellWidth + Gap)));
    }

    protected override Size MeasureOverride(Size available)
    {
        int cols = Columns(available.Width);
        double cell = double.IsInfinity(available.Width) ? MinCellWidth : (available.Width - (cols - 1) * Gap) / cols;
        double total = 0;
        int shown = 0;
        double rowH = 0;
        foreach (var child in Children)
        {
            if (child.Visibility == Visibility.Collapsed) continue;
            child.Measure(new Size(cell, double.PositiveInfinity));
            rowH = Math.Max(rowH, child.DesiredSize.Height);
            if (++shown % cols == 0) { total += rowH + Gap; rowH = 0; }
        }
        if (shown % cols != 0) total += rowH + Gap;
        if (shown > 0) total -= Gap;
        return new Size(double.IsInfinity(available.Width) ? cols * cell + (cols - 1) * Gap : available.Width, total);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        int cols = Columns(finalSize.Width);
        double cell = (finalSize.Width - (cols - 1) * Gap) / cols;
        var visible = Children.Where(c => c.Visibility != Visibility.Collapsed).ToList();
        double y = 0;
        for (int start = 0; start < visible.Count; start += cols)
        {
            int end = Math.Min(start + cols, visible.Count);
            double rowH = 0;
            for (int i = start; i < end; i++) rowH = Math.Max(rowH, visible[i].DesiredSize.Height);
            for (int i = start; i < end; i++)
                visible[i].Arrange(new Rect((i - start) * (cell + Gap), y, cell, rowH));
            y += rowH + Gap;
        }
        return finalSize;
    }
}
