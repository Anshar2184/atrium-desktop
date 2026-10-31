using System.Runtime.InteropServices;
using Vortice.D3DCompiler;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DirectComposition;
using Vortice.DXGI;
using Vortice.Mathematics;

namespace Atrium.Probe;

[StructLayout(LayoutKind.Sequential)]
internal struct GlassParams
{
    public float SizeX, SizeY, RoiOriginX, RoiOriginY;
    public float RoiSizeX, RoiSizeY, Radius, Edge;
    public float Refract, Dispersion, BlurLod, TintMix;
    public float TintR, TintG, TintB, TintA;
    public float Highlight, Shadow, LightX, LightY;
    public float Inset, BlurMix, Pad0, Pad1;

    public static GlassParams Liquid(float w, float h, float inset) => new()
    {
        SizeX = w, SizeY = h, Radius = 9999, Edge = 34, Refract = 26, Dispersion = 0.08f,
        BlurLod = 2.2f, BlurMix = 0.35f, TintMix = 0.10f, TintR = 1, TintG = 1, TintB = 1, TintA = 1,
        Highlight = 0.55f, Shadow = 0.28f, LightX = -0.6f, LightY = -0.8f, Inset = inset,
    };

    public static GlassParams Frosted(float w, float h, float inset) => new()
    {
        SizeX = w, SizeY = h, Radius = 9999, Edge = 1, Refract = 0, Dispersion = 0,
        BlurLod = 4.5f, BlurMix = 1, TintMix = 0.22f, TintR = 0.96f, TintG = 0.96f, TintB = 0.98f, TintA = 1,
        Highlight = 0.30f, Shadow = 0.28f, LightX = -0.6f, LightY = -0.8f, Inset = inset,
    };

    public GlassParams Lerp(GlassParams b, float t)
    {
        GlassParams r = this;
        float L(float x, float y) => x + (y - x) * t;
        r.Edge = L(Edge, b.Edge); r.Refract = L(Refract, b.Refract); r.Dispersion = L(Dispersion, b.Dispersion);
        r.BlurLod = L(BlurLod, b.BlurLod); r.BlurMix = L(BlurMix, b.BlurMix); r.TintMix = L(TintMix, b.TintMix);
        r.TintR = L(TintR, b.TintR); r.TintG = L(TintG, b.TintG); r.TintB = L(TintB, b.TintB);
        r.Highlight = L(Highlight, b.Highlight);
        return r;
    }
}

// Renders the glass material for one window: copy a padded background ROI from
// the capture texture, build mips for blur, draw one full-window triangle into a
// premultiplied composition swap chain hosted by DirectComposition.
internal sealed class GlassRenderer : IDisposable
{
    public const int Pad = 64;
    private readonly Gpu _gpu;
    private readonly int _w, _h;
    private readonly IDXGISwapChain1? _swap;
    private readonly IDCompositionDevice? _dcomp;
    private readonly IDCompositionTarget? _target;
    private readonly IDCompositionVisual? _visual;
    private readonly ID3D11Texture2D _roi;
    private readonly ID3D11ShaderResourceView _roiSrv;
    private readonly ID3D11VertexShader _vs;
    private readonly ID3D11PixelShader _ps;
    private readonly ID3D11Buffer _cb;
    private readonly ID3D11SamplerState _sampler;
    private readonly ID3D11BlendState _blend;
    private readonly Queue<(ID3D11Query Disjoint, ID3D11Query T0, ID3D11Query T1)> _pending = new();
    private readonly Stack<(ID3D11Query, ID3D11Query, ID3D11Query)> _free = new();
    public List<double> GpuMs { get; } = new();
    public int Presents;

