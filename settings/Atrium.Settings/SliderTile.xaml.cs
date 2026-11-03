using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace Atrium.Settings;

// One adjustable parameter on a tile: name, value, slider, a word for the value and a description.
public sealed partial class SliderTile : UserControl
{
    private Func<double, string>? _hint;
    private string _unit = "";
    private double _min, _max;

    public event Action<double>? UserValueChanged;
    public event Action<bool>? Engaged;

    public SliderTile()
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
        HintText.Text = _hint?.Invoke(v) ?? "";
        ValueRun.Text = Math.Round(v).ToString(System.Globalization.CultureInfo.InvariantCulture);
        UnitRun.Text = _unit is "" or "%" ? _unit : " " + _unit;
    }

    private void OnTrackChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        Refresh(e.NewValue);
        UserValueChanged?.Invoke(e.NewValue);
    }

    // The tile being adjusted is lit a little (the hero shows that parameter).
    private void SetEngaged(bool on)
    {
        Surface.Background = Tokens.Brush(on ? "TileActiveBrush" : "CardBrush");
        Engaged?.Invoke(on);
    }
}
