#include "capture.h"

#include "../log.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace atrium {

MonitorCapture::MonitorCapture(const Gpu& gpu, HMONITOR monitor, HANDLE frameEvent)
{
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));

    auto dxgi = gpu.device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    device_ = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    size_ = item.Size();
    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
    token_ = pool_.FrameArrived([frameEvent](auto&&, auto&&) { SetEvent(frameEvent); });
    session_ = pool_.CreateCaptureSession(item);
    session_.IsCursorCaptureEnabled(false);
    try {
        session_.IsBorderRequired(false); // no yellow border (VERIFICATION 1.11)
    } catch (const winrt::hresult_error& e) {
        Log(L"capture: IsBorderRequired(false) rejected: 0x%08X", static_cast<unsigned>(e.code()));
    }
    session_.StartCapture();
    Log(L"capture: monitor %dx%d started", size_.Width, size_.Height);
}

MonitorCapture::~MonitorCapture()
{
    if (pool_)
        pool_.FrameArrived(token_);
    if (session_)
        session_.Close();
    if (pool_)
        pool_.Close();
}

wgc::Direct3D11CaptureFrame MonitorCapture::TryLatest()
{
    wgc::Direct3D11CaptureFrame latest{nullptr};
    while (auto f = pool_.TryGetNextFrame()) {
        if (latest)
            latest.Close();
        latest = f;
    }
    if (latest) {
        auto cs = latest.ContentSize();
        if (cs.Width != size_.Width || cs.Height != size_.Height) {
            // Resolution changed: later frames come at the new size.
            size_ = cs;
            pool_.Recreate(device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
            Log(L"capture: monitor resized to %dx%d", size_.Width, size_.Height);
        }
    }
    return latest;
}

winrt::com_ptr<ID3D11Texture2D> MonitorCapture::TextureOf(const wgc::Direct3D11CaptureFrame& frame)
{
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
    return tex;
}

} // namespace atrium