    public GlassRenderer(Gpu gpu, IntPtr hwnd, int w, int h)
    {
        _gpu = gpu; _w = w; _h = h;
        var dev = gpu.Device;
        var src = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Shaders", "glass.hlsl"));
        _vs = dev.CreateVertexShader(Compiler.Compile(src, "VS", "glass.hlsl", "vs_5_0", ShaderFlags.OptimizationLevel3, EffectFlags.None).Span);
        _ps = dev.CreatePixelShader(Compiler.Compile(src, "PS", "glass.hlsl", "ps_5_0", ShaderFlags.OptimizationLevel3, EffectFlags.None).Span);
        _cb = dev.CreateBuffer(new BufferDescription((uint)Marshal.SizeOf<GlassParams>(), BindFlags.ConstantBuffer, ResourceUsage.Default));
        _sampler = dev.CreateSamplerState(new SamplerDescription(Filter.MinMagMipLinear, TextureAddressMode.Clamp, TextureAddressMode.Clamp, TextureAddressMode.Clamp));
        _blend = dev.CreateBlendState(BlendDescription.Opaque);

        uint rw = (uint)(w + 2 * Pad), rh = (uint)(h + 2 * Pad);
        _roi = dev.CreateTexture2D(new Texture2DDescription
        {
            Width = rw, Height = rh, MipLevels = 0, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default,
            BindFlags = BindFlags.ShaderResource | BindFlags.RenderTarget, MiscFlags = ResourceOptionFlags.GenerateMips,
        });
        _roiSrv = dev.CreateShaderResourceView(_roi);

        if (hwnd != IntPtr.Zero)
        {
            _swap = gpu.Factory.CreateSwapChainForComposition(dev, new SwapChainDescription1
            {
                Width = (uint)w, Height = (uint)h, Format = Format.B8G8R8A8_UNorm, BufferCount = 2,
                BufferUsage = Usage.RenderTargetOutput, SampleDescription = new SampleDescription(1, 0),
                SwapEffect = SwapEffect.FlipSequential, AlphaMode = AlphaMode.Premultiplied, Scaling = Scaling.Stretch,
            });
            using var dxgiDev = dev.QueryInterface<IDXGIDevice>();
            _dcomp = DComp.DCompositionCreateDevice<IDCompositionDevice>(dxgiDev);
            _dcomp.CreateTargetForHwnd(hwnd, true, out _target).CheckError();
            _dcomp.CreateVisual(out _visual).CheckError();
            _visual!.SetContent(_swap).CheckError();
            _target!.SetRoot(_visual).CheckError();
            _dcomp.Commit().CheckError();
        }
    }

    // Copies the background under the window (monitor-relative winX/winY) into the ROI.
    public void UpdateBackground(ID3D11Texture2D source, int winX, int winY)
    {
        var sd = source.Description;
        int x0 = winX - Pad, y0 = winY - Pad, x1 = winX + _w + Pad, y1 = winY + _h + Pad;
        int cx0 = Math.Max(0, x0), cy0 = Math.Max(0, y0), cx1 = Math.Min((int)sd.Width, x1), cy1 = Math.Min((int)sd.Height, y1);
        if (cx1 <= cx0 || cy1 <= cy0) return;
        var ctx = _gpu.Context;
        ctx.CopySubresourceRegion(_roi, 0, (uint)(cx0 - x0), (uint)(cy0 - y0), 0, source, 0, new Box(cx0, cy0, 0, cx1, cy1, 1));
        ctx.GenerateMips(_roiSrv);
    }

    private void Draw(ID3D11RenderTargetView rtv, GlassParams p)
    {
        var ctx = _gpu.Context;
        p.RoiOriginX = Pad; p.RoiOriginY = Pad; p.RoiSizeX = _w + 2 * Pad; p.RoiSizeY = _h + 2 * Pad;
        p.SizeX = _w; p.SizeY = _h;
        ctx.UpdateSubresource(in p, _cb);
        ctx.OMSetRenderTargets(rtv);
        ctx.OMSetBlendState(_blend);
        ctx.RSSetViewport(new Viewport(0, 0, _w, _h));
        ctx.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        ctx.VSSetShader(_vs);
        ctx.PSSetShader(_ps);
        ctx.PSSetConstantBuffer(0, _cb);
        ctx.PSSetShaderResource(0, _roiSrv);
        ctx.PSSetSampler(0, _sampler);
        ctx.Draw(3, 0);
        ctx.PSSetShaderResource(0, null!);
    }

