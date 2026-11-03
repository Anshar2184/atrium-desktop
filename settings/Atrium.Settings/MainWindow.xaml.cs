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

    // One slider tile: its group, UI range, and the mapping between the stored
    // value and the slider value (percentages are stored as 0..1).
    private sealed record SliderDef(string Group, string Title, double Min, double Max, double Step, string Unit,
        Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private SliderDef[] Sliders => new SliderDef[]
    {
        new("clarity", "透明度", 0, 100, 1, "%",
            s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("clarity", "模糊", 0, 100, 1, "%",
            s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("edge", "折射强度", 0, 60, 1, "px",
            s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("edge", "折射边缘宽度", 1, 80, 1, "px",
            s => s.Edge, (s, v) => s.Edge = v, true),
        new("edge", "色散", 0, 50, 1, "%",
            s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        new("light", "高光", 0, 100, 1, "%",
            s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("light", "阴影", 0, 100, 1, "%",
            s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),

        new("size", "高度", 32, 120, 1, "px",
            s => s.Height, (s, v) => s.Height = v, false),
        new("size", "圆角", 0, 80, 1, "px",
            s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("dock", "图标大小", 16, 64, 1, "px",
            s => s.IconSize, (s, v) => s.IconSize = v, false),
        new("width", "最大宽度", 300, 2400, 10, "px",
            s => s.Width, (s, v) => s.Width = v, false),
        new("dock", "底部间距", 0, 200, 1, "px",
            s => s.Margin, (s, v) => s.Margin = v, false),
        new("dock", "悬停放大", 100, 200, 5, "%",
            s => s.Magnify * 100, (s, v) => s.Magnify = v / 100, false),

        new("notify", "停留时间", 3, 30, 1, "s",
            s => s.NotifyTimeout, (s, v) => s.NotifyTimeout = v, false),
    };

    private static readonly string[] PageTitles =
    {
        "材质", "形状", "任务栏", "通知", "小组件", "系统", "设计语言", "关于",
    };
    private readonly CoreClient _client = new();
    private readonly List<(SliderDef Def, SliderTile Tile)> _rows = new();
    private readonly List<(uint Rgb, Button Button, FrameworkElement Ring)> _tintButtons = new();
    private BarSettings _saved = new();
    private BarSettings _current = new();
    private bool _loading;
    private bool _previewBusy, _previewPending;
    private bool _closingConfirmed;
    private FrameworkElement[] _mains = Array.Empty<FrameworkElement>();
    private FrameworkElement?[] _sides = Array.Empty<FrameworkElement?>();
    private int _page;
    private bool _folded; // a narrow window: the inspector's groups under the workspace
    private int _flashId;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "atrium.ico"));
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        // A roomy default (the preview is the centre of the window), never larger than the screen.
        var work = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        int width = Math.Min((int)(1440 * scale), work.Width * 92 / 100), height = Math.Min((int)(920 * scale), work.Height * 92 / 100);
        AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2, width, height));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(980 * scale);
            presenter.PreferredMinimumHeight = (int)(680 * scale);
        }
        AppWindow.Closing += OnClosing;
        Closed += (_, _) =>
        {
            _widgetWatcher?.Dispose();
            StopDesignAnimations();
            _renderWorker?.Stop();
            _swatchWorker?.Stop();
            StopTiles();
        };

        _mains = new FrameworkElement[] { MainMaterial, MainShape, MainDock, MainNotify, MainWidgets, MainSystem, DesignStack, MainAbout };
        _sides = new FrameworkElement?[] { SideMaterial, SideShape, SideDock, SideNotify, SideWidgets, null, null, null };
        LoupeSwitch.IsOn = true;
        BuildRows();
        BuildTintSwatches();
        InitPins();
        InitHero();
        PhotoChanged += () =>
        {
            if (_page == 4) { ShowWidgetMap(); RefreshGallery(); }
            if (_page == 3) ShowNotifyPreview(false);
        };
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

    // A page is its workspace part (under the title, with the live preview on the first three) and its
    // inspector part; pages without parameters of their own (system, design, about) take the full width.
    private void ShowPage(int index)
    {
        if (_page == 6 && index != 6) StopDesignAnimations();
        _page = index;
        for (int i = 0; i < _mains.Length; i++)
        {
            _mains[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
            if (_sides[i] is { } side) side.Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_mains[index] == DesignStack)
        {
            BuildDesignPage();
            RefreshDesignDetails(); // the "current" column follows the settings
        }
        if (_mains[index] == MainWidgets)
        {
            if (WidgetGallery.Children.Count == 0) BuildWidgetGallery(); // built when first shown
            else RefreshGallery();
            ShowWidgetMap();
        }
        if (_mains[index] == MainNotify) ShowNotifyPreview(true);
        PageTitle.Text = PageTitles[index];
        HeroBlock.Visibility = index < 3 ? Visibility.Visible : Visibility.Collapsed;
        LoupeToggle.Visibility = index == 0 ? Visibility.Visible : Visibility.Collapsed;
        Loupe.Visibility = index == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        // The design page reads wider than a settings page.
        CenterStack.MaxWidth = _mains[index] == DesignStack ? 1280 : 1120;
        ArrangeInspector();
        CenterScroll.ChangeView(null, 0, null, true);
        InspectorScroll.ChangeView(null, 0, null, true);
        if (index < 3) UpdateHero();
        if (index == 0) UpdateCustomSwatch();
        AnimateIn(_mains[index]);
        if (_sides[index] is { } shown) AnimateIn(shown);
    }

    // The inspector: its own column when there is room for the workspace beside it, else folded
    // under the workspace (the groups move, keeping their state).
    private const double FoldBelow = 900;

    private void OnContentSizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool folded = e.NewSize.Width < FoldBelow;
        if (folded == _folded && _mains.Length > 0) return;
        _folded = folded;
        if (folded)
        {
            InspectorScroll.Content = null;
            InspectorStack.Margin = new Thickness(0, 8, 0, 0);
            FoldedInspector.Child = InspectorStack;
        }
        else
        {
            FoldedInspector.Child = null;
            InspectorStack.ClearValue(FrameworkElement.MarginProperty);
            InspectorScroll.Content = InspectorStack;
        }
        ArrangeInspector();
    }

    private void ArrangeInspector()
    {
        bool has = _sides.Length > _page && _sides[_page] != null;
        InspectorPane.Visibility = has && !_folded ? Visibility.Visible : Visibility.Collapsed;
        FoldedInspector.Visibility = has && _folded ? Visibility.Visible : Visibility.Collapsed;
    }

    // Every page uses the viewport width, including pages with small intrinsic content.
    private void OnWorkspaceSizeChanged(object sender, SizeChangedEventArgs e) =>
        CenterStack.Width = Math.Max(0, e.NewSize.Width - CenterStack.Margin.Left - CenterStack.Margin.Right);

    // The previews keep a natural proportion as the workspace widens.
    private void OnCenterSizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = e.NewSize.Width;
        HeroFrame.Height = Math.Clamp(Math.Round(w * 0.52), 280, 580);
        NotifyStage.Height = Math.Clamp(Math.Round(w * 0.42), 260, 440);
    }

    private void OnLoupeToggled(object sender, RoutedEventArgs e)
    {
        Loupe.Visibility = _page == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        if (Loupe.Visibility == Visibility.Collapsed)
        {
            _loupeBrush.ImageSource = null;
            _loupeBmp = null;
        }
        UpdateHero();
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

    // Every slider is a tile in its group's grid: after the group's other tiles, or before them
    // where the sliders lead (the tint, the notification switch).
    private void BuildRows()
    {
        // The basic three are tiles in the workspace; the rest are plain rows in the inspector.
        var groups = new Dictionary<string, (Panel Panel, bool First, bool Flat)>
        {
            ["clarity"] = (ClarityTiles, true, false), ["edge"] = (EdgeRows, false, true), ["light"] = (LightRows, false, true),
            ["size"] = (SizeRows, false, true), ["width"] = (WidthRows, false, true), ["dock"] = (DockRows, false, true), ["notify"] = (NotifyRows, true, true),
        };
        var placed = new Dictionary<string, int>();
        foreach (var d in Sliders)
        {
            var (panel, first, flat) = groups[d.Group];
            var tile = new SliderTile { Flat = flat };
            tile.Configure(d.Title, d.Min, d.Max, d.Step, d.Unit);
            tile.UserValueChanged += v => OnSliderChanged(d, v);
            tile.Engaged += on => OnEngage(d.Title, on);
            _rows.Add((d, tile));
            int at = first ? placed.GetValueOrDefault(d.Group) : panel.Children.Count;
            panel.Children.Insert(at, tile);
            placed[d.Group] = at + 1;
        }
        AutoWidthSwitch.GotFocus += (_, _) => OnEngage("最大宽度", true);
        AutoWidthSwitch.LostFocus += (_, _) => OnEngage("最大宽度", false);
    }

    private static readonly (string Name, uint Rgb)[] TintChoices =
    {
        ("白色", 0xFFFFFF), ("暖白", 0xFFE6C8), ("冷蓝", 0xD3E6FF), ("玫瑰", 0xFFD0DC), ("薄荷", 0xCCF4E0), ("石墨", 0x2E3138),
    };

    private void BuildTintSwatches()
    {
        foreach (var (name, rgb) in TintChoices)
        {
            var ring = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 28, Height = 28, Stroke = Tokens.Brush("AccentBrush"), StrokeThickness = 2, Opacity = 0 };
            ring.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
            var dot = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = 22, Height = 22, Fill = new SolidColorBrush(TintColor(rgb)), Stroke = Tokens.Brush("CardStrokeBrush"), StrokeThickness = 1,
            };
            var button = new Button
            {
                Content = new Grid { Width = 28, Height = 28, Children = { dot, ring } },
                Style = (Style)Application.Current.Resources["IconButtonStyle"], Width = 32, Height = 32, CornerRadius = new CornerRadius(16),
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
        (Photo.Selected switch { "wallpaper" => BgWallpaper, "paper" => BgPaper, "geometry" => BgGeometry, "layers" => BgLayers, _ => BgSpecimen }).IsChecked = true;
        ClockSwitch.IsOn = _current.ShowClock;
        TraySwitch.IsOn = _current.ShowTray;
        StatusSwitch.IsOn = _current.ShowStatus;
        NotificationsSwitch.IsOn = _current.ShowNotifications;
        AutoWidthSwitch.IsOn = _current.AutoWidth;
        RunningSwitch.IsOn = _current.ShowRunning;
        HideBodySwitch.IsOn = _current.NotifyHideBody;
        ListenerSwitch.IsOn = _current.NotifyListener;
        WeChatSwitch.IsOn = _current.NotifyWeChat;
        FeishuSwitch.IsOn = _current.NotifyFeishu;
        WidgetsSwitch.IsOn = _current.Widgets;
        (_current.WidgetsOnRight ? WidgetsRight : WidgetsLeft).IsChecked = true;
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
        if (_page == 3) ShowNotifyPreview(false);
        if (_page == 4) ShowWidgetMap();
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

    private void OnBarContentToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowTray = TraySwitch.IsOn;
        _current.ShowStatus = StatusSwitch.IsOn;
        _current.ShowNotifications = NotificationsSwitch.IsOn;
        Changed();
    }

    private void OnWidgetAlignmentChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.WidgetsOnRight = tag == "right";
        ShowWidgetMap();
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
        UpdateCustomSwatch();
        if (_page == 3) ShowNotifyPreview(false);
        SchedulePreview();
    }

    // Title-bar actions appear while there is something to save, and briefly after a save.
    private void UpdateButtons()
    {
        bool dirty = !_current.SameAs(_saved);
        SaveButton.IsEnabled = dirty;
        RevertButton.IsEnabled = dirty;
        _flashId++;
        DirtyText.Text = "未保存";
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0xFF, 0x9F, 0x0A));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Visible;
        ShowSaveBar(dirty);
    }

    private void ShowSaveBar(bool on)
    {
        SaveBar.OpacityTransition ??= new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
        SaveBar.Opacity = on ? 1 : 0;
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

    // A group's "restore defaults": its parameters (and what goes with them) back to the defaults.
    private void OnGroupReset(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string group }) return;
        var d = BarSettings.Liquid();
        foreach (var (def, _) in _rows)
            if (def.Group == group) def.Set(_current, def.Get(d));
        if (group == "clarity") _current.Tint = d.Tint;
        if (group == "width") _current.AutoWidth = d.AutoWidth;
        if (group == "notify") _current.NotifyHideBody = d.NotifyHideBody;
        if (group is "clarity" or "edge" or "light")
            _current.Preset = _current.SameMaterial(BarSettings.Liquid()) ? "liquid" : _current.SameMaterial(BarSettings.Frosted()) ? "frosted" : "custom";
        LoadIntoControls();
        Changed();
    }

    // The material back to the Liquid preset (layout, apps and sources stay as they are).
    private void OnDefaults(object sender, RoutedEventArgs e)
    {
        _current.ApplyMaterial(BarSettings.Liquid());
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
