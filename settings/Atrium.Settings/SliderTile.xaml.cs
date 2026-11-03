using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace Atrium.Settings;

// One adjustable parameter: name, value and slider.
public sealed partial class SliderTile : UserControl
{
    private string _unit = "";
    private double _min, _max;
    private bool _flat;

    public event Action<double>? UserValueChanged;
    public event Action<bool>? Engaged;

    public SliderTile()
    {
        InitializeComponent();
        GotFocus += (_, _) => SetEngaged(true);
        LostFocus += (_, _) => SetEngaged(false);
    }

    public void Configure(string title, double min, double max, double step, string unit)
    {
        _unit = unit; _min = min; _max = max;
        TitleText.Text = title;
        Track.Minimum = min; Track.Maximum = max; Track.StepFrequency = step;
        AutomationProperties.SetName(Track, title);
    }

    // In the inspector: a plain row, no card (lit while in use all the same).
    public bool Flat
    {
        get => _flat;
        set
        {
            _flat = value;
            Surface.Style = (Style)Application.Current.Resources[value ? "FlatTileStyle" : "TileStyle"];
            Surface.Background = Tokens.Brush(value ? "TransparentBrush" : "CardBrush");
        }
    }

    // Sets the slider without raising UserValueChanged semantics for the caller (the caller guards with its own flag).
    public void SetValue(double v)
    {
        Track.Value = Math.Clamp(v, _min, _max);
        Refresh(Track.Value);
    }

    private void Refresh(double v)
    {
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
        Surface.Background = Tokens.Brush(on ? "TileActiveBrush" : _flat ? "TransparentBrush" : "CardBrush");
        Engaged?.Invoke(on);
    }
}
