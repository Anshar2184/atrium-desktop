// The backdrop of the glass: one monitor, captured without Atrium's own
// windows. Two ways:
//  - DXGI Desktop Duplication (preferred): Atrium's windows are excluded from
//    it alone (WCA_EXCLUDED_FROM_DDA), so screenshots and recordings that use
//    Windows.Graphics.Capture or GDI still see them;
//  - Windows.Graphics.Capture (fallback): Atrium's windows must then be kept
//    out of every capture (WDA_EXCLUDEFROMCAPTURE), or the glass would sample
//    itself.
// Frames are pulled by the render thread; arrival only signals an event.
#pragma once

#include "gpu.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <memory>
#include <thread>

namespace atrium {

class BackdropSource {
public:
    virtual ~BackdropSource() = default;
    // The newest frame since the last call, or null; valid until Release().
    virtual ID3D11Texture2D* Acquire() = 0;
    virtual void Release() = 0;
    // The source stopped (the display mode changed, the secure desktop came): make a new one.
    virtual bool Lost() const { return false; }
    // Whether Atrium's windows may show in other captures while this one runs.
    virtual bool KeepsOwnWindowsOut() const = 0;
    virtual const wchar_t* Name() const = 0;
};

class MonitorCapture : public BackdropSource {
public:
    MonitorCapture(const Gpu& gpu, HMONITOR monitor, HANDLE frameEvent);
    ~MonitorCapture() override;
    MonitorCapture(const MonitorCapture&) = delete;
    MonitorCapture& operator=(const MonitorCapture&) = delete;

    ID3D11Texture2D* Acquire() override;
    void Release() override;
    bool KeepsOwnWindowsOut() const override { return false; }
    const wchar_t* Name() const override { return L"Windows.Graphics.Capture"; }

private:
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::SizeInt32 size_{};
    winrt::event_token token_{};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame_{nullptr};
    winrt::com_ptr<ID3D11Texture2D> texture_;
};

class DuplicationCapture : public BackdropSource {
public:
    // Null when the monitor cannot be duplicated; `why` says why (temporary:
    // E_ACCESSDENIED while the secure desktop shows).
    // elsewhere: DXGI lists the monitor under another GPU (a hybrid laptop
    // that placed this process on the discrete one) - see PreferPowerSavingGpu.
    static std::unique_ptr<DuplicationCapture> Create(const Gpu& gpu, HMONITOR monitor, HANDLE frameEvent, HRESULT& why, bool& elsewhere);
    ~DuplicationCapture() override;
    DuplicationCapture(const DuplicationCapture&) = delete;
    DuplicationCapture& operator=(const DuplicationCapture&) = delete;

    ID3D11Texture2D* Acquire() override;
    void Release() override;
    bool Lost() const override { return lost_; }
    bool KeepsOwnWindowsOut() const override { return true; }
    const wchar_t* Name() const override { return L"DXGI Desktop Duplication"; }
    static bool Temporary(HRESULT why);

private:
    DuplicationCapture() = default;
    void Pump(); // the waiting thread: holds one acquired frame until the render thread is done with it

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGIOutputDuplication> dup_;
    winrt::com_ptr<IDXGIResource> held_;     // the acquired frame (ready_ set)
    winrt::com_ptr<ID3D11Texture2D> texture_; // the same, while the render thread reads it
    HANDLE frameEvent_ = nullptr;
    HANDLE released_ = nullptr; // the render thread copied and released the frame
    HANDLE stop_ = nullptr;
    std::atomic<bool> ready_{false}, lost_{false};
    std::thread thread_;
};

// On a hybrid laptop DXGI lists the screens only under the GPU Windows placed
// the process on. Asks Windows (its per-app graphics preference, as in
// Settings > System > Display > Graphics) to run Atrium on the power-saving
// GPU, which drives the panel; from the next start. Leaves a choice the user
// made alone. True if the preference was written.
bool PreferPowerSavingGpu();

// Atrium's windows: always out of Desktop Duplication (Atrium's own
// backdrop); out of every other capture too unless `visible`.
void SetCaptureExclusion(HWND hwnd, bool visible);

} // namespace atrium
