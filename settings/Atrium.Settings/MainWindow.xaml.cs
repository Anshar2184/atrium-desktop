using System.Diagnostics;
using System.Numerics;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Atrium.Settings;

public sealed partial class MainWindow : Window
{
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);

    // One slider card: setting key, UI range, and the mapping between the stored
    // value and the slider value (percentages are stored as 0..1).
    private sealed record SliderDef(string Title, string Description, double Min, double Max, double Step, string Unit,
        string MinCaption, string MaxCaption, Func<double, string> Hint, Func<Illustration> Glyph,
        Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private SliderDef[] MaterialSliders => new SliderDef[]
    {
        new("透明度", "玻璃混入多少染色。100% 如清水般通透，0% 像一块纯色亚克力。", 0, 100, 1, "%", "纯色", "通透",
            v => v >= 90 ? "几乎无色" : v >= 70 ? "清透" : v >= 40 ? "淡淡的染色" : v >= 15 ? "浓郁的染色" : "接近纯色",
            Illustrations.Transparency, s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("模糊", "背景被磨砂的程度。0% 依然清晰可辨，100% 只剩色块，像隔着一层磨砂玻璃。", 0, 100, 1, "%", "清晰", "磨砂",
            v => v < 1 ? "完全清晰" : v < 25 ? "微微朦胧" : v < 60 ? "柔和" : v < 85 ? "磨砂" : "深度磨砂",
            Illustrations.Blur, s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("折射强度", "边缘把背景“弯折”的距离，像水滴边缘放大背后的景物。0 就是一块平板玻璃。", 0, 60, 1, "px", "0", "60 px",
            v => v < 1 ? "平板玻璃，无折射" : v <= 12 ? "轻微弯折" : v <= 36 ? "水滴般的边缘" : "夸张的凸透镜",
            Illustrations.Refraction, s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("折射边缘宽度", "折射发生的范围：从外沿向内多少像素。窄像薄片，宽像厚实的凸透镜。", 1, 80, 1, "px", "1 px", "80 px",
            v => v <= 8 ? "窄边，薄如刀片" : v <= 28 ? "适中的圆润边缘" : "宽厚的透镜边",
            Illustrations.Edge, s => s.Edge, (s, v) => s.Edge = v, true),
        new("色散", "边缘处红、绿、蓝分离的程度，像棱镜折出的彩虹边。0% 为纯净无色。", 0, 50, 1, "%", "0%", "50%",
            v => v < 1 ? "无色散，纯净" : v <= 10 ? "几乎看不出" : v <= 30 ? "细腻的彩虹边" : "明显的棱镜彩边",
            Illustrations.Dispersion, s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        new("高光", "边缘受光的反光强度，就像玻璃边沿被灯照亮的那条亮线。", 0, 100, 1, "%", "无", "强烈",
            v => v < 1 ? "没有反光" : v < 35 ? "含蓄" : v < 75 ? "清晰的亮边" : "强烈的镜面光",
            Illustrations.Highlight, s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("阴影", "玻璃下方投影的浓度。越浓，玻璃栏看起来离桌面越“悬浮”。", 0, 100, 1, "%", "无", "浓",
            v => v < 1 ? "没有投影" : v < 25 ? "若有若无" : v < 55 ? "轻盈悬浮" : "厚重的投影",
            Illustrations.Shadow, s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),
    };

    private SliderDef[] LayoutSliders => new SliderDef[]
    {
        new("圆角", "角的弯曲程度。超过高度的一半时即为胶囊形。", 0, 80, 1, "px", "直角", "80 px",
            v => v < 1 ? "直角" : v >= _current.Height / 2 ? "完整的胶囊形" : v < 12 ? "微圆角" : "圆润",
            Illustrations.Corner, s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("最大宽度", "开启“自动宽度”时，玻璃栏按内容收缩，最宽不超过此值；不会超过屏幕工作区。", 300, 2400, 10, "px", "300 px", "2400 px",
            v => v >= 1920 ? "宽于一块 1080p 屏幕" : $"约占 1080p 屏幕宽度的 {Math.Round(v / 19.2)}%",
            Illustrations.Width, s => s.Width, (s, v) => s.Width = v, false),
        new("高度", "玻璃栏的整体厚度，图标的留白与时钟的字号也随之变化。", 32, 120, 1, "px", "32 px", "120 px",
            v => v < 44 ? "纤薄" : v <= 68 ? "标准" : "厚实",
            Illustrations.Height, s => s.Height, (s, v) => s.Height = v, false),
        new("底部间距", "与工作区底边（系统任务栏上方）的距离。", 0, 200, 1, "px", "贴底", "200 px",
            v => v < 1 ? "贴着底边" : v <= 16 ? "贴近底边" : v <= 60 ? "悬浮" : "高高悬浮",
            Illustrations.Margin, s => s.Margin, (s, v) => s.Margin = v, false),
        new("图标大小", "任务栏图标的边长。系统任务栏的图标是 24 px，小一点会让玻璃栏更轻盈。", 16, 64, 1, "px", "16 px", "64 px",
            v => v < 24 ? "比系统图标更小" : v < 25 ? "与系统任务栏相同" : v <= 36 ? "略大于系统图标" : "大图标",
            Illustrations.IconSize, s => s.IconSize, (s, v) => s.IconSize = v, false),
        new("悬停放大", "鼠标经过时，指针下的图标放大到这个倍数，两侧的图标像波浪一样依次变大，玻璃栏也随之伸展。100% 即关闭。", 100, 200, 5, "%", "关闭", "200%",
            v => v < 101 ? "关闭，悬停时只亮起底板" : v < 140 ? "轻轻放大" : v < 180 ? "像 macOS 程序坞" : "夸张的放大",
            Illustrations.Magnify, s => s.Magnify * 100, (s, v) => s.Magnify = v / 100, false),
    };

    private SliderDef[] NotifySliders => new SliderDef[]
    {
        new("通知停留时间", "新通知弹出后停留多久；鼠标停在卡片上时不会消失。", 3, 30, 1, "s", "3 s", "30 s",
            v => v <= 4 ? "一闪而过" : v <= 10 ? "适中" : "从容阅读",
            Illustrations.NotifyTimeout, s => s.NotifyTimeout, (s, v) => s.NotifyTimeout = v, false),
    };

    private static readonly (string Overline, string Title, string Subtitle)[] PageInfo =
    {
        ("01 · MATERIAL", "材质", "玻璃的质感：通透、模糊、折射与光。"),
        ("02 · SHAPE", "形状", "玻璃栏的轮廓与位置：圆角、尺寸与间距。"),
        ("03 · PINNED", "固定的应用", "拖动卡片排序，决定它们在玻璃栏上的先后。"),
        ("04 · CONTENT", "内容", "栏上显示什么、文字如何适应背景，以及通知。"),
        ("05 · ABOUT", "关于", "一条悬浮在桌面底部的玻璃任务栏。"),
    };

    private readonly CoreClient _client = new();
    private readonly List<(SliderDef Def, SliderCard Card)> _rows = new();
    private readonly List<(uint Rgb, Button Button, FrameworkElement Ring)> _tintButtons = new();
    private BarSettings _saved = new();
    private BarSettings _current = new();
    private bool _loading;
    private bool _previewBusy, _previewPending;
    private bool _closingConfirmed;
    private FrameworkElement[] _pages = Array.Empty<FrameworkElement>();
    private Action<Color>? _setTintGlyph;
    private Illustration? _autoWidthGlyph, _clockGlyph, _runningGlyph;
    private int _flashId;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "atrium.ico"));
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new Windows.Graphics.SizeInt32((int)(1120 * scale), (int)(780 * scale)));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(900 * scale);
            presenter.PreferredMinimumHeight = (int)(640 * scale);
        }
        AppWindow.Closing += OnClosing;

        _pages = new FrameworkElement[] { PageMaterial, PageShape, PagePins, PageContent, PageAbout };
        BuildCards();
        InitPins();
        InitHero();
        PathText.Text = CoreClient.SettingsPath;
        var v = typeof(MainWindow).Assembly.GetName().Version;
        VersionText.Text = "版本 " + (v?.ToString(3) ?? "0.2.0");
        ShowStatus(false);
        Nav.SelectedIndex = 0;
        _ = ConnectAndLoadAsync();
    }

    // ---- Pages ----

    private void OnNavChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Nav.SelectedIndex >= 0) ShowPage(Nav.SelectedIndex);
    }

    private void ShowPage(int index)
    {
        for (int i = 0; i < _pages.Length; i++)
            _pages[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
        var info = PageInfo[index];
        HeaderOverline.Text = info.Overline;
        HeaderTitle.Text = info.Title;
        HeaderSubtitle.Text = info.Subtitle;
        HeroFrame.Visibility = index < 2 ? Visibility.Visible : Visibility.Collapsed;
        if (index < 2) UpdateHero();
        AnimateIn(_pages[index]);
        AnimateIn(Header);
    }

    // Fade and rise a few pixels; the transitions are attached one tick later so the start values stick.
    private void AnimateIn(UIElement e)
    {
        e.OpacityTransition = null;
        e.TranslationTransition = null;
        e.Opacity = 0;
        e.Translation = new Vector3(0, 10, 0);
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(240) };
            e.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(300) };
            e.Opacity = 1;
            e.Translation = Vector3.Zero;
        });
    }

    // ---- Cards ----

    private void BuildCards()
    {
        foreach (var d in MaterialSliders)
            MaterialGrid.Children.Insert(MaterialGrid.Children.Count - 1, CreateCard(d));
        // The auto-width card sits right after the width card.
        var layout = LayoutSliders;
        for (int i = 0; i < layout.Length; i++)
            LayoutGrid.Children.Insert(i < 2 ? i : i + 1, CreateCard(layout[i]));
        foreach (var d in NotifySliders)
            NotifyGrid.Children.Insert(0, CreateCard(d));

        var tint = Illustrations.Tint();
        TintGlyphHost.Child = tint.Root;
        _setTintGlyph = tint.SetColor;
        BuildTintSwatches();

        _autoWidthGlyph = Illustrations.AutoWidth();
        AutoWidthGlyphHost.Child = _autoWidthGlyph.Root;
        _clockGlyph = Illustrations.ClockToggle();
        ClockGlyphHost.Child = _clockGlyph.Root;
        _runningGlyph = Illustrations.RunningToggle();
        RunningGlyphHost.Child = _runningGlyph.Root;

        AutoWidthCard.GotFocus += (_, _) => OnEngage("最大宽度", true);
        AutoWidthCard.LostFocus += (_, _) => OnEngage("最大宽度", false);
    }

    private SliderCard CreateCard(SliderDef d)
    {
        var card = new SliderCard();
        var glyph = d.Glyph();
        card.Configure(d.Title, d.Description, d.Min, d.Max, d.Step, d.Unit, d.MinCaption, d.MaxCaption, d.Hint, glyph.Root, glyph.Update);
        card.UserValueChanged += v => OnSliderChanged(d, v);
        card.Engaged += on => OnEngage(d.Title, on);
        _rows.Add((d, card));
        return card;
    }

    private static readonly (string Name, uint Rgb)[] TintChoices =
    {
        ("白色", 0xFFFFFF), ("暖白", 0xFFE6C8), ("冷蓝", 0xD3E6FF), ("玫瑰", 0xFFD0DC), ("薄荷", 0xCCF4E0), ("石墨", 0x2E3138),
    };

    private void BuildTintSwatches()
    {
        foreach (var (name, rgb) in TintChoices)
        {
            var ring = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 30, Height = 30, Style = (Style)Application.Current.Resources["GlyphAccentStyle"], StrokeThickness = 2, Opacity = 0 };
            ring.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
            var dot = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = 22, Height = 22, Fill = new SolidColorBrush(TintColor(rgb)),
                Stroke = new SolidColorBrush(Color.FromArgb(48, 128, 128, 128)), StrokeThickness = 1,
            };
            var button = new Button
            {
                Content = new Grid { Width = 30, Height = 30, Children = { dot, ring } },
                Width = 36, Height = 36, Padding = new Thickness(0), CornerRadius = new CornerRadius(18),
                Style = (Style)Application.Current.Resources["SubtleButtonStyle"],
            };
            AutomationProperties.SetName(button, "染色 " + name);
            ToolTipService.SetToolTip(button, name);
            var captured = rgb;
            button.Click += (_, _) => SetTint(captured);
            _tintButtons.Add((rgb, button, ring));
            TintSwatches.Children.Add(button);
        }
    }

    private void SetTint(uint rgb)
    {
        _current.Tint = rgb;
        MarkCustom();
        var c = TintColor(rgb);
        _loading = true;
        TintPicker.Color = c;
        _loading = false;
        ShowTint();
        Changed();
    }

    private void ShowTint()
    {
        var c = TintColor(_current.Tint);
        TintSwatch.Background = new SolidColorBrush(c);
        TintHex.Text = "#" + _current.Tint.ToString("X6", System.Globalization.CultureInfo.InvariantCulture);
        _setTintGlyph?.Invoke(c);
        foreach (var (rgb, _, ring) in _tintButtons) ring.Opacity = rgb == _current.Tint ? 1 : 0;
    }

    // ---- Connection ----

    private async Task ConnectAndLoadAsync()
    {
        // ATRIUM_SETTINGS_OFFLINE=1: never talk to a running core (development aid).
        bool connected = Environment.GetEnvironmentVariable("ATRIUM_SETTINGS_OFFLINE") != "1" && await _client.ConnectAsync(800);
        var (saved, current) = await _client.GetAsync();
        _saved = saved;
        _current = current;
        ShowStatus(connected);
        LoadIntoControls();
        UpdateButtons();
    }

    private static readonly SolidColorBrush RunningBrush = new(Color.FromArgb(255, 76, 194, 106));
    private static readonly SolidColorBrush IdleBrush = new(Color.FromArgb(255, 150, 150, 156));

    private void ShowStatus(bool connected)
    {
        if (connected)
        {
            StatusDot.Fill = RunningBrush;
            StatusTitle.Text = "Atrium 正在运行";
            StatusMessage.Text = "修改会实时显示在玻璃栏上。";
            StartCoreButton.Visibility = Visibility.Collapsed;
        }
        else
        {
            StatusDot.Fill = IdleBrush;
            StatusTitle.Text = "Atrium 未运行";
            StatusMessage.Text = "可以继续修改并保存，下次启动时生效。";
            StartCoreButton.Visibility = CoreExe() != null ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private void ShowMessage(InfoBarSeverity severity, string title, string message)
    {
        Status.Severity = severity;
        Status.Title = title;
        Status.Message = message;
        Status.IsOpen = true;
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

    // ---- Controls <-> settings ----

    private void LoadIntoControls()
    {
        _loading = true;
        foreach (var (d, card) in _rows)
            card.SetValue(d.Get(_current));
        (_current.Preset switch { "liquid" => PresetLiquid, "frosted" => PresetFrosted, _ => PresetCustom }).IsChecked = true;
        ClockSwitch.IsOn = _current.ShowClock;
        AutoWidthSwitch.IsOn = _current.AutoWidth;
        RunningSwitch.IsOn = _current.ShowRunning;
        HideBodySwitch.IsOn = _current.NotifyHideBody;
        ListenerSwitch.IsOn = _current.NotifyListener;
        WeChatSwitch.IsOn = _current.NotifyWeChat;
        FeishuSwitch.IsOn = _current.NotifyFeishu;
        WidgetsSwitch.IsOn = _current.Widgets;
        _autoWidthGlyph?.Update(_current.AutoWidth ? 1 : 0);
        _clockGlyph?.Update(_current.ShowClock ? 1 : 0);
        _runningGlyph?.Update(_current.ShowRunning ? 1 : 0);
        TextColorBox.SelectedIndex = _current.TextColor switch { "light" => 1, "dark" => 2, _ => 0 };
        ShowTextColor();
        RebuildPins();
        TintPicker.Color = TintColor(_current.Tint);
        ShowTint();
        _loading = false;
        UpdateHero();
        UpdateCustomSwatch();
    }

    private static Color TintColor(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    private void OnSliderChanged(SliderDef d, double v)
    {
        if (_loading) return;
        d.Set(_current, v);
        if (d.Material) MarkCustom();
        Changed();
    }

    private void MarkCustom()
    {
        _current.Preset = "custom";
        bool was = _loading;
        _loading = true;
        PresetCustom.IsChecked = true;
        _loading = was;
    }

    private void OnPresetChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        if (tag == "liquid") _current.ApplyMaterial(BarSettings.Liquid());
        else if (tag == "frosted") _current.ApplyMaterial(BarSettings.Frosted());
        else _current.Preset = "custom";
        LoadIntoControls();
        Changed();
    }

    private void OnTintChanged(ColorPicker sender, ColorChangedEventArgs args)
    {
        if (_loading) return;
        _current.Tint = ((uint)args.NewColor.R << 16) | ((uint)args.NewColor.G << 8) | args.NewColor.B;
        MarkCustom();
        ShowTint();
        Changed();
    }

    private void OnClockToggled(object sender, RoutedEventArgs e)
    {
        _clockGlyph?.Update(ClockSwitch.IsOn ? 1 : 0);
        if (_loading) return;
        _current.ShowClock = ClockSwitch.IsOn;
        Changed();
    }

    private void OnAutoWidthToggled(object sender, RoutedEventArgs e)
    {
        _autoWidthGlyph?.Update(AutoWidthSwitch.IsOn ? 1 : 0);
        if (_loading) return;
        _current.AutoWidth = AutoWidthSwitch.IsOn;
        Changed();
    }

    private void OnRunningToggled(object sender, RoutedEventArgs e)
    {
        _runningGlyph?.Update(RunningSwitch.IsOn ? 1 : 0);
        if (_loading) return;
        _current.ShowRunning = RunningSwitch.IsOn;
        Changed();
    }

    private void OnHideBodyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyHideBody = HideBodySwitch.IsOn;
        Changed();
    }

    private void OnListenerToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyListener = ListenerSwitch.IsOn;
        Changed();
    }

    private void OnFeishuToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyFeishu = FeishuSwitch.IsOn;
        Changed();
    }

    private void OnWidgetsToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.Widgets = WidgetsSwitch.IsOn;
        Changed();
    }

    private void OnWeChatToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyWeChat = WeChatSwitch.IsOn;
        Changed();
    }

    private void OnTextColorChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || TextColorBox.SelectedItem is not ComboBoxItem { Tag: string tag }) return;
        _current.TextColor = tag;
        ShowTextColor();
        Changed();
    }

    // The two sample chips mark which text colours the current choice allows.
    private void ShowTextColor()
    {
        var accent = (Brush)Application.Current.Resources["AccentFillColorDefaultBrush"];
        var none = new SolidColorBrush(Colors.Transparent);
        ChipLight.BorderBrush = _current.TextColor is "light" or "auto" ? accent : none;
        ChipDark.BorderBrush = _current.TextColor is "dark" or "auto" ? accent : none;
        ChipLight.Opacity = _current.TextColor == "dark" ? 0.45 : 1;
        ChipDark.Opacity = _current.TextColor == "light" ? 0.45 : 1;
    }

    private void Changed()
    {
        UpdateButtons();
        UpdateHero();
        SchedulePreview();
    }

    private void UpdateButtons()
    {
        bool dirty = !_current.SameAs(_saved);
        SaveButton.IsEnabled = dirty;
        RevertButton.IsEnabled = dirty;
        _flashId++;
        DirtyText.Text = "未保存的修改";
        DirtyDot.Fill = (Brush)Application.Current.Resources["SystemFillColorCautionBrush"];
        DirtyLabel.Opacity = dirty ? 1 : 0;
    }

    // A short "saved" confirmation in the footer.
    private async void FlashSaved()
    {
        int id = ++_flashId;
        DirtyText.Text = "已保存";
        DirtyDot.Fill = (Brush)Application.Current.Resources["SystemFillColorSuccessBrush"];
        DirtyLabel.Opacity = 1;
        await Task.Delay(1800);
        if (id == _flashId) DirtyLabel.Opacity = 0;
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
            ShowMessage(InfoBarSeverity.Error, "保存失败", error);
            return false;
        }
        _saved = _current.Clone();
        Status.IsOpen = false;
        UpdateButtons();
        FlashSaved();
        ShowStatus(_client.Connected);
        return true;
    }

    private async void OnRevert(object sender, RoutedEventArgs e)
    {
        _current = _saved.Clone();
        Status.IsOpen = false;
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
