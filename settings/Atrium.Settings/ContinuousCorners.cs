using System.Numerics;
using System.Runtime.CompilerServices;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;
using Windows.UI;
using Microsoft.Graphics.Canvas.Geometry;

namespace Atrium.Settings;

// Visual states require normal dependency properties: WinUI cannot animate
// custom attached properties. Keep these logical paints separate from the
// renderer brush so leaving a state restores the original binding correctly.
public sealed class ContinuousSurface : Grid
{
    public static readonly DependencyProperty PaintProperty = DependencyProperty.Register(
        nameof(Paint), typeof(Brush), typeof(ContinuousSurface), new PropertyMetadata(null,
            (owner, args) => ContinuousCorners.SetPaint(owner, (Brush?)args.NewValue)));
    public Brush? Paint { get => (Brush?)GetValue(PaintProperty); set => SetValue(PaintProperty, value); }
    public static readonly DependencyProperty EdgeProperty = DependencyProperty.Register(
        nameof(Edge), typeof(Brush), typeof(ContinuousSurface), new PropertyMetadata(null,
            (owner, args) => ContinuousCorners.SetEdge(owner, (Brush?)args.NewValue)));
    public Brush? Edge { get => (Brush?)GetValue(EdgeProperty); set => SetValue(EdgeProperty, value); }
    public ContinuousSurface() => ContinuousCorners.SetEnabled(this, true);
}

// Applies the analytic boundary as an alpha mask. Native CornerRadius / cubic
// clips are removed; layout, content, focus, input and motion stay with WinUI.
public sealed class ContinuousCorners : DependencyObject
{
    public static readonly DependencyProperty EnabledProperty = DependencyProperty.RegisterAttached(
        "Enabled", typeof(bool), typeof(ContinuousCorners), new PropertyMetadata(false, EnabledChanged));
    public static bool GetEnabled(DependencyObject value) => (bool)value.GetValue(EnabledProperty);
    public static void SetEnabled(DependencyObject value, bool enabled) => value.SetValue(EnabledProperty, enabled);
    public static readonly DependencyProperty PaintProperty = DependencyProperty.RegisterAttached(
        "Paint", typeof(Brush), typeof(ContinuousCorners), new PropertyMetadata(null, PaintChanged));
    public static Brush? GetPaint(DependencyObject value) => (Brush?)value.GetValue(PaintProperty);
    public static void SetPaint(DependencyObject value, Brush? brush) => value.SetValue(PaintProperty, brush);
    public static readonly DependencyProperty EdgeProperty = DependencyProperty.RegisterAttached(
        "Edge", typeof(Brush), typeof(ContinuousCorners), new PropertyMetadata(null, PaintChanged));
    public static Brush? GetEdge(DependencyObject value) => (Brush?)value.GetValue(EdgeProperty);
    public static void SetEdge(DependencyObject value, Brush? brush) => value.SetValue(EdgeProperty, brush);
    public static readonly DependencyProperty ClipChildrenProperty = DependencyProperty.RegisterAttached(
        "ClipChildren", typeof(bool), typeof(ContinuousCorners), new PropertyMetadata(false));
    public static bool GetClipChildren(DependencyObject value) => (bool)value.GetValue(ClipChildrenProperty);
    public static void SetClipChildren(DependencyObject value, bool clip) => value.SetValue(ClipChildrenProperty, clip);
    private static readonly ConditionalWeakTable<FrameworkElement, State> States = new();
    private static readonly ConditionalWeakTable<Control, FocusChrome> FocusOwners = new();
    private static readonly SolidColorBrush Transparent = new(Colors.Transparent);
    private static void PaintChanged(DependencyObject value, DependencyPropertyChangedEventArgs args)
    {
        if (value is FrameworkElement element && States.TryGetValue(element, out var state)) state.RefreshPaint();
    }

    private static void EnabledChanged(DependencyObject value, DependencyPropertyChangedEventArgs args)
    {
        if (value is not FrameworkElement element || args.NewValue is not true) return;
        element.Loaded += (_, _) => Install(element);
        if (element.IsLoaded) Install(element);
    }

