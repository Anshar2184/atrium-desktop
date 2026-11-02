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

    // One slider row: its group, UI range, and the mapping between the stored
    // value and the slider value (percentages are stored as 0..1).
    private sealed record SliderDef(string Group, string Title, string Description, double Min, double Max, double Step, string Unit,
        Func<double, string> Hint, Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private SliderDef[] Sliders => new SliderDef[]
    {
        new("material", "透明度", "玻璃混入多少染色。100% 如清水般通透，0% 像一块纯色亚克力。", 0, 100, 1, "%",
            v => v >= 90 ? "几乎无色" : v >= 70 ? "清透" : v >= 40 ? "淡淡的染色" : v >= 15 ? "浓郁的染色" : "接近纯色",
            s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("material", "模糊", "背景被磨砂的程度。0% 依然清晰可辨，100% 只剩色块。", 0, 100, 1, "%",
            v => v < 1 ? "完全清晰" : v < 25 ? "微微朦胧" : v < 60 ? "柔和" : v < 85 ? "磨砂" : "深度磨砂",
            s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("material", "折射强度", "边缘把背景“弯折”的距离，像水滴边缘放大背后的景物。0 就是一块平板玻璃。", 0, 60, 1, "px",
            v => v < 1 ? "平板玻璃" : v <= 12 ? "轻微弯折" : v <= 36 ? "水滴般的边缘" : "夸张的凸透镜",
            s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("material", "折射边缘宽度", "折射发生的范围：从外沿向内多少像素。窄像薄片，宽像厚实的凸透镜。", 1, 80, 1, "px",
            v => v <= 8 ? "薄如刀片" : v <= 28 ? "圆润" : "宽厚的透镜边",
            s => s.Edge, (s, v) => s.Edge = v, true),
        new("material", "色散", "边缘处红、绿、蓝分离的程度，像棱镜折出的彩虹边。", 0, 50, 1, "%",
            v => v < 1 ? "纯净" : v <= 10 ? "几乎看不出" : v <= 30 ? "细腻的彩虹边" : "明显的棱镜彩边",
            s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        new("material", "高光", "玻璃边沿被光照亮的那条亮线。", 0, 100, 1, "%",
            v => v < 1 ? "没有反光" : v < 35 ? "含蓄" : v < 75 ? "清晰的亮边" : "强烈的镜面光",
            s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("material", "阴影", "玻璃下方投影的浓度。越浓，玻璃栏看起来离桌面越“悬浮”。", 0, 100, 1, "%",
            v => v < 1 ? "没有投影" : v < 25 ? "若有若无" : v < 55 ? "轻盈悬浮" : "厚重的投影",
            s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),

        new("size", "高度", "玻璃栏的整体厚度，图标的留白与时钟的字号也随之变化。", 32, 120, 1, "px",
            v => v < 44 ? "纤薄" : v <= 68 ? "标准" : "厚实",
            s => s.Height, (s, v) => s.Height = v, false),
        new("size", "图标大小", "任务栏图标的边长。系统任务栏的图标是 24 px。", 16, 64, 1, "px",
            v => v < 24 ? "比系统图标小" : v < 25 ? "与系统相同" : v <= 36 ? "略大" : "大图标",
            s => s.IconSize, (s, v) => s.IconSize = v, false),
        new("size", "圆角", "角的弯曲程度。超过高度的一半时即为胶囊形。", 0, 80, 1, "px",
            v => v < 1 ? "直角" : v >= _current.Height / 2 ? "胶囊形" : v < 12 ? "微圆角" : "圆润",
            s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("width", "最大宽度", "开启“自动宽度”时最宽不超过此值；不会超过屏幕工作区。", 300, 2400, 10, "px",
            v => v >= 1920 ? "宽于 1080p 屏幕" : $"约 1080p 宽度的 {Math.Round(v / 19.2)}%",
            s => s.Width, (s, v) => s.Width = v, false),
        new("motion", "底部间距", "与工作区底边的距离。", 0, 200, 1, "px",
            v => v < 1 ? "贴着底边" : v <= 16 ? "贴近底边" : v <= 60 ? "悬浮" : "高高悬浮",
            s => s.Margin, (s, v) => s.Margin = v, false),
        new("motion", "悬停放大", "指针下的图标放大到这个倍数，两侧依次变大，玻璃栏随之伸展。100% 即关闭。", 100, 200, 5, "%",
            v => v < 101 ? "关闭" : v < 140 ? "轻轻放大" : v < 180 ? "像 macOS 程序坞" : "夸张",
            s => s.Magnify * 100, (s, v) => s.Magnify = v / 100, false),

        new("notify", "停留时间", "新通知弹出后停留多久；指针停在卡片上时不会消失。", 3, 30, 1, "s",
            v => v <= 4 ? "一闪而过" : v <= 10 ? "适中" : "从容阅读",
            s => s.NotifyTimeout, (s, v) => s.NotifyTimeout = v, false),
    };

    private static readonly (string Title, string Subtitle)[] PageInfo =
    {
        ("材质", "玻璃的质感：通透、模糊、折射与光。"),
        ("形状", "玻璃栏的尺寸、轮廓与位置。"),
        ("程序坞", "栏上显示什么，以及固定的应用和它们的顺序。"),
        ("通知", "通知卡片怎样弹出，以及显示哪些应用的消息。"),
        ("小组件", "放在桌面上的玻璃小组件。"),
        ("系统", "Atrium 与 Windows 自带界面的分工。"),
        ("设计规范", "排版、圆角、动效与尺寸：每个数值都取自 Atrium 的代码。"),
        ("关于", "一条悬浮在桌面底部的玻璃程序坞。"),
    };

    private readonly CoreClient _client = new();
    private readonly List<(SliderDef Def, SliderRow Row)> _rows = new();
    private readonly List<(uint Rgb, Button Button, FrameworkElement Ring)> _tintButtons = new();
    private BarSettings _saved = new();
    private BarSettings _current = new();
    private bool _loading;
    private bool _previewBusy, _previewPending;
    private bool _closingConfirmed;
    private FrameworkElement[] _pages = Array.Empty<FrameworkElement>();
    private int _flashId;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "atrium.ico"));
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new Windows.Graphics.SizeInt32((int)(1280 * scale), (int)(860 * scale)));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(980 * scale);
            presenter.PreferredMinimumHeight = (int)(680 * scale);
        }
        AppWindow.Closing += OnClosing;

        _pages = new FrameworkElement[] { PageMaterial, PageShape, PageDock, PageNotify, PageWidgets, PageSystem, PageDesign, PageAbout };
        BuildRows();
        BuildTintSwatches();
        InitPins();
        InitHero();
        PathText.Text = CoreClient.SettingsPath;
        var v = typeof(MainWindow).Assembly.GetName().Version;
        VersionText.Text = "版本 " + (v?.ToString(3) ?? "0.9");
        ShowStatus(false);
        // `--page widgets` (the widgets' menu): open on that page.
        var args = Environment.GetCommandLineArgs();
        int page = Array.IndexOf(args, "--page") is int i && i >= 0 && i + 1 < args.Length && args[i + 1] == "widgets" ? 4 : 0;
        Nav.SelectedIndex = page;
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
        if (_pages[index] == PageDesign) BuildDesignPage();
        if (_pages[index] == PageWidgets && WidgetGallery.Children.Count == 0) BuildWidgetGallery(); // built when first shown
        var info = PageInfo[index];
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

    // ---- Rows ----

    // Every slider goes into its group's card, rows divided by hairlines.
    private void BuildRows()
    {
        var groups = new Dictionary<string, Panel>
        {
            ["material"] = MaterialRows, ["size"] = ShapeSizeRows, ["width"] = ShapeWidthRows, ["motion"] = ShapeMotionRows, ["notify"] = NotifyRows,
        };
        foreach (var d in Sliders)
        {
            var panel = groups[d.Group];
            var row = new SliderRow();
            row.Configure(d.Title, d.Description, d.Min, d.Max, d.Step, d.Unit, d.Hint);
            row.UserValueChanged += v => OnSliderChanged(d, v);
            row.Engaged += on => OnEngage(d.Title, on);
            _rows.Add((d, row));
            var line = new Microsoft.UI.Xaml.Shapes.Rectangle { Style = (Style)Application.Current.Resources["SeparatorStyle"] };
            if (d.Group == "notify")
            {
                // Above the group's toggle.
                panel.Children.Insert(0, line);
                panel.Children.Insert(0, row);
                continue;
            }
            if (panel.Children.Count > 0) panel.Children.Add(line);
            panel.Children.Add(row);
        }
        AutoWidthCard.GotFocus += (_, _) => OnEngage("最大宽度", true);
        AutoWidthCard.LostFocus += (_, _) => OnEngage("最大宽度", false);
    }

    private static readonly (string Name, uint Rgb)[] TintChoices =
    {
        ("白色", 0xFFFFFF), ("暖白", 0xFFE6C8), ("冷蓝", 0xD3E6FF), ("玫瑰", 0xFFD0DC), ("薄荷", 0xCCF4E0), ("石墨", 0x2E3138),
    };

    private void BuildTintSwatches()
    {
        foreach (var (name, rgb) in TintChoices)
        {
            var ring = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 30, Height = 30, Stroke = Tokens.Brush("AccentBrush"), StrokeThickness = 2, Opacity = 0 };
            ring.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
            var dot = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = 24, Height = 24, Fill = new SolidColorBrush(TintColor(rgb)), Stroke = Tokens.Brush("CardStrokeBrush"), StrokeThickness = 1,
            };
            var button = new Button
            {
                Content = new Grid { Width = 30, Height = 30, Children = { dot, ring } },
                Style = (Style)Application.Current.Resources["IconButtonStyle"], Width = 36, Height = 36, CornerRadius = new CornerRadius(18),
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
        _loading = true;
        TintPicker.Color = TintColor(rgb);
        _loading = false;
        ShowTint();
        Changed();
    }

    private void ShowTint()
    {
        var c = TintColor(_current.Tint);
        TintSwatch.Fill = new SolidColorBrush(c);
        TintHex.Text = "#" + _current.Tint.ToString("X6", System.Globalization.CultureInfo.InvariantCulture);
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
        HideTaskbarSwitch.IsOn = _current.HideTaskbar;
        (_current.WidgetBackdrop == "wallpaper" ? BackdropWallpaper : BackdropDesktop).IsChecked = true;
        HideBannersSwitch.IsOn = _current.HideBanners;
        (_current.TextColor switch { "light" => TextLight, "dark" => TextDark, _ => TextAuto }).IsChecked = true;
        UpdateGallery();
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
        UpdateGallery();
        if (_loading) return;
        _current.Widgets = WidgetsSwitch.IsOn;
        Changed();
    }

    private void OnWidgetBackdropChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.WidgetBackdrop = tag;
        Changed();
    }

    private void OnHideTaskbarToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HideTaskbar = HideTaskbarSwitch.IsOn;
        Changed();
    }

    private void OnHideBannersToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HideBanners = HideBannersSwitch.IsOn;
        Changed();
    }

    private void OnWeChatToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyWeChat = WeChatSwitch.IsOn;
        Changed();
    }

    private void OnTextColorChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.TextColor = tag;
        Changed();
    }

    private void Changed()
    {
        UpdateButtons();
        UpdateHero();
        SchedulePreview();
    }

    // The floating capsule: shown while there is something to save, and for a moment after a save.
    private void UpdateButtons()
    {
        bool dirty = !_current.SameAs(_saved);
        SaveButton.IsEnabled = dirty;
        RevertButton.IsEnabled = dirty;
        _flashId++;
        DirtyText.Text = "未保存的修改";
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0xFF, 0x9F, 0x0A));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Visible;
        ShowSaveBar(dirty);
    }

    private void ShowSaveBar(bool on)
    {
        if (SaveBar.OpacityTransition == null)
        {
            SaveBar.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(200) };
            SaveBar.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(320) };
        }
        SaveBar.Opacity = on ? 1 : 0;
        SaveBar.Translation = new Vector3(0, on ? 0 : 24, 32);
        SaveBar.IsHitTestVisible = on;
    }

    private async void FlashSaved()
    {
        int id = ++_flashId;
        DirtyText.Text = "已保存";
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0x30, 0xD1, 0x58));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Collapsed;
        ShowSaveBar(true);
        await Task.Delay(1600);
        if (id == _flashId) ShowSaveBar(false);
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
        // What is saved is this snapshot: edits made while it is on its way stay unsaved.
        var sent = _current.Clone();
        var error = await _client.CommitAsync(sent.Clone());
        if (error != null)
        {
            ShowMessage(InfoBarSeverity.Error, "保存失败", error);
            return false;
        }
        _saved = sent;
        Status.IsOpen = false;
        UpdateButtons();
        if (_current.SameAs(_saved)) FlashSaved(); // edits made meanwhile keep the capsule as it is
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
