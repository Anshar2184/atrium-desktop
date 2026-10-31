using System.Runtime.InteropServices;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace Atrium.Probe;

// Windows.Graphics.Capture helpers using raw vtable calls for the two Win32
// interop interfaces, so no COM marshalling layer is involved.
internal static unsafe class Wgc
{
    private static readonly Guid IID_IGraphicsCaptureItemInterop = new("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356");
    private static readonly Guid IID_IGraphicsCaptureItem = new("79C3F95B-31F7-4EC2-A464-632EF5D30760");
    private static readonly Guid IID_IDirect3DDxgiInterfaceAccess = new("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1");
    private static readonly Guid IID_ID3D11Texture2D = new("6f15aaf2-d208-4e89-9ab4-489535d34f9c");

    [DllImport("d3d11.dll")]
    private static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);

    public static IDirect3DDevice WrapDevice(ID3D11Device device)
    {
        using var dxgi = device.QueryInterface<IDXGIDevice>();
        Marshal.ThrowExceptionForHR(CreateDirect3D11DeviceFromDXGIDevice(dxgi.NativePointer, out var p));
        try { return MarshalInterface<IDirect3DDevice>.FromAbi(p); }
        finally { Marshal.Release(p); }
    }

    private static GraphicsCaptureItem Create(IntPtr handle, int slot)
    {
        using var factory = ActivationFactory.Get("Windows.Graphics.Capture.GraphicsCaptureItem");
        Guid iidInterop = IID_IGraphicsCaptureItemInterop;
        Marshal.ThrowExceptionForHR(Marshal.QueryInterface(factory.ThisPtr, ref iidInterop, out var interop));
        try
        {
            Guid iid = IID_IGraphicsCaptureItem;
            IntPtr item;
            var fn = (delegate* unmanaged[Stdcall]<IntPtr, IntPtr, Guid*, IntPtr*, int>)(*(*(void***)interop + slot));
            Marshal.ThrowExceptionForHR(fn(interop, handle, &iid, &item));
            try { return GraphicsCaptureItem.FromAbi(item); }
            finally { Marshal.Release(item); }
        }
        finally { Marshal.Release(interop); }
    }

    public static GraphicsCaptureItem ForWindow(IntPtr hwnd) => Create(hwnd, 3);
    public static GraphicsCaptureItem ForMonitor(IntPtr hmon) => Create(hmon, 4);

    public static ID3D11Texture2D TextureOf(Direct3D11CaptureFrame frame)
    {
        var unk = ((IWinRTObject)frame.Surface).NativeObject.ThisPtr;
        Guid iidAccess = IID_IDirect3DDxgiInterfaceAccess;
        Marshal.ThrowExceptionForHR(Marshal.QueryInterface(unk, ref iidAccess, out var access));
        try
        {
            Guid iid = IID_ID3D11Texture2D;
            IntPtr tex;
            var fn = (delegate* unmanaged[Stdcall]<IntPtr, Guid*, IntPtr*, int>)(*(*(void***)access + 3));
            Marshal.ThrowExceptionForHR(fn(access, &iid, &tex));
            return new ID3D11Texture2D(tex);
        }
        finally { Marshal.Release(access); }
    }
}

// A capture session that keeps only the most recent frame copied into a texture
// owned by us, so the WGC frame can be returned to the pool immediately.
// Metadata delivered together with the texture it describes. Times are in
// 100 ns units on the QPC time base (same base as SystemRelativeTime).
internal sealed record FrameMeta(long Id, long SourceTime, long ArrivalTime, long CopiedTime, IReadOnlyList<Windows.Graphics.RectInt32>? Dirty)
{
    public static long Now() => (long)(System.Diagnostics.Stopwatch.GetTimestamp() * (10_000_000.0 / System.Diagnostics.Stopwatch.Frequency));
}

internal sealed class CaptureFeed : IDisposable
{
    private FrameMeta? _meta;
    private readonly bool _reportDirty;
    private readonly Gpu _gpu;
    private readonly Direct3D11CaptureFramePool _pool;
    private readonly GraphicsCaptureSession _session;
    private readonly object _lock = new();
    private ID3D11Texture2D? _latest;
    public int Frames;
    public long LastFrameTicks;
    public Windows.Graphics.SizeInt32 Size { get; private set; }
    public string? BorderNote { get; }