    internal static void Install(FrameworkElement root)
    {
        if (!root.IsLoaded)
        {
            if (!GetEnabled(root)) SetEnabled(root, true);
            return;
        }
        Visit(root);
    }

    private static void Visit(DependencyObject node)
    {
        if (node is FrameworkElement element && !States.TryGetValue(element, out _) && State.Radius(element) > 0)
            States.Add(element, new State(element));
        int count = VisualTreeHelper.GetChildrenCount(node);
        for (int i = 0; i < count; ++i) Visit(VisualTreeHelper.GetChild(node, i));
    }

    internal static Brush? Original(FrameworkElement element) => States.TryGetValue(element, out var state)
        ? state.Background : State.GetBackground(element);
    internal static double RadiusOf(FrameworkElement element) => States.TryGetValue(element, out var state) ? state.Corner : State.Radius(element);
    // Acquiring a handout visual conflicts with XAML Translation/Scale and their
    // transitions. These brushes only need the compositor; the connected root
    // supplies the same one without taking ownership of an animated element.
    private static Compositor CompositorFor(FrameworkElement element) =>
        ElementCompositionPreview.GetElementVisual(element.XamlRoot?.Content ?? element).Compositor;

    private sealed class State
    {
        private readonly FrameworkElement element;
        private readonly DependencyProperty radiusProperty, backgroundProperty, strokeProperty;
        private AnalyticBrush? fill;
        private AnalyticBrush? outline;
        private SpriteVisual? outlineVisual;
        private bool changing;
        internal double Corner;
        internal Brush? Background;
        private Brush? stroke;
        private readonly Brush? baseBackground, baseStroke;
        private FocusChrome? focus;
        private CompositionGeometricClip? nativeClip;
        private (double Width, double Height, double Radius) clipGeometry;
        internal void RefreshPaint()
        {
            Background = GetPaint(element) ?? baseBackground;
            stroke = GetEdge(element) ?? baseStroke;
            Update();
        }

        internal State(FrameworkElement element)
        {
            this.element = element;
            (radiusProperty, backgroundProperty, strokeProperty) = element switch
            {
                Border => (Border.CornerRadiusProperty, Border.BackgroundProperty, Border.BorderBrushProperty),
                Grid => (Grid.CornerRadiusProperty, Panel.BackgroundProperty, Grid.BorderBrushProperty),
                ContentPresenter => (ContentPresenter.CornerRadiusProperty, ContentPresenter.BackgroundProperty, ContentPresenter.BorderBrushProperty),
                Rectangle => (Rectangle.RadiusXProperty, Shape.FillProperty, Shape.StrokeProperty),
                _ => throw new InvalidOperationException(),
            };
            Corner = Radius(element);
            baseBackground = (Brush?)element.GetValue(backgroundProperty);
            baseStroke = (Brush?)element.GetValue(strokeProperty);
            Background = GetPaint(element) ?? baseBackground;
            stroke = GetEdge(element) ?? baseStroke;
            element.RegisterPropertyChangedCallback(backgroundProperty, (_, _) =>
            {
                if (changing) return;
                Background = (Brush?)element.GetValue(backgroundProperty);
                Update();
            });
            element.RegisterPropertyChangedCallback(strokeProperty, (_, _) =>
            {
                if (changing) return;
                stroke = (Brush?)element.GetValue(strokeProperty);
                Update();
            });
            element.RegisterPropertyChangedCallback(radiusProperty, (_, _) =>
            {
                if (changing) return;
                Corner = Radius(element);
                Update();
            });
            var thicknessProperty = element switch
            {
                Border => Border.BorderThicknessProperty, Grid => Grid.BorderThicknessProperty,
                ContentPresenter => ContentPresenter.BorderThicknessProperty, _ => Shape.StrokeThicknessProperty,
            };
            element.RegisterPropertyChangedCallback(thicknessProperty, (_, _) => { if (!changing) Update(); });
            element.SizeChanged += (_, _) => Update();
            element.ActualThemeChanged += (_, _) => Update();
            element.Unloaded += (_, _) => { fill?.Deactivate(); outline?.Deactivate(); };
            element.Loaded += (_, _) => Update();
            for (var parent = Control.GetIsTemplateFocusTarget(element) ? VisualTreeHelper.GetParent(element) : null;
                 parent != null; parent = VisualTreeHelper.GetParent(parent))
            {
                if (parent is not Control control) continue;
                if (control is not ScrollViewer and not UserControl and not ItemsControl && (control.IsTabStop || control.UseSystemFocusVisuals))
                    focus = FocusOwners.GetValue(control, c => new FocusChrome(c, element));
                break;
            }
            Update();
        }

