using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace Atrium.Settings;

// One adjustable parameter: illustration, title, big value, plain-language description, slider.
public sealed partial class SliderCard : UserControl
{
    private Func<double, string>? _hint;
    private Action<double>? _glyph;
    private string _unit = "";
    private double _min, _max;

    public event Action<double>? UserValueChanged;
    public event Action<bool>? Engaged;

    public SliderCard()
    {
        InitializeComponent();
        GotFocus += (_, _) => SetEngaged(true);
        LostFocus += (_, _) => SetEngaged(false);
    }

    public void Configure(string title, string description, double min, double max, double step, string unit,
        string minCaption, string maxCaption, Func<double, string> hint, FrameworkElement glyph, Action<double> glyphUpdate)
    {
        _unit = unit; _min = min; _max = max; _hint = hint; _glyph = glyphUpdate;
        TitleText.Text = title;
        DescriptionText.Text = description;
        DescriptionText.Visibility = description.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        MinCaption.Text = minCaption;
        MaxCaption.Text = maxCaption;
        GlyphHost.Child = glyph;
        Track.Minimum = min; Track.Maximum = max; Track.StepFrequency = step;
        AutomationProperties.SetName(Track, title);
    }

    public double Value => Track.Value;

    // Sets the slider without raising UserValueChanged semantics for the caller (the caller guards with its own flag).
    public void SetValue(double v)
    {
        Track.Value = Math.Clamp(v, _min, _max);
        Refresh(Track.Value);
    }

    private void Refresh(double v)
    {
        ValueRun.Text = Math.Round(v).ToString(System.Globalization.CultureInfo.InvariantCulture);
        UnitRun.Text = _unit.Length > 0 ? " " + _unit : "";
        HintText.Text = _hint?.Invoke(v) ?? "";
        _glyph?.Invoke(_max > _min ? (v - _min) / (_max - _min) : 0);
    }

    private void OnTrackChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        Refresh(e.NewValue);
        UserValueChanged?.Invoke(e.NewValue);
    }

    private void SetEngaged(bool on)
    {
        Ring.Opacity = on ? 1 : 0;
        Engaged?.Invoke(on);
    }
}