    public void RenderAndPresent(GlassParams p)
    {
        if (_swap == null) return;
        var ctx = _gpu.Context;
        var q = _free.Count > 0 ? _free.Pop() : (_gpu.Device.CreateQuery(new QueryDescription(QueryType.TimestampDisjoint)),
            _gpu.Device.CreateQuery(new QueryDescription(QueryType.Timestamp)), _gpu.Device.CreateQuery(new QueryDescription(QueryType.Timestamp)));
        using var back = _swap.GetBuffer<ID3D11Texture2D>(0);
        using var rtv = _gpu.Device.CreateRenderTargetView(back);
        ctx.Begin(q.Item1);
        ctx.End(q.Item2);
        Draw(rtv, p);
        ctx.End(q.Item3);
        ctx.End(q.Item1);
        _pending.Enqueue(q);
        _swap.Present(1, PresentFlags.None);
        Presents++;
        Collect();
    }

    private void Collect()
    {
        var ctx = _gpu.Context;
        while (_pending.Count > 0)
        {
            var q = _pending.Peek();
            if (!ctx.GetData(q.Disjoint, AsyncGetDataFlags.DoNotFlush, out QueryDataTimestampDisjoint dj)) break;
            if (!ctx.GetData(q.T0, AsyncGetDataFlags.DoNotFlush, out ulong t0) || !ctx.GetData(q.T1, AsyncGetDataFlags.DoNotFlush, out ulong t1)) break;
            if (!dj.Disjoint && dj.Frequency > 0 && t1 > t0) GpuMs.Add((t1 - t0) * 1000.0 / dj.Frequency);
            _pending.Dequeue();
            _free.Push(q);
        }
    }

    private byte[]? _lastSig;
    public const int SigMip = 3;

    // Reads a coarse mip of the backdrop ROI and reports whether it differs from
    // the previous call. Used to tell our own present echo from real changes.
    public bool BackdropChanged(double threshold, out double diff)
    {
        var d = _roi.Description;
        uint sw = Math.Max(1u, d.Width >> SigMip), sh = Math.Max(1u, d.Height >> SigMip);
        using var staging = _gpu.CreateStaging(sw, sh);
        _gpu.Context.CopySubresourceRegion(staging, 0, 0, 0, 0, _roi, (uint)SigMip);
        var map = _gpu.Context.Map(staging, 0, MapMode.Read);
        var sig = new byte[sw * sh * 4];
        try
        {
            for (int row = 0; row < sh; row++)
                Marshal.Copy(map.DataPointer + (nint)(row * map.RowPitch), sig, (int)(row * sw * 4), (int)(sw * 4));
        }
        finally { _gpu.Context.Unmap(staging, 0); }
        diff = 0;
        if (_lastSig != null && _lastSig.Length == sig.Length)
        {
            int max = 0;
            for (int i = 0; i < sig.Length; i++) max = Math.Max(max, Math.Abs(sig[i] - _lastSig[i]));
            diff = max;
        }
        else diff = 255;
        _lastSig = sig;
        return diff > threshold;
    }

