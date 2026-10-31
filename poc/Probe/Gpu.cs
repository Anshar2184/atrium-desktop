using System.Runtime.InteropServices;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;

namespace Atrium.Probe;

internal sealed class Gpu : IDisposable
{
    public IDXGIFactory2 Factory { get; }
    public IDXGIAdapter1 Adapter { get; }
    public ID3D11Device Device { get; }
    public ID3D11DeviceContext Context { get; }
    public string AdapterName { get; }

    private Gpu(IDXGIFactory2 factory, IDXGIAdapter1 adapter, ID3D11Device device, ID3D11DeviceContext ctx)
    {
        Factory = factory;
        Adapter = adapter;
        Device = device;
        Context = ctx;
        AdapterName = adapter.Description1.Description;
    }

    // Creates a device on the adapter that owns the output containing `monitor`,
    // which is what DXGI duplication requires and what avoids cross-adapter copies.
    public static Gpu ForMonitor(IntPtr monitor)
    {
        var factory = DXGI.CreateDXGIFactory2<IDXGIFactory2>(false);
        for (uint a = 0; factory.EnumAdapters1(a, out var adapter).Success; a++)
        {
            for (uint o = 0; adapter.EnumOutputs(o, out var output).Success; o++)
            {
                bool match = output.Description.Monitor == monitor;
                output.Dispose();
                if (match) return Create(factory, adapter);
            }
            adapter.Dispose();
        }
        factory.EnumAdapters1(0, out var first).CheckError();
        return Create(factory, first);
    }

    private static Gpu Create(IDXGIFactory2 factory, IDXGIAdapter1 adapter)
    {
        D3D11.D3D11CreateDevice(adapter, DriverType.Unknown, DeviceCreationFlags.BgraSupport,
            new[] { FeatureLevel.Level_11_1, FeatureLevel.Level_11_0 }, out ID3D11Device device, out ID3D11DeviceContext ctx).CheckError();
        return new Gpu(factory, adapter, device!, ctx!);
    }

    public ID3D11Texture2D CreateStaging(uint w, uint h, Format fmt = Format.B8G8R8A8_UNorm) =>
        Device.CreateTexture2D(new Texture2DDescription
        {
            Width = w, Height = h, MipLevels = 1, ArraySize = 1, Format = fmt,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Staging,
            CPUAccessFlags = CpuAccessFlags.Read, BindFlags = BindFlags.None,
        });

    // Copies a region of a GPU texture to CPU memory (BGRA8). Only for probes;
    // the product never reads back pixels per frame.
    public byte[] ReadBack(ID3D11Texture2D src, int x, int y, int w, int h, out int stride)
    {
        using var staging = CreateStaging((uint)w, (uint)h, src.Description.Format);
        var box = new Box(x, y, 0, x + w, y + h, 1);
        Context.CopySubresourceRegion(staging, 0, 0, 0, 0, src, 0, box);
        var map = Context.Map(staging, 0, MapMode.Read);
        try
        {
            stride = w * 4;
            var data = new byte[stride * h];
            for (int row = 0; row < h; row++)
                Marshal.Copy(map.DataPointer + (nint)(row * map.RowPitch), data, row * stride, stride);
            return data;
        }
        finally { Context.Unmap(staging, 0); }
    }

    public void Dispose()
    {
        Context.Dispose();
        Device.Dispose();
        Adapter.Dispose();
        Factory.Dispose();
    }
}

internal static class Pixels
{
    // Fraction of BGRA pixels within `tol` of the given RGB color.
    public static double Fraction(byte[] bgra, byte r, byte g, byte b, int tol = 24)
    {
        int n = bgra.Length / 4, hit = 0;
        for (int i = 0; i < n; i++)
        {
            int bb = bgra[i * 4], gg = bgra[i * 4 + 1], rr = bgra[i * 4 + 2];
            if (Math.Abs(rr - r) <= tol && Math.Abs(gg - g) <= tol && Math.Abs(bb - b) <= tol) hit++;
        }
        return n == 0 ? 0 : (double)hit / n;
    }

    public static double Mean(byte[] bgra)
    {
        long sum = 0;
        for (int i = 0; i < bgra.Length; i += 4) sum += bgra[i] + bgra[i + 1] + bgra[i + 2];
        return bgra.Length == 0 ? 0 : sum / (bgra.Length / 4.0 * 3);
    }

    public static double BlackFraction(byte[] bgra) => Fraction(bgra, 0, 0, 0, 6);

    public static void SaveBmp(string path, byte[] bgra, int w, int h)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);
        int size = 54 + bgra.Length;
        bw.Write((byte)'B'); bw.Write((byte)'M'); bw.Write(size); bw.Write(0); bw.Write(54);
        bw.Write(40); bw.Write(w); bw.Write(-h); bw.Write((short)1); bw.Write((short)32);
        bw.Write(0); bw.Write(bgra.Length); bw.Write(2835); bw.Write(2835); bw.Write(0); bw.Write(0);
        bw.Write(bgra);
    }
}
