// D3D11 device on the adapter that drives a given monitor. Rendering there
// avoids cross-adapter copies on hybrid laptops (D02, VERIFICATION 1.13).
#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <string>

namespace atrium {

struct Gpu {
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::wstring adapterName;

    static Gpu ForMonitor(HMONITOR monitor);
};

} // namespace atrium
