#include "gpu.h"

#include "../log.h"

#include <vector>

namespace atrium {

namespace {

Gpu Create(const winrt::com_ptr<IDXGIFactory2>& factory, const winrt::com_ptr<IDXGIAdapter1>& adapter)
{
    Gpu g;
    g.factory = factory;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                           g.device.put(), nullptr, g.context.put()));
    // Only the render thread uses the immediate context, but WGC works with the
    // device from its own threads.
    if (auto mt = g.device.try_as<ID3D11Multithread>())
        mt->SetMultithreadProtected(TRUE);
    if (adapter) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        g.adapterName = d.Description;
    }
    return g;
}

// The adapter that really scans out `monitor`, from the display configuration.
// On hybrid laptops DXGI may list the output under the discrete GPU the process
// was placed on, while the integrated GPU drives the panel (VERIFICATION 1.10).
bool ScanoutAdapter(HMONITOR monitor, LUID& luid)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi))
        return false;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && wcscmp(source.viewGdiDeviceName, mi.szDevice) == 0) {
            luid = paths[i].sourceInfo.adapterId;
            return true;
        }
    }
    return false;
}

} // namespace

Gpu Gpu::ForMonitor(HMONITOR monitor)
{
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())));
    LUID luid{};
    if (auto f4 = factory.try_as<IDXGIFactory4>(); f4 && ScanoutAdapter(monitor, luid)) {
        winrt::com_ptr<IDXGIAdapter1> scanout;
        if (SUCCEEDED(f4->EnumAdapterByLuid(luid, IID_PPV_ARGS(scanout.put())))) {
            Gpu g = Create(factory, scanout);
            Log(L"gpu: %s scans out the bar's monitor", g.adapterName.c_str());
            return g;
        }
    }
    // Fallback: the adapter DXGI lists the monitor's output under.
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) == S_OK; ++a) {
        winrt::com_ptr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.put()) == S_OK; ++o) {
            DXGI_OUTPUT_DESC d{};
            output->GetDesc(&d);
            output = nullptr;
            if (d.Monitor == monitor) {
                Gpu g = Create(factory, adapter);
                Log(L"gpu: %s drives the bar's monitor", g.adapterName.c_str());
                return g;
            }
        }
        adapter = nullptr;
    }
    Log(L"gpu: no adapter owns the monitor, using the default adapter");
    return Create(factory, nullptr);
}

} // namespace atrium
