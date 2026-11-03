using System.Numerics;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace Atrium.Settings;

// The switch's knob (SwitchStyle): pressed, it stretches towards where it will go and squashes a
// little; let go (or toggled) it springs back to round as it lands, so the switch feels elastic.
// Composition animations on the knob's own visual: no layout, nothing on the UI thread per frame.
public sealed class KnobFace : Grid
{
    private ToggleSwitch? _owner;
    private Visual? _visual;
    private bool _pressed;
    private readonly PointerEventHandler _down, _up;

    public KnobFace()
    {
        _down = (_, _) => Stretch();
        _up = (_, _) => { if (_pressed) Relax(); };
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        DependencyObject? p = this;
        while (p != null && p is not ToggleSwitch) p = VisualTreeHelper.GetParent(p);
        if (p is not ToggleSwitch owner || owner == _owner) return;
        _owner = owner;
        _visual = ElementCompositionPreview.GetElementVisual(this);
        owner.AddHandler(PointerPressedEvent, _down, true);
        owner.AddHandler(PointerReleasedEvent, _up, true);
        owner.AddHandler(PointerCanceledEvent, _up, true);
        owner.AddHandler(PointerCaptureLostEvent, _up, true);
        owner.Toggled += OnToggled;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        if (_owner == null) return;
        _owner.RemoveHandler(PointerPressedEvent, _down);
        _owner.RemoveHandler(PointerReleasedEvent, _up);
        _owner.RemoveHandler(PointerCanceledEvent, _up);
        _owner.RemoveHandler(PointerCaptureLostEvent, _up);
        _owner.Toggled -= OnToggled;
        _owner = null;
    }

    // Anchored on the side it rests on, it grows towards the other side.
    private void Anchor() =>
        _visual!.CenterPoint = new Vector3(_owner!.IsOn ? (float)ActualWidth : 0, (float)ActualHeight / 2, 0);

    private void Stretch()
    {
        if (_visual == null || _owner is not { IsEnabled: true }) return;
        _pressed = true;
        Anchor();
        var c = _visual.Compositor;
        var a = c.CreateVector3KeyFrameAnimation();
        a.InsertKeyFrame(1, new Vector3(1.26f, 0.93f, 1), c.CreateCubicBezierEasingFunction(new Vector2(0.2f, 0.9f), new Vector2(0.3f, 1f)));
        a.Duration = TimeSpan.FromMilliseconds(150);
        _visual.StartAnimation("Scale", a);
    }

    // Back to round on a lively spring; the anchor is the side it now rests on, so it lands
    // leading edge first and the trailing edge catches up.
    private void Relax()
    {
        if (_visual == null || _owner == null) return;
        _pressed = false;
        Anchor();
        var s = _visual.Compositor.CreateSpringVector3Animation();
        s.FinalValue = Vector3.One;
        s.DampingRatio = 0.52f;
        s.Period = TimeSpan.FromMilliseconds(48);
        _visual.StartAnimation("Scale", s);
    }

    private void OnToggled(object sender, RoutedEventArgs e)
    {
        if (_pressed) Relax();
    }
}

// A segmented control: the chosen segment's outline glides to the one picked (no check mark).
// Holds RadioButtons in SegmentStyle as its XAML children.
public sealed class SegmentTrack : Grid
{
    private readonly Border _pill = new()
    {
        CornerRadius = new CornerRadius(13), BorderThickness = new Thickness(1), HorizontalAlignment = HorizontalAlignment.Left,
        IsHitTestVisible = false, Opacity = 0,
    };
    private readonly TranslateTransform _move = new();
    private RadioButton? _at;
    private bool _ready;

    public SegmentTrack()
    {
        Background = Tokens.Brush("FillBrush");
        CornerRadius = new CornerRadius(15);
        Padding = new Thickness(2);
        VerticalAlignment = VerticalAlignment.Center;
        HorizontalAlignment = HorizontalAlignment.Left;
        _pill.Background = Tokens.Brush("SegmentSelectedBrush");
        _pill.BorderBrush = Tokens.Brush("AccentBrush");
        _pill.RenderTransform = _move;
        Children.Add(_pill); // before the XAML children: it sits behind them
        Loaded += (_, _) => Setup();
    }

    private void Setup()
    {
        if (_ready) return;
        _ready = true;
        int col = 0;
        foreach (var child in Children)
        {
            if (child is not RadioButton rb) continue;
            ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            SetColumn(rb, col++);
            rb.Checked += (_, _) => MoveTo(rb, true);
            rb.SizeChanged += (_, _) => { if (rb == _at && !_ticking) MoveTo(rb, false); };
        }
        SetColumnSpan(_pill, Math.Max(1, col));
        var on = Children.OfType<RadioButton>().FirstOrDefault(r => r.IsChecked == true);
        if (on != null) MoveTo(on, false);
    }

    // Where the outline is going; it follows on the frame clock (an interruptible, frame-rate
    // independent approach, as the bar's hover plates), so a second pick mid-way just turns it.
    private double _x, _w, _toX, _toW;
    private long _last;
    private bool _ticking;

    private void MoveTo(RadioButton rb, bool animate)
    {
        _at = rb;
        if (!_ready || rb.ActualWidth <= 0) return;
        _toX = rb.TransformToVisual(this).TransformPoint(default).X - Padding.Left;
        _toW = rb.ActualWidth;
        if (!animate || _pill.Opacity < 0.5)
        {
            _x = _toX; _w = _toW;
            Apply();
            _pill.Opacity = 1;
            return;
        }
        if (_ticking) return;
        _ticking = true;
        _last = Environment.TickCount64;
        CompositionTarget.Rendering += Tick;
    }

    private void Tick(object? sender, object e)
    {
        long now = Environment.TickCount64;
        double dt = Math.Clamp((now - _last) / 1000.0, 0, 0.05), k = 1 - Math.Exp(-dt * 16);
        _last = now;
        _x += (_toX - _x) * k;
        _w += (_toW - _w) * k;
        if (Math.Abs(_toX - _x) < 0.2 && Math.Abs(_toW - _w) < 0.2)
        {
            _x = _toX; _w = _toW;
            CompositionTarget.Rendering -= Tick;
            _ticking = false;
        }
        Apply();
    }

    private void Apply()
    {
        _pill.Width = Math.Max(0, _w);
        _move.X = _x;
    }
}