    // Pure GPU cost without swap-chain waits: n iterations of (ROI copy + mips)
    // and of the glass draw, each bracketed by timestamp queries.
    public (List<double> Update, List<double> Draw) BenchOffscreen(ID3D11Texture2D source, int winX, int winY, GlassParams p, int n)
    {
        var dev = _gpu.Device; var ctx = _gpu.Context;
        using var tex = dev.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_w, Height = (uint)_h, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget,
        });
        using var rtv = dev.CreateRenderTargetView(tex);
        var upd = new List<double>(); var drw = new List<double>();
        for (int i = 0; i < n; i++)
        {
            using var dj = dev.CreateQuery(new QueryDescription(QueryType.TimestampDisjoint));
            using var a = dev.CreateQuery(new QueryDescription(QueryType.Timestamp));
            using var b = dev.CreateQuery(new QueryDescription(QueryType.Timestamp));
            using var c = dev.CreateQuery(new QueryDescription(QueryType.Timestamp));
            ctx.Begin(dj);
            ctx.End(a);
            UpdateBackground(source, winX, winY);
            ctx.End(b);
            Draw(rtv, p);
            ctx.End(c);
            ctx.End(dj);
            ctx.Flush();
            QueryDataTimestampDisjoint d; ulong ta, tb, tc;
            while (!ctx.GetData(dj, AsyncGetDataFlags.None, out d)) Thread.SpinWait(50);
            ctx.GetData(a, AsyncGetDataFlags.None, out ta); ctx.GetData(b, AsyncGetDataFlags.None, out tb); ctx.GetData(c, AsyncGetDataFlags.None, out tc);
            if (d.Disjoint || d.Frequency == 0) continue;
            upd.Add((tb - ta) * 1000.0 / d.Frequency);
            drw.Add((tc - tb) * 1000.0 / d.Frequency);
        }
        return (upd, drw);
    }

    // Throughput fallback for drivers whose timestamps collapse within one
    // command buffer: run n iterations back to back and wait on an event query.
    public double BenchThroughputMs(ID3D11Texture2D source, int winX, int winY, GlassParams p, int n)
    {
        var dev = _gpu.Device; var ctx = _gpu.Context;
        using var tex = dev.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_w, Height = (uint)_h, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget,
        });
        using var rtv = dev.CreateRenderTargetView(tex);
        using var ev = dev.CreateQuery(new QueryDescription(QueryType.Event));
        void Wait() { ctx.End(ev); ctx.Flush(); while (!ctx.GetData(ev, AsyncGetDataFlags.None, out int _)) Thread.SpinWait(20); }
        Wait();
        var sw = System.Diagnostics.Stopwatch.StartNew();
        for (int i = 0; i < n; i++) { UpdateBackground(source, winX, winY); Draw(rtv, p); }
        Wait();
        return sw.Elapsed.TotalMilliseconds / n;
    }

    // Renders the glass into an offscreen texture and returns BGRA premultiplied pixels.
    public byte[] RenderOffscreen(GlassParams p)
    {
        using var tex = _gpu.Device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_w, Height = (uint)_h, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget,
        });
        using var rtv = _gpu.Device.CreateRenderTargetView(tex);
        Draw(rtv, p);
        return _gpu.ReadBack(tex, 0, 0, _w, _h, out _);
    }

    public void Dispose()
    {
        foreach (var q in _pending) { q.Disjoint.Dispose(); q.T0.Dispose(); q.T1.Dispose(); }
        foreach (var q in _free) { q.Item1.Dispose(); q.Item2.Dispose(); q.Item3.Dispose(); }
        _visual?.Dispose(); _target?.Dispose(); _dcomp?.Dispose(); _swap?.Dispose();
        _roiSrv.Dispose(); _roi.Dispose(); _vs.Dispose(); _ps.Dispose(); _cb.Dispose(); _sampler.Dispose(); _blend.Dispose();
    }
}

internal static class Stats
{
    public static double Pct(IReadOnlyList<double> v, double p)
    {
        if (v.Count == 0) return double.NaN;
        var s = v.OrderBy(x => x).ToArray();
        return Math.Round(s[Math.Clamp((int)Math.Ceiling(p / 100 * s.Length) - 1, 0, s.Length - 1)], 3);
    }

    // Composites premultiplied BGRA `top` over opaque `under` in place.
    public static void Over(byte[] under, byte[] top)
    {
        for (int i = 0; i < under.Length; i += 4)
        {
            int a = top[i + 3];
            for (int k = 0; k < 3; k++) under[i + k] = (byte)Math.Min(255, top[i + k] + under[i + k] * (255 - a) / 255);
        }
    }
}