        internal static double Radius(FrameworkElement e) => e switch
        {
            Border b => b.CornerRadius.TopLeft, Grid p => p.CornerRadius.TopLeft,
            ContentPresenter c => c.CornerRadius.TopLeft,
            Rectangle r => r.RadiusX, _ => 0,
        };
        internal static Brush? GetBackground(FrameworkElement e) => e switch
        {
            Border b => b.Background, Panel p => p.Background, Control c => c.Background,
            ContentPresenter c => c.Background, Rectangle r => r.Fill, _ => null,
        };
        private Thickness Thickness => element switch
        {
            Border b => b.BorderThickness, Grid p => p.BorderThickness,
            ContentPresenter c => c.BorderThickness, Rectangle r => new Thickness(r.StrokeThickness), _ => new Thickness(),
        };
        private TimeSpan Transition => element switch
        {
            Border b => b.BackgroundTransition?.Duration ?? TimeSpan.Zero,
            Panel p => p.BackgroundTransition?.Duration ?? TimeSpan.Zero,
            ContentPresenter c => c.BackgroundTransition?.Duration ?? TimeSpan.Zero, _ => TimeSpan.Zero,
        };

        private void Update()
        {
            if (element.ActualWidth <= 0 || element.ActualHeight <= 0) return;
            changing = true;
            try
            {
                element.SetValue(radiusProperty, element is Rectangle ? 0d : new CornerRadius(0));
                if (element is Rectangle rect) rect.RadiusY = 0;
                // Keep live acrylic and bitmap brushes connected to XAML: image
                // invalidation, alignment, transitions and material policies stay
                // native. The finite path clip below is still the A08 gap;
                // it must not be described as analytic pixel coverage.
                if (Background is AcrylicBrush or ImageBrush)
                {
                    ApplyNativeClip();
                    element.SetValue(backgroundProperty, Background);
                }
                else
                {
                    if (GetClipChildren(element)) ApplyNativeClip();
                    else if (nativeClip != null) ElementCompositionPreview.GetElementVisual(element).Clip = null;
                    if (Background != null)
                    {
                        fill ??= new AnalyticBrush(element);
                        fill.Update(Corner, 0, Background, Transition);
                    }
                    element.SetValue(backgroundProperty, Background == null ? null : fill);
                }
                element.SetValue(strokeProperty, Transparent);
                double width = Math.Max(Math.Max(Thickness.Left, Thickness.Right), Math.Max(Thickness.Top, Thickness.Bottom));
                if (stroke != null && width > 0)
                {
                    if (outline == null)
                    {
                        outline = new AnalyticBrush(element, false);
                        outlineVisual = CompositorFor(element).CreateSpriteVisual();
                        outlineVisual.Brush = outline.Native;
                        ElementCompositionPreview.SetElementChildVisual(element, outlineVisual);
                    }
                    outline.Update(Corner, width, stroke, Transition);
                    outlineVisual!.Size = new Vector2((float)element.ActualWidth, (float)element.ActualHeight);
                    outlineVisual.Opacity = 1;
                }
                else if (outlineVisual != null) outlineVisual.Opacity = 0;
                focus?.Update(Corner);
            }
            finally { changing = false; }
        }