    public Action<Direct3D11CaptureFrame>? Inspect;
    public string? DirtyNote { get; }

    public CaptureFeed(Gpu gpu, GraphicsCaptureItem item, bool tryBorderless, bool reportDirty = false)
    {
        _gpu = gpu;
        _reportDirty = reportDirty;
        Size = item.Size;
        var device = Wgc.WrapDevice(gpu.Device);
        _pool = Direct3D11CaptureFramePool.CreateFreeThreaded(device, DirectXPixelFormat.B8G8R8A8UIntNormalized, 2, item.Size);
        _pool.FrameArrived += OnFrame;
        _session = _pool.CreateCaptureSession(item);
        try { _session.IsCursorCaptureEnabled = false; } catch { }
        if (tryBorderless)
        {
            try { _session.IsBorderRequired = false; BorderNote = "IsBorderRequired=false accepted"; }
            catch (Exception e) { BorderNote = "IsBorderRequired=false rejected: " + e.Message; }
        }
        if (reportDirty)
        {
            try { _session.DirtyRegionMode = GraphicsCaptureDirtyRegionMode.ReportOnly; DirtyNote = "DirtyRegionMode=ReportOnly"; }
            catch (Exception e) { DirtyNote = "DirtyRegionMode rejected: " + e.Message; }
        }
        _session.StartCapture();
    }

    private void OnFrame(Direct3D11CaptureFramePool pool, object _)
    {
        long arrival = FrameMeta.Now();
        using var frame = pool.TryGetNextFrame();
        if (frame == null) return;
        Inspect?.Invoke(frame);
        IReadOnlyList<Windows.Graphics.RectInt32>? dirty = null;
        if (_reportDirty) { try { dirty = frame.DirtyRegions.ToList(); } catch { } }
        using var tex = Wgc.TextureOf(frame);
        var desc = tex.Description;
        // Lock order everywhere: device context first, then the frame slot.
        lock (_gpu.Context)
        lock (_lock)
        {
            if (_latest == null || _latest.Description.Width != desc.Width || _latest.Description.Height != desc.Height)
            {
                _latest?.Dispose();
                desc.BindFlags = BindFlags.ShaderResource;
                desc.MiscFlags = ResourceOptionFlags.None;
                desc.Usage = ResourceUsage.Default;
                desc.CPUAccessFlags = CpuAccessFlags.None;
                _latest = _gpu.Device.CreateTexture2D(desc);
            }
            _gpu.Context.CopyResource(_latest, tex);
            _meta = new FrameMeta(Frames + 1, frame.SystemRelativeTime.Ticks, arrival, FrameMeta.Now(), dirty);
        }
        if (frame.ContentSize.Width != Size.Width || frame.ContentSize.Height != Size.Height)
        {
            Size = frame.ContentSize;
            pool.Recreate(Wgc.WrapDevice(_gpu.Device), DirectXPixelFormat.B8G8R8A8UIntNormalized, 2, Size);
        }
        Interlocked.Increment(ref Frames);
        LastFrameTicks = Environment.TickCount64;
    }

    public T? WithLatest<T>(Func<ID3D11Texture2D, T> f)
    {
        lock (_gpu.Context)
        lock (_lock) return _latest == null ? default : f(_latest);
    }

    public T? WithLatest<T>(Func<ID3D11Texture2D, FrameMeta, T> f)
    {
        lock (_gpu.Context)
        lock (_lock) return _latest == null || _meta == null ? default : f(_latest, _meta);
    }

    public bool WaitFrames(int count, int timeoutMs)
    {
        var end = Environment.TickCount64 + timeoutMs;
        int start = Frames;
        while (Frames - start < count && Environment.TickCount64 < end) { Native.Pump(); Thread.Sleep(5); }
        return Frames - start >= count;
    }

    public void Dispose()
    {
        _session.Dispose();
        _pool.Dispose();
        lock (_lock) _latest?.Dispose();
    }
}
