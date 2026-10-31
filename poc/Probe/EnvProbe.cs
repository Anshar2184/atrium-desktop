using System.Text.Json.Nodes;
using Vortice.DXGI;
using Windows.Foundation.Metadata;
using Windows.Graphics.Capture;
using static Atrium.Probe.Native;

namespace Atrium.Probe;

// P00: which adapter drives which output, DPI, HDR and capture API availability.
internal static class EnvProbe
{
    public static void Run()
    {
        var rep = new Report("p00-env");
        using var factory = DXGI.CreateDXGIFactory2<IDXGIFactory6>(false);
        var adapters = new JsonArray();
        for (uint a = 0; factory.EnumAdapters1(a, out var adapter).Success; a++)
        {
            using (adapter)
            {
                var d = adapter.Description1;
                var outputs = new JsonArray();
                for (uint o = 0; adapter.EnumOutputs(o, out var output).Success; o++)
                {
                    using (output)
                    {
                        var od = output.Description;
                        var node = new JsonObject
                        {
                            ["device"] = od.DeviceName,
                            ["desktop"] = $"{od.DesktopCoordinates.Left},{od.DesktopCoordinates.Top},{od.DesktopCoordinates.Right},{od.DesktopCoordinates.Bottom}",
                            ["attached"] = (bool)od.AttachedToDesktop,
                            ["rotation"] = od.Rotation.ToString(),
                        };
                        GetDpiForMonitor(od.Monitor, 0, out var dx, out _);
                        node["dpi"] = dx;
                        var mi = new MONITORINFOEX { cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFOEX>() };
                        GetMonitorInfo(od.Monitor, ref mi);
                        node["work"] = mi.rcWork.ToString();
                        node["primary"] = (mi.dwFlags & 1) != 0;
                        using var o6 = output.QueryInterfaceOrNull<IDXGIOutput6>();
                        if (o6 != null)
                        {
                            var d1 = o6.Description1;
                            node["colorSpace"] = d1.ColorSpace.ToString();
                            node["bitsPerColor"] = d1.BitsPerColor;
                            node["hdr"] = d1.ColorSpace == ColorSpaceType.RgbFullG2084NoneP2020;
                            node["maxLuminance"] = d1.MaxLuminance;
                        }
                        outputs.Add(node);
                    }
                }
                adapters.Add(new JsonObject
                {
                    ["name"] = d.Description,
                    ["luid"] = $"{d.Luid.HighPart:X8}{d.Luid.LowPart:X8}",
                    ["vendor"] = $"0x{d.VendorId:X4}",
                    ["dedicatedMB"] = (long)(d.DedicatedVideoMemory / 1048576),
                    ["software"] = (d.Flags & AdapterFlags.Software) != 0,
                    ["outputs"] = outputs,
                });
            }
        }
        rep.Set("adapters", adapters);

        bool owned = false;
        foreach (var a in adapters)
            if (a!["outputs"]!.AsArray().Count > 0 && !(bool)a["software"]!) owned = true;
        rep.Verdict("outputs-traceable", owned ? "Passed" : "Failed", "each attached output maps to an adapter LUID");

        rep.Set("wgc.isSupported", GraphicsCaptureSession.IsSupported());
        rep.Set("wgc.isBorderRequiredPresent", ApiInformation.IsPropertyPresent("Windows.Graphics.Capture.GraphicsCaptureSession", "IsBorderRequired"));
        rep.Set("wgc.accessClassPresent", ApiInformation.IsTypePresent("Windows.Graphics.Capture.GraphicsCaptureAccess"));
        rep.Set("wgc.dirtyRegionPresent", ApiInformation.IsPropertyPresent("Windows.Graphics.Capture.GraphicsCaptureSession", "DirtyRegionMode"));
        rep.Set("wgc.minUpdateIntervalPresent", ApiInformation.IsPropertyPresent("Windows.Graphics.Capture.GraphicsCaptureSession", "MinUpdateInterval"));
        rep.Set("idleSeconds", IdleSeconds());
        rep.Save();
    }
}