        private void ApplyNativeClip()
        {
            var next = (element.ActualWidth, element.ActualHeight, Corner);
            if (nativeClip != null && clipGeometry == next)
            {
                ElementCompositionPreview.GetElementVisual(element).Clip = nativeClip;
                return;
            }
            clipGeometry = next;
            float w = (float)next.Item1, h = (float)next.Item2;
            var p = CornerProfile.For(w, h, (float)Corner);
            // No CanvasDevice: this path is device-independent. The points are a
            // subpixel tessellation of F=0 for the platform clip. Its finite
            // line segments are NOT a G3 visible boundary (audit A08).
            using var builder = new CanvasPathBuilder(null);
            int steps = Math.Max(16, (int)Math.Ceiling(Math.PI * Math.Sqrt(p.Reach / 0.01) / 2));
            for (int corner = 0; corner < 4; ++corner)
                for (int i = 0; i <= steps; ++i)
                {
                    var q = p.Point(i * Math.PI / (2 * steps));
                    Vector2 point = corner switch
                    {
                        0 => new(w - p.Reach + p.Reach * q.X, p.Reach + p.Reach * q.Y),
                        1 => new(w - p.Reach - p.Reach * q.Y, h - p.Reach + p.Reach * q.X),
                        2 => new(p.Reach - p.Reach * q.X, h - p.Reach - p.Reach * q.Y),
                        _ => new(p.Reach + p.Reach * q.Y, p.Reach - p.Reach * q.X),
                    };
                    if (corner == 0 && i == 0) builder.BeginFigure(point); else builder.AddLine(point);
                }
            builder.EndFigure(CanvasFigureLoop.Closed);
            using var geometry = CanvasGeometry.CreatePath(builder);
            var compositor = ElementCompositionPreview.GetElementVisual(element).Compositor;
            var path = compositor.CreatePathGeometry(new CompositionPath(geometry));
            nativeClip ??= compositor.CreateGeometricClip();
            nativeClip.Geometry = path;
            ElementCompositionPreview.GetElementVisual(element).Clip = nativeClip;
        }
    }

    private sealed class FocusChrome
    {
        private readonly Control control;
        private readonly FrameworkElement owner;
        private readonly AnalyticBrush primary, secondary;
        private readonly SpriteVisual outer, inner;
        private readonly ContainerVisual visual;
        private readonly double primaryWidth, secondaryWidth;
        internal FocusChrome(Control control, FrameworkElement owner)
        {
            this.control = control;
            this.owner = owner;
            primaryWidth = control.FocusVisualPrimaryThickness.Left;
            secondaryWidth = control.FocusVisualSecondaryThickness.Left;
            // Keep the original brushes (including their theme bindings), widths
            // and margin. Suppress only the platform's circular focus geometry.
            control.UseSystemFocusVisuals = true;
            control.FocusVisualPrimaryThickness = new Thickness(0);
            control.FocusVisualSecondaryThickness = new Thickness(0);
            var compositor = CompositorFor(owner);
            primary = new AnalyticBrush(owner, false);
            secondary = new AnalyticBrush(owner, false);
            outer = compositor.CreateSpriteVisual(); outer.Brush = primary.Native;
            inner = compositor.CreateSpriteVisual(); inner.Brush = secondary.Native;
            visual = compositor.CreateContainerVisual();
            visual.Children.InsertAtTop(outer);
            visual.Children.InsertAtTop(inner);
            ElementCompositionPreview.SetElementChildVisual(control, visual);
            control.RegisterPropertyChangedCallback(Control.FocusStateProperty, (_, _) => Show());
            Show();
        }
        internal void Update(double radius)
        {
            var margin = control.FocusVisualMargin;
            double w = Math.Max(0, owner.ActualWidth - margin.Left - margin.Right);
            double h = Math.Max(0, owner.ActualHeight - margin.Top - margin.Bottom);
            radius -= margin.Left;
            primary.Update(radius, primaryWidth + secondaryWidth, control.FocusVisualPrimaryBrush, TimeSpan.Zero, w, h);
            outer.Size = new Vector2((float)w, (float)h);
            secondary.Update(Math.Max(0, radius - primaryWidth), secondaryWidth, control.FocusVisualSecondaryBrush, TimeSpan.Zero,
                Math.Max(0, w - 2 * primaryWidth), Math.Max(0, h - 2 * primaryWidth));
            inner.Size = new Vector2((float)Math.Max(0, w - 2 * primaryWidth), (float)Math.Max(0, h - 2 * primaryWidth));
            inner.Offset = new Vector3((float)primaryWidth, (float)primaryWidth, 0);
            inner.Opacity = secondaryWidth > 0 ? 1 : 0;
            outer.Opacity = primaryWidth > 0 ? 1 : 0;
            var position = owner.TransformToVisual(control).TransformPoint(default);
            visual.Offset = new Vector3((float)(position.X + margin.Left), (float)(position.Y + margin.Top), 0);
        }
        private void Show() => visual.Opacity = control.FocusState == FocusState.Keyboard ? 1 : 0;
    }

