using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Atrium.Settings;

public sealed partial class MainWindow : Window
{
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);

    // One slider row: setting key, UI range, and the mapping between the stored
    // value and the slider value (percentages are stored as 0..1).
    private sealed record SliderDef(string Title, string Description, double Min, double Max, double Step, string Unit,
        Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private static readonly SliderDef[] MaterialSliders =
    {
        new("透明度", "越高越通透，越低染色越重。", 0, 100, 1, "%", s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("模糊", "背景的模糊程度。", 0, 100, 1, "%", s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("折射强度", "边缘处背景被“弯曲”的最大距离。", 0, 60, 1, "px", s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("折射边缘宽度", "从外沿向内发生折射的宽度。", 1, 80, 1, "px", s => s.Edge, (s, v) => s.Edge = v, true),
        new("色散", "边缘处红绿蓝通道的分离，形成彩虹边。", 0, 50, 1, "%", s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        new("高光", "边缘的反光强度。", 0, 100, 1, "%", s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("阴影", "玻璃下方的投影浓度。", 0, 100, 1, "%", s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),
    };

    private static readonly SliderDef[] LayoutSliders =
    {
        new("圆角", "超过高度的一半时即为胶囊形。", 0, 80, 1, "px", s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("宽度", "不会超过屏幕工作区。", 300, 2400, 10, "px", s => s.Width, (s, v) => s.Width = v, false),
        new("高度", "", 32, 120, 1, "px", s => s.Height, (s, v) => s.Height = v, false),
        new("底部间距", "与工作区底边（系统任务栏上方）的距离。", 0, 200, 1, "px", s => s.Margin, (s, v) => s.Margin = v, false),
    };

    private readonly CoreClient _client = new();
    private readonly List<(SliderDef Def, Slider Slider, TextBlock Value)> _rows = new();
    private BarSettings _saved = new();
    private BarSettings _current = new();
    private bool _loading;
    private bool _previewBusy, _previewPending;
    private bool _closingConfirmed;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "atrium.ico"));
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new Windows.Graphics.SizeInt32((int)(760 * scale), (int)(820 * scale)));
        AppWindow.Closing += OnClosing;

        foreach (var d in MaterialSliders) MaterialPanel.Children.Add(CreateRow(d));
        foreach (var d in LayoutSliders) LayoutPanel.Children.Add(CreateRow(d));
        PathText.Text = "设置文件：" + CoreClient.SettingsPath;
        _ = ConnectAndLoadAsync();
    }

    private Grid CreateRow(SliderDef d)
    {
        var card = new Grid { Style = (Style)Application.Current.Resources["CardStyle"], ColumnSpacing = 16 };
        card.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        card.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(260) });
        card.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(56) });
        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = d.Title });
        if (d.Description.Length > 0)
            text.Children.Add(new TextBlock { Text = d.Description, Style = (Style)Application.Current.Resources["CardDescriptionStyle"] });
        var slider = new Slider { Minimum = d.Min, Maximum = d.Max, StepFrequency = d.Step, VerticalAlignment = VerticalAlignment.Center };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(slider, d.Title);
        slider.SetValue(Grid.ColumnProperty, 1);
        var value = new TextBlock { VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right };
        value.SetValue(Grid.ColumnProperty, 2);
        card.Children.Add(text);
        card.Children.Add(slider);
        card.Children.Add(value);
        _rows.Add((d, slider, value));
        slider.ValueChanged += (_, e) => OnSliderChanged(d, value, e.NewValue);
        return card;
    }

    private async Task ConnectAndLoadAsync()
    {
        bool connected = await _client.ConnectAsync(800);
        var (saved, current) = await _client.GetAsync();
        _saved = saved;
        _current = current;
        ShowStatus(connected);
        LoadIntoControls();
        UpdateButtons();
    }

    private void ShowStatus(bool connected)
    {
        if (connected)
        {
            Status.Severity = InfoBarSeverity.Success;
            Status.Title = "Atrium 正在运行";
            Status.Message = "修改会实时显示在玻璃栏上。";
            StartCoreButton.Visibility = Visibility.Collapsed;
        }
        else
        {
            Status.Severity = InfoBarSeverity.Informational;
            Status.Title = "Atrium 未运行";
            Status.Message = "可以继续修改并保存，下次启动时生效。";
            StartCoreButton.Visibility = CoreExe() != null ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private static string? CoreExe()
    {
        foreach (var p in new[] { Path.Combine(AppContext.BaseDirectory, "..", "Atrium.Core.exe"), Path.Combine(AppContext.BaseDirectory, "Atrium.Core.exe") })
            if (File.Exists(p)) return Path.GetFullPath(p);
        return null;
    }

    private async void OnStartCore(object sender, RoutedEventArgs e)
    {
        var exe = CoreExe();
        if (exe == null) return;
        Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
        StartCoreButton.IsEnabled = false;
        for (int i = 0; i < 20 && !_client.Connected; i++)
        {
            await Task.Delay(250);
            await _client.ConnectAsync(250);
        }
        StartCoreButton.IsEnabled = true;
        if (_client.Connected)
        {
            ShowStatus(true);
            if (!_current.SameAs(_saved)) SchedulePreview();
        }
    }

    private void LoadIntoControls()
    {
        _loading = true;
        foreach (var (d, slider, value) in _rows)
        {
            slider.Value = Math.Clamp(d.Get(_current), d.Min, d.Max);
            value.Text = Format(d, slider.Value);
        }
        PresetBox.SelectedIndex = _current.Preset switch { "liquid" => 0, "frosted" => 1, _ => 2 };
        ClockSwitch.IsOn = _current.ShowClock;
        var c = TintColor(_current.Tint);
        TintPicker.Color = c;
        TintSwatch.Background = new SolidColorBrush(c);
        _loading = false;
    }

    private static string Format(SliderDef d, double v) => $"{Math.Round(v)} {d.Unit}";

    private static Color TintColor(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    private void OnSliderChanged(SliderDef d, TextBlock value, double v)
    {
        value.Text = Format(d, v);
        if (_loading) return;
        d.Set(_current, v);
        if (d.Material) MarkCustom();
        Changed();
    }

    private void MarkCustom()
    {
        _current.Preset = "custom";
        _loading = true;
        PresetBox.SelectedIndex = 2;
        _loading = false;
    }

    private void OnPresetChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || PresetBox.SelectedItem is not ComboBoxItem { Tag: string tag }) return;
        if (tag == "liquid") _current.ApplyMaterial(BarSettings.Liquid());
        else if (tag == "frosted") _current.ApplyMaterial(BarSettings.Frosted());
        else _current.Preset = "custom";
        LoadIntoControls();
        Changed();
    }

    private void OnTintChanged(ColorPicker sender, ColorChangedEventArgs args)
    {
        TintSwatch.Background = new SolidColorBrush(args.NewColor);
        if (_loading) return;
        _current.Tint = ((uint)args.NewColor.R << 16) | ((uint)args.NewColor.G << 8) | args.NewColor.B;
        MarkCustom();
        Changed();
    }

    private void OnClockToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowClock = ClockSwitch.IsOn;
        Changed();
    }

    private void Changed()
    {
        UpdateButtons();
        SchedulePreview();
    }

    private void UpdateButtons()
    {
        bool dirty = !_current.SameAs(_saved);
        SaveButton.IsEnabled = dirty;
        RevertButton.IsEnabled = dirty;
    }

    // At most one preview in flight; the newest state is sent when it returns,
    // so dragging a slider never queues up stale frames.
    private async void SchedulePreview()
    {
        if (!_client.Connected) return;
        if (_previewBusy)
        {
            _previewPending = true;
            return;
        }
        _previewBusy = true;
        do
        {
            _previewPending = false;
            await _client.PreviewAsync(_current.Clone());
            await Task.Delay(16);
        } while (_previewPending);
        _previewBusy = false;
        if (!_client.Connected) ShowStatus(false);
    }

    private async void OnSave(object sender, RoutedEventArgs e) => await SaveAsync();

    private async Task<bool> SaveAsync()
    {
        var error = await _client.CommitAsync(_current.Clone());
        if (error != null)
        {
            Status.Severity = InfoBarSeverity.Error;
            Status.Title = "保存失败";
            Status.Message = error;
            return false;
        }
        _saved = _current.Clone();
        UpdateButtons();
        ShowStatus(_client.Connected);
        return true;
    }

    private async void OnRevert(object sender, RoutedEventArgs e)
    {
        _current = _saved.Clone();
        LoadIntoControls();
        UpdateButtons();
        await _client.CancelAsync();
    }

    private void OnDefaults(object sender, RoutedEventArgs e)
    {
        _current = BarSettings.Liquid();
        LoadIntoControls();
        Changed();
    }

    private async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_closingConfirmed || _current.SameAs(_saved))
        {
            _client.Dispose(); // the core reverts any unsaved preview on disconnect
            return;
        }
        args.Cancel = true;
        var dialog = new ContentDialog
        {
            XamlRoot = Content.XamlRoot,
            Title = "保存修改？",
            Content = "未保存的修改在关闭后会被撤销。",
            PrimaryButtonText = "保存",
            SecondaryButtonText = "不保存",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
        };
        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.None) return;
        if (result == ContentDialogResult.Primary && !await SaveAsync()) return;
        _closingConfirmed = true;
        Close();
    }
}
