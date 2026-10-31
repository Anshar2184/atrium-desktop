using Vortice.Direct3D11;
using Vortice.DXGI;

namespace Atrium.Probe;

// DXGI Desktop Duplication for one output, keeping our own copy of the last frame.
internal sealed class DupFeed : IDisposable
{
    private readonly Gpu _gpu;
    private readonly IDXGIOutputDuplication _dup;
    private ID3D11Texture2D? _copy;
    public int Frames;
    public string OutputName { get; }

    public DupFeed(Gpu gpu, IntPtr monitor)
    {
        _gpu = gpu;
        IDXGIOutput1? target = null;
        for (uint o = 0; gpu.Adapter.EnumOutputs(o, out var output).Success; o++)
        {
            if (output.Description.Monitor == monitor) { target = output.QueryInterface<IDXGIOutput1>(); output.Dispose(); break; }
            output.Dispose();
        }
        if (target == null) throw new InvalidOperationException("monitor is not attached to the device adapter");
        OutputName = target.Description.DeviceName;
        _dup = target.DuplicateOutput(gpu.Device);
        target.Dispose();
    }

    // Pulls at most one frame; returns whether a new frame arrived.
    public bool Poll(uint timeoutMs)
    {
        var r = _dup.AcquireNextFrame(timeoutMs, out var info, out var res);
        if (r.Failure) return false;
        try
        {
            if (info.LastPresentTime == 0 && _copy != null) return false; // pointer-only update
            using var tex = res!.QueryInterface<ID3D11Texture2D>();
            if (_copy == null)
            {
                var d = tex.Description;
                d.BindFlags = BindFlags.ShaderResource; d.MiscFlags = ResourceOptionFlags.None;
                d.Usage = ResourceUsage.Default; d.CPUAccessFlags = CpuAccessFlags.None;
                _copy = _gpu.Device.CreateTexture2D(d);
            }
            lock (_gpu.Context) _gpu.Context.CopyResource(_copy, tex);
            Frames++;
            return true;
        }
        finally
        {
            res?.Dispose();
            _dup.ReleaseFrame();
        }
    }

    public ID3D11Texture2D? Latest => _copy;

    public void Dispose()
    {
        _copy?.Dispose();
        _dup.Dispose();
    }
}