    private sealed class AnalyticBrush : XamlCompositionBrushBase
    {
        private readonly FrameworkElement owner;
        private readonly Compositor compositor;
        private readonly CompositionMaskBrush mask;
        private readonly CompositionColorBrush color;
        private readonly CompositionNineGridBrush nine;
        private readonly CompositionSurfaceBrush maskSource;
        private readonly SpriteVisual? raster;
        private readonly CompositionVisualSurface? rasterSurface;
        private Brush? source;
        private long sourceToken;
        private long gradientStartToken, gradientEndToken;
        private CompositionLinearGradientBrush? gradient;
        private MaskKey? activeMask;
        private (double W, double H, double R, double Stroke, double Scale) geometry;
        private int generation;
        internal CompositionBrush Native => mask;

        internal AnalyticBrush(FrameworkElement owner, bool xaml = true)
        {
            this.owner = owner;
            compositor = CompositorFor(owner);
            color = compositor.CreateColorBrush(Colors.Transparent);
            mask = compositor.CreateMaskBrush();
            mask.Source = color;
            maskSource = compositor.CreateSurfaceBrush();
            maskSource.Stretch = CompositionStretch.Fill;
            nine = compositor.CreateNineGridBrush();
            nine.Source = maskSource;
            mask.Mask = nine;
            if (xaml)
            {
                // XAML adds its own primitive mask. A surface is a supported
                // input there; nesting a CompositionMaskBrush is not.
                raster = compositor.CreateSpriteVisual();
                raster.Brush = mask;
                rasterSurface = compositor.CreateVisualSurface();
                rasterSurface.SourceVisual = raster;
                var surfaceBrush = compositor.CreateSurfaceBrush(rasterSurface);
                surfaceBrush.Stretch = CompositionStretch.Fill;
                CompositionBrush = surfaceBrush;
            }
        }

        internal void Update(double radius, double stroke, Brush? next, TimeSpan transition, double? width = null, double? height = null)
        {
            var nextGeometry = (width ?? owner.ActualWidth, height ?? owner.ActualHeight, radius, stroke, owner.XamlRoot?.RasterizationScale ?? 1);
            if (geometry != nextGeometry)
            {
                geometry = nextGeometry;
                if (raster != null)
                {
                    raster.Size = new Vector2((float)(geometry.W * geometry.Scale), (float)(geometry.H * geometry.Scale));
                    rasterSurface!.SourceSize = raster.Size;
                }
                _ = SetMaskAsync(++generation);
            }
            if (!ReferenceEquals(source, next))
            {
                Deactivate();
                source = next;
                if (source is SolidColorBrush)
                    sourceToken = source.RegisterPropertyChangedCallback(SolidColorBrush.ColorProperty, (_, _) => SetSource(TimeSpan.Zero));
                if (source is LinearGradientBrush)
                {
                    gradientStartToken = source.RegisterPropertyChangedCallback(LinearGradientBrush.StartPointProperty, (_, _) => SetGradientPosition());
                    gradientEndToken = source.RegisterPropertyChangedCallback(LinearGradientBrush.EndPointProperty, (_, _) => SetGradientPosition());
                }
                SetSource(transition);
            }
        }

        internal void Deactivate()
        {
            if (source is SolidColorBrush) source.UnregisterPropertyChangedCallback(SolidColorBrush.ColorProperty, sourceToken);
            if (source is LinearGradientBrush)
            {
                source.UnregisterPropertyChangedCallback(LinearGradientBrush.StartPointProperty, gradientStartToken);
                source.UnregisterPropertyChangedCallback(LinearGradientBrush.EndPointProperty, gradientEndToken);
            }
            source = null;
        }

