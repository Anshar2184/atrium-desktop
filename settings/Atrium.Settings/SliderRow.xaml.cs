using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace Atrium.Settings;

// One adjustable parameter: name, value (with a word for it), slider, description.
public sealed partial class SliderRow : UserControl
{
    private Func<double, string>? _hint;
    private string _unit = "";
    private double _min, _max;

    public event Action<double>? UserValueChanged;
    public event Action<bool>? Engaged;

    public SliderRow()
    {
        InitializeComponent();
        GotFocus += (_, _) => SetEngaged(true);
        LostFocus += (_, _) => SetEngaged(false);
    }

    public void Configure(string title, string description, double min, double max, double step, string unit, Func<double, string> hint)
    {
        _unit = unit; _min = min; _max = max; _hint = hint;
        TitleText.Text = title;
        DescriptionText.Text = description;
        DescriptionText.Visibility = description.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
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
        string hint = _hint?.Invoke(v) ?? "";
        HintRun.Text = hint.Length > 0 ? hint + "   " : "";
        ValueRun.Text = Math.Round(v).ToString(System.Globalization.CultureInfo.InvariantCulture);
        UnitRun.Text = _unit.Length > 0 ? " " + _unit : "";
    }

    private void OnTrackChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        Refresh(e.NewValue);
        UserValueChanged?.Invoke(e.NewValue);
    }

    // The row being adjusted is lit a little (the hero shows that parameter).
    private void SetEngaged(bool on)
    {
        Surface.Background = on ? Tokens.Brush("HoverBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        Engaged?.Invoke(on);
    }
}
