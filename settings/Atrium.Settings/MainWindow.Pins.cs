using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Pickers;

namespace Atrium.Settings;

// One card of the pinned-apps list.
public sealed class PinVm : INotifyPropertyChanged
{
    private readonly MainWindow _owner;
    private string _number = "";
    private bool _canUp, _canDown;
    private ImageSource? _icon;

    internal PinVm(MainWindow owner, PinnedApp pin)
    {
        _owner = owner;
        Pin = pin;
        Title = MainWindow.PinTitle(pin);
        // Files show their file name; Store and system apps have no path.
        Detail = pin.Path.Length > 0 ? System.IO.Path.GetFileName(pin.Path) : "已安装的应用";
        UpName = "上移 " + Title;
        DownName = "下移 " + Title;
        RemoveName = "移除 " + Title;
    }

    internal PinnedApp Pin { get; }
    public string Title { get; }
    public string Detail { get; }
    public string UpName { get; }
    public string DownName { get; }
    public string RemoveName { get; }

    public string Number { get => _number; private set => Set(ref _number, value, nameof(Number)); }
    public bool CanUp { get => _canUp; private set => Set(ref _canUp, value, nameof(CanUp)); }
    public bool CanDown { get => _canDown; private set => Set(ref _canDown, value, nameof(CanDown)); }
    public ImageSource? Icon
    {
        get => _icon;
        set { if (Set(ref _icon, value, nameof(Icon))) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(FallbackVisibility))); }
    }
    public Visibility FallbackVisibility => _icon == null ? Visibility.Visible : Visibility.Collapsed;

    internal void SetPosition(int index, int count)
    {
        Number = (index + 1).ToString("00");
        CanUp = index > 0;
        CanDown = index < count - 1;
    }

    public void MoveUp() => _owner.MovePin(this, -1);
    public void MoveDown() => _owner.MovePin(this, 1);
    public void Remove() => _owner.RemovePin(this);

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool Set<T>(ref T field, T value, string name)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }
}

public sealed partial class MainWindow
{
    private readonly ObservableCollection<PinVm> _pinVms = new();
    private readonly Dictionary<string, ImageSource?> _iconCache = new(StringComparer.OrdinalIgnoreCase);

    internal static string PinTitle(PinnedApp p) =>
        p.Name.Length > 0 ? p.Name : p.Path.Length > 0 ? System.IO.Path.GetFileNameWithoutExtension(p.Path) : p.Aumid;

    private void InitPins() => PinsList.ItemsSource = _pinVms;

    private void RebuildPins()
    {
        _pinVms.Clear();
        foreach (var pin in _current.Pins ?? new List<PinnedApp>())
        {
            var vm = new PinVm(this, pin);
            _pinVms.Add(vm);
            _ = LoadIconAsync(vm);
        }
        RefreshPins();
    }

    private void RefreshPins()
    {
        for (int i = 0; i < _pinVms.Count; i++) _pinVms[i].SetPosition(i, _pinVms.Count);
        bool empty = _pinVms.Count == 0;
        PinsEmpty.Visibility = empty ? Visibility.Visible : Visibility.Collapsed;
        PinsList.Visibility = empty ? Visibility.Collapsed : Visibility.Visible;
        PinCount.Text = $"{_pinVms.Count} / 64";
    }

    // The collection is the order the user sees; the settings follow it.
    private void SyncPins()
    {
        _current.Pins = _pinVms.Select(v => v.Pin).ToList();
        RefreshPins();
    }

    private async Task LoadIconAsync(PinVm vm)
    {
        var name = ShellIcons.ParsingName(vm.Pin);
        if (!_iconCache.TryGetValue(name, out var source))
        {
            var img = await ShellIcons.LoadAsync(name, 96); // 32 DIP at up to 300 % scale
            if (img != null)
            {
                var bmp = new WriteableBitmap(img.Width, img.Height);
                using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(img.Bgra, 0, img.Bgra.Length);
                bmp.Invalidate();
                source = bmp;
            }
            _iconCache[name] = source;
        }
        if (source != null) vm.Icon = source;
    }

    internal void MovePin(PinVm vm, int delta)
    {
        int index = _pinVms.IndexOf(vm), to = index + delta;
        if (index < 0 || to < 0 || to >= _pinVms.Count) return;
        _pinVms.Move(index, to);
        SyncPins();
        FocusPinButton(vm, delta < 0 ? "上移" : "下移");
        Changed();
    }

    internal void RemovePin(PinVm vm)
    {
        int index = _pinVms.IndexOf(vm);
        if (index < 0) return;
        _pinVms.RemoveAt(index);
        SyncPins();
        if (_pinVms.Count > 0) FocusPinButton(_pinVms[Math.Min(index, _pinVms.Count - 1)], "移除");
        Changed();
    }

    // Keyboard users keep their place after a move or removal.
    private void FocusPinButton(PinVm vm, string verb)
    {
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            if (PinsList.ContainerFromItem(vm) is not DependencyObject container) return;
            var buttons = new List<Button>();
            Collect(container, buttons);
            var target = buttons.FirstOrDefault(b => b.IsEnabled && AutomationProperties.GetName(b).StartsWith(verb)) ?? buttons.FirstOrDefault(b => b.IsEnabled);
            target?.Focus(FocusState.Programmatic);
        });
    }

    private static void Collect(DependencyObject root, List<Button> into)
    {
        for (int i = 0, n = VisualTreeHelper.GetChildrenCount(root); i < n; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is Button b) into.Add(b);
            Collect(child, into);
        }
    }

    // Drag and drop reordered the collection; write the new order back.
    private void OnPinsDragCompleted(ListViewBase sender, DragItemsCompletedEventArgs args)
    {
        var order = _pinVms.Select(v => v.Pin).ToList();
        if (_current.Pins != null && order.SequenceEqual(_current.Pins))
        {
            RefreshPins();
            return;
        }
        SyncPins();
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
        if (_pinVms.Count >= 64)
        {
            ShowMessage(InfoBarSeverity.Warning, "固定的应用已达上限", "最多可以固定 64 个应用。");
            return;
        }
        var vm = new PinVm(this, new PinnedApp(System.IO.Path.GetFileNameWithoutExtension(file.Path), file.Path, "", ""));
        _pinVms.Add(vm);
        _ = LoadIconAsync(vm);
        SyncPins();
        FocusPinButton(vm, "移除");
        PinsList.ScrollIntoView(vm);
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
}