        private void SetSource(TimeSpan transition)
        {
            if (source is LinearGradientBrush linear)
            {
                gradient ??= compositor.CreateLinearGradientBrush();
                gradient.ColorStops.Clear();
                foreach (var stop in linear.GradientStops) gradient.ColorStops.Add(compositor.CreateColorGradientStop((float)stop.Offset, stop.Color));
                SetGradientPosition();
                mask.Source = gradient;
                return;
            }
            mask.Source = color;
            Color target = source is SolidColorBrush solid ? solid.Color : Colors.Transparent;
            if (transition > TimeSpan.Zero)
            {
                var animation = compositor.CreateColorKeyFrameAnimation();
                // Auto can sweep through purple between neutral and blue UI
                // states. Interpolate the channels without introducing a hue.
                animation.InterpolationColorSpace = CompositionColorSpace.Rgb;
                animation.Duration = transition;
                animation.InsertKeyFrame(1, target, compositor.CreateLinearEasingFunction());
                color.StartAnimation("Color", animation);
            }
            else { color.StopAnimation("Color"); color.Color = target; }
        }

        private void SetGradientPosition()
        {
            if (source is not LinearGradientBrush linear || gradient == null) return;
            gradient.StartPoint = new Vector2((float)linear.StartPoint.X, (float)linear.StartPoint.Y);
            gradient.EndPoint = new Vector2((float)linear.EndPoint.X, (float)linear.EndPoint.Y);
        }

        private async Task SetMaskAsync(int ticket)
        {
            var g = geometry;
            float scale = (float)g.Scale, w = (float)g.W * scale, h = (float)g.H * scale, r = (float)g.R * scale, s = (float)g.Stroke * scale;
            var profile = CornerProfile.For(w, h, r);
            var inner = CornerProfile.For(Math.Max(0, w - 2 * s), Math.Max(0, h - 2 * s), Math.Max(0, r - s));
            var key = new MaskKey(profile, inner, s);
            nine.SetInsets(profile.Reach);
            nine.SetInsetScales(raster == null ? 1 / scale : 1);
            if (activeMask == key && maskSource.Surface != null) return;
            var surface = await MaskForAsync(key);
            if (ticket != generation) return;
            maskSource.Surface = surface;
            activeMask = key;
        }
    }

    private readonly record struct MaskKey(CornerProfile Outer, CornerProfile Inner, float Stroke);
    private static readonly Dictionary<MaskKey, (long Bytes, Task<LoadedImageSurface> Surface)> Masks = new();
    private static long maskBytes;
    private static Task<LoadedImageSurface> MaskForAsync(MaskKey key)
    {
        if (Masks.TryGetValue(key, out var cached)) return cached.Surface;
        int inset = (int)MathF.Ceiling(key.Outer.Reach) + 1, size = inset * 2 + 1;
        long bytes = (long)size * size * 4;
        while (Masks.Count > 0 && (Masks.Count >= 64 || maskBytes + bytes > 4 * 1024 * 1024))
        {
            var first = Masks.First();
            maskBytes -= first.Value.Bytes;
            Masks.Remove(first.Key);
        }
        var pixels = new byte[size * size * 4];
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                float a = key.Outer.Coverage(x + 0.5f, y + 0.5f, size, size);
                if (key.Stroke > 0)
                    a = Math.Max(0, a - key.Inner.Coverage(x + 0.5f - key.Stroke, y + 0.5f - key.Stroke,
                        size - 2 * key.Stroke, size - 2 * key.Stroke));
                int offset = (y * size + x) * 4;
                pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = pixels[offset + 3] = (byte)MathF.Round(a * 255);
            }
        var surface = LoadPixelsAsync((uint)size, (uint)size, pixels);
        Masks.Add(key, (bytes, surface));
        maskBytes += bytes;
        return surface;
    }

    private static async Task<LoadedImageSurface> LoadPixelsAsync(uint width, uint height, byte[] pixels)
    {
        using var stream = new InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, width, height, 96, 96, pixels);
        await encoder.FlushAsync();
        stream.Seek(0);
        var surface = LoadedImageSurface.StartLoadFromStream(stream);
        var loaded = new TaskCompletionSource();
        surface.LoadCompleted += (_, _) => loaded.TrySetResult();
        await loaded.Task;
        return surface;
    }
}
