// Windows.Graphics.Capture of one monitor. Frames are pulled by the render
// thread; the arrival callback only signals an event.
#pragma once

#include "gpu.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace atrium {

class MonitorCapture {
public:
    MonitorCapture(const Gpu& gpu, HMONITOR monitor, HANDLE frameEvent);
    ~MonitorCapture();
    MonitorCapture(const MonitorCapture&) = delete;
    MonitorCapture& operator=(const MonitorCapture&) = delete;

    // The newest pending frame (older ones are dropped), or null. The frame
    // must be closed soon so the pool can reuse its buffer.
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame TryLatest();
    static winrt::com_ptr<ID3D11Texture2D> TextureOf(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame);

private:
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::SizeInt32 size_{};
    winrt::event_token token_{};
};

} // namespace atrium
