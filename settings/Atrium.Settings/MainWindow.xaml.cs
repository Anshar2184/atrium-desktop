using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage;
using Windows.Storage.Pickers;
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
        new("最大宽度", "开启“自动宽度”时，玻璃栏按内容收缩，最宽不超过此值；不会超过屏幕工作区。", 300, 2400, 10, "px", s => s.Width, (s, v) => s.Width = v, false),
        new("高度", "", 32, 120, 1, "px", s => s.Height, (s, v) => s.Height = v, false),
        new("底部间距", "与工作区底边（系统任务栏上方）的距离。", 0, 200, 1, "px", s => s.Margin, (s, v) => s.Margin = v, false),
        new("图标大小", "任务栏图标的边长。", 16, 64, 1, "px", s => s.IconSize, (s, v) => s.IconSize = v, false),
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
        // The auto-width card sits right after the width row.
        for (int i = 0; i < LayoutSliders.Length; i++)
            LayoutPanel.Children.Insert(i < 2 ? i : i + 1, CreateRow(LayoutSliders[i]));
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
        var value = new TextBlock { VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right, TextAlignment = TextAlignment.Right };
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
        AutoWidthSwitch.IsOn = _current.AutoWidth;
        RunningSwitch.IsOn = _current.ShowRunning;
        TextColorBox.SelectedIndex = _current.TextColor switch { "light" => 1, "dark" => 2, _ => 0 };
        RebuildPins();
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

    private void OnAutoWidthToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.AutoWidth = AutoWidthSwitch.IsOn;
        Changed();
    }

    private void OnRunningToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowRunning = RunningSwitch.IsOn;
        Changed();
    }

    private void OnTextColorChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || TextColorBox.SelectedItem is not ComboBoxItem { Tag: string tag }) return;
        _current.TextColor = tag;
        Changed();
    }

    // ---- Pinned apps ----

    private readonly Dictionary<string, ImageSource?> _iconCache = new(StringComparer.OrdinalIgnoreCase);

    private static string PinTitle(PinnedApp p) =>
        p.Name.Length > 0 ? p.Name : p.Path.Length > 0 ? System.IO.Path.GetFileNameWithoutExtension(p.Path) : p.Aumid;

    private void RebuildPins(int focusIndex = -1, string? focusTag = null)
    {
        var pins = _current.Pins ?? new List<PinnedApp>();
        PinsList.Children.Clear();
        PinsEmpty.Visibility = pins.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        for (int i = 0; i < pins.Count; i++)
        {
            if (i > 0)
                PinsList.Children.Add(new Border { Height = 1, Background = (Brush)Application.Current.Resources["DividerStrokeColorDefaultBrush"] });
            PinsList.Children.Add(CreatePinRow(pins[i], i, pins.Count));
        }
        if (focusIndex >= 0 && pins.Count > 0)
        {
            focusIndex = Math.Clamp(focusIndex, 0, pins.Count - 1);
            var row = (Grid)PinsList.Children[focusIndex * 2];
            var buttons = row.Children.OfType<StackPanel>().First().Children.OfType<Button>().ToList();
            var target = buttons.FirstOrDefault(b => (string)b.Tag == focusTag && b.IsEnabled) ?? buttons.FirstOrDefault(b => b.IsEnabled);
            target?.Focus(FocusState.Programmatic);
        }
    }

    private Grid CreatePinRow(PinnedApp pin, int index, int count)
    {
        var title = PinTitle(pin);
        var row = new Grid { Padding = new Thickness(0, 8, 0, 8), ColumnSpacing = 12, MinHeight = 48 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(32) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var icon = new Grid { Width = 32, Height = 32, VerticalAlignment = VerticalAlignment.Center };
        var glyph = new FontIcon { Glyph = "\uECAA", FontSize = 22, Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"] };
        icon.Children.Add(glyph);
        row.Children.Add(icon);
        _ = LoadIconAsync(pin, icon, glyph);

        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = title, TextTrimming = TextTrimming.CharacterEllipsis });
        // Files show where they are; Store and system apps have no useful path.
        var detail = pin.Path.Length > 0 ? pin.Path : "已安装的应用";
        if (detail.Length > 0 && detail != title)
            text.Children.Add(new TextBlock { Text = detail, Style = (Style)Application.Current.Resources["CardDescriptionStyle"], TextTrimming = TextTrimming.CharacterEllipsis, TextWrapping = TextWrapping.NoWrap });
        text.SetValue(Grid.ColumnProperty, 1);
        row.Children.Add(text);

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        buttons.SetValue(Grid.ColumnProperty, 2);
        buttons.Children.Add(PinButton("\uE70E", "上移", title, "up", index > 0, () => MovePin(index, -1)));
        buttons.Children.Add(PinButton("\uE70D", "下移", title, "down", index < count - 1, () => MovePin(index, 1)));
        buttons.Children.Add(PinButton("\uE74D", "移除", title, "remove", true, () => RemovePin(index)));
        row.Children.Add(buttons);
        return row;
    }

    private static Button PinButton(string glyph, string verb, string title, string tag, bool enabled, Action click)
    {
        var b = new Button
        {
            Content = new FontIcon { Glyph = glyph, FontSize = 14 },
            Width = 36, Height = 32, Padding = new Thickness(0),
            Style = (Style)Application.Current.Resources["SubtleButtonStyle"],
            IsEnabled = enabled, Tag = tag,
        };
        AutomationProperties.SetName(b, verb + " " + title);
        ToolTipService.SetToolTip(b, verb);
        b.Click += (_, _) => click();
        return b;
    }

    private async Task LoadIconAsync(PinnedApp pin, Grid host, FontIcon fallback)
    {
        var name = ShellIcons.ParsingName(pin);
        if (!_iconCache.TryGetValue(name, out var source))
        {
            var img = await ShellIcons.LoadAsync(name, 64); // 32 DIP at up to 200 % scale
            if (img != null)
            {
                var bmp = new WriteableBitmap(img.Width, img.Height);
                using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(img.Bgra, 0, img.Bgra.Length);
                bmp.Invalidate();
                source = bmp;
            }
            _iconCache[name] = source;
        }
        if (source == null) return;
        host.Children.Remove(fallback);
        host.Children.Add(new Image { Source = source, Width = 32, Height = 32 });
    }

    private void MovePin(int index, int delta)
    {
        var pins = _current.Pins;
        int to = index + delta;
        if (pins == null || index < 0 || index >= pins.Count || to < 0 || to >= pins.Count) return;
        (pins[index], pins[to]) = (pins[to], pins[index]);
        RebuildPins(to, delta < 0 ? "up" : "down");
        Changed();
    }

    private void RemovePin(int index)
    {
        var pins = _current.Pins;
        if (pins == null || index < 0 || index >= pins.Count) return;
        pins.RemoveAt(index);
        RebuildPins(index, "remove");
        Changed();
    }

    private async void OnAddPin(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.ComputerFolder };
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        picker.FileTypeFilter.Add(".exe");
        picker.FileTypeFilter.Add(".lnk");
        var file = await picker.PickSingleFileAsync();
        if (file == null) return;
        var pins = _current.Pins ??= new List<PinnedApp>();
        if (pins.Count >= 64)
        {
            Status.Severity = InfoBarSeverity.Warning;
            Status.Title = "固定的应用已达上限";
            Status.Message = "最多可以固定 64 个应用。";
            return;
        }
        pins.Add(new PinnedApp(System.IO.Path.GetFileNameWithoutExtension(file.Path), file.Path, "", ""));
        RebuildPins(pins.Count - 1, "remove");
        Changed();
    }

    private async void OnImportPins(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = Content.XamlRoot,
            Title = "导入系统任务栏固定项？",
            Content = "当前的固定列表将被 Windows 任务栏上固定的应用替换。",
            PrimaryButtonText = "替换",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        _current.Pins = CoreClient.ImportTaskbarPins();
        RebuildPins();
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
