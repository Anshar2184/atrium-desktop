#include "capture.h"

#include "../log.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace atrium {

namespace {

constexpr int kBuffers = 1;

// SetWindowCompositionAttribute: documented, not recommended, no header.
// WCA_EXCLUDED_FROM_DDA (Windows 10 1709): the window is left out of
// Desktop Duplication only.
struct CompositionData {
    DWORD attribute;
    PVOID data;
    SIZE_T size;
};
constexpr DWORD kExcludedFromDda = 24;
using SetCompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);

SetCompositionFn SetComposition()
{
    static SetCompositionFn fn = reinterpret_cast<SetCompositionFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return fn;
}

} // namespace

bool PreferPowerSavingGpu()
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
        return false;
    const wchar_t* key = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, exe, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS)
        return false; // the user chose already
    const wchar_t value[] = L"GpuPreference=1;";
    LSTATUS e = RegSetKeyValueW(HKEY_CURRENT_USER, key, exe, REG_SZ, value, sizeof(value));
    Log(L"capture: graphics preference \"power saving\" for %s %s", exe, e == ERROR_SUCCESS ? L"set (from the next start)" : L"could not be set");
    return e == ERROR_SUCCESS;
}

void SetCaptureExclusion(HWND hwnd, bool visible)
{
    if (!hwnd)
        return;
    BOOL on = TRUE;
    CompositionData d{kExcludedFromDda, &on, sizeof(on)};
    if (!SetComposition() || !SetComposition()(hwnd, &d))
        visible = false; // without it, Desktop Duplication would see the window
    if (!SetWindowDisplayAffinity(hwnd, visible ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE))
        Log(L"capture: SetWindowDisplayAffinity failed (%lu)", GetLastError());
}

// ---- Windows.Graphics.Capture -------------------------------------------------

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
    // One buffer: each frame is closed right after its ROI is copied, and a
    // full-monitor buffer costs 4 bytes per pixel (25 MB at 3200x2000).
    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, kBuffers, size_);
    token_ = pool_.FrameArrived([frameEvent](auto&&, auto&&) { SetEvent(frameEvent); });
    session_ = pool_.CreateCaptureSession(item);
    session_.IsCursorCaptureEnabled(false);
    try {
        session_.IsBorderRequired(false); // no yellow border
    } catch (const winrt::hresult_error& e) {
        Log(L"capture: IsBorderRequired(false) rejected: 0x%08X", static_cast<unsigned>(e.code()));
    }
    session_.StartCapture();
    Log(L"capture: monitor %dx%d started (Windows.Graphics.Capture)", size_.Width, size_.Height);
}

MonitorCapture::~MonitorCapture()
{
    Release();
    if (pool_)
        pool_.FrameArrived(token_);
    if (session_)
        session_.Close();
    if (pool_)
        pool_.Close();
}

ID3D11Texture2D* MonitorCapture::Acquire()
{
    Release();
    wgc::Direct3D11CaptureFrame latest{nullptr};
    while (auto f = pool_.TryGetNextFrame()) {
        if (latest)
            latest.Close();
        latest = f;
    }
    if (!latest)
        return nullptr;
    auto cs = latest.ContentSize();
    if (cs.Width != size_.Width || cs.Height != size_.Height) {
        // Resolution changed: later frames come at the new size.
        size_ = cs;
        pool_.Recreate(device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, kBuffers, size_);
        Log(L"capture: monitor resized to %dx%d", size_.Width, size_.Height);
    }
    frame_ = latest;
    auto access = frame_.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture_.put())));
    return texture_.get();
}

void MonitorCapture::Release()
{
    texture_ = nullptr;
    if (frame_) {
        frame_.Close();
        frame_ = nullptr;
    }
}

// ---- DXGI Desktop Duplication -------------------------------------------------

bool DuplicationCapture::Temporary(HRESULT why)
{
    // The secure desktop (UAC, lock) or a disconnected session: it comes back.
    return why == E_ACCESSDENIED || why == DXGI_ERROR_SESSION_DISCONNECTED || why == DXGI_ERROR_ACCESS_LOST;
}

std::unique_ptr<DuplicationCapture> DuplicationCapture::Create(const Gpu& gpu, HMONITOR monitor, HANDLE frameEvent, HRESULT& why, bool& elsewhere)
{
    why = DXGI_ERROR_NOT_FOUND;
    elsewhere = false;
    // The monitor's output as DXGI lists it (on hybrid laptops only under the
    // GPU the process runs on); it must be on the device's adapter.
    winrt::com_ptr<IDXGIDevice> dxgi = gpu.device.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> mine;
    if (FAILED(why = dxgi->GetAdapter(mine.put())))
        return nullptr;
    DXGI_ADAPTER_DESC md{};
    mine->GetDesc(&md);
    winrt::com_ptr<IDXGIOutput> output, o;
    for (UINT i = 0; !output && mine->EnumOutputs(i, o.put()) == S_OK; ++i, o = nullptr) {
        DXGI_OUTPUT_DESC d{};
        if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == monitor)
            output = o;
    }
    if (!output) {
        // Where DXGI lists it instead (the process was placed on another GPU).
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT a = 0; !elsewhere && gpu.factory.as<IDXGIFactory1>()->EnumAdapters1(a, adapter.put()) == S_OK; ++a, adapter = nullptr) {
            DXGI_ADAPTER_DESC1 ad{};
            adapter->GetDesc1(&ad);
            for (UINT i = 0; adapter->EnumOutputs(i, o.put()) == S_OK; ++i, o = nullptr) {
                DXGI_OUTPUT_DESC d{};
                if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == monitor) {
                    elsewhere = true;
                    Log(L"capture: DXGI lists the bar's monitor under %s, not under %s that drives it", ad.Description, md.Description);
                    break;
                }
            }
        }
    }
    if (!output) {
        why = DXGI_ERROR_NOT_FOUND;
        return nullptr;
    }
    // Plain 8-bit BGRA, as the glass reads Windows.Graphics.Capture's frames
    // (an HDR desktop is converted by DXGI).
    winrt::com_ptr<IDXGIOutputDuplication> dup;
    if (auto o5 = output.try_as<IDXGIOutput5>()) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        why = o5->DuplicateOutput1(gpu.device.get(), 0, 1, formats, dup.put());
    } else {
        why = output.as<IDXGIOutput1>()->DuplicateOutput(gpu.device.get(), dup.put());
    }
    if (FAILED(why))
        return nullptr;
    DXGI_OUTDUPL_DESC desc{};
    dup->GetDesc(&desc);
    if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        why = DXGI_ERROR_UNSUPPORTED; // a rotated screen comes unrotated; the capture's frames fit the glass as they are
        return nullptr;
    }
    // The waiting thread and the render thread both use the device.
    if (auto mt = gpu.context.try_as<ID3D11Multithread>())
        mt->SetMultithreadProtected(TRUE);
    std::unique_ptr<DuplicationCapture> c(new DuplicationCapture());
    c->device_ = gpu.device;
    c->context_ = gpu.context;
    c->dup_ = dup;
    c->frameEvent_ = frameEvent;
    c->released_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    c->stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    c->thread_ = std::thread([p = c.get()] { p->Pump(); });
    Log(L"capture: monitor %ux%u started (DXGI Desktop Duplication)", desc.ModeDesc.Width, desc.ModeDesc.Height);
    why = S_OK;
    return c;
}

DuplicationCapture::~DuplicationCapture()
{
    SetEvent(stop_);
    if (thread_.joinable())
        thread_.join();
    if (ready_) {
        texture_ = nullptr;
        held_ = nullptr;
        dup_->ReleaseFrame();
    }
    CloseHandle(released_);
    CloseHandle(stop_);
}

void DuplicationCapture::Pump()
{
    while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> res;
        // No waiting inside: the device is shared with the render thread
        // (multithread-protected), and a wait in AcquireNextFrame would hold
        // it there, stalling the bar's frames for as long.
        HRESULT hr = dup_->AcquireNextFrame(0, &info, res.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (WaitForSingleObject(stop_, 5) != WAIT_TIMEOUT)
                return;
            continue;
        }
        if (FAILED(hr)) {
            Log(L"capture: duplication stopped (0x%08X)", static_cast<unsigned>(hr));
            lost_ = true;
            SetEvent(frameEvent_);
            return;
        }
        if (info.LastPresentTime.QuadPart == 0) {
            dup_->ReleaseFrame(); // only the pointer moved
            continue;
        }
        held_ = res;
        ready_ = true;
        SetEvent(frameEvent_);
        // Holds the frame until the render thread has copied it.
        HANDLE wait[] = {stop_, released_};
        if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0)
            return;
    }
}

// The acquired frame itself, as Windows.Graphics.Capture's: the glass copies
// only its regions (the bar, the popups); released after them.
ID3D11Texture2D* DuplicationCapture::Acquire()
{
    Release(); // the frame handed out last time
    if (!ready_)
        return nullptr;
    texture_ = held_.try_as<ID3D11Texture2D>();
    if (!texture_) {
        held_ = nullptr;
        ready_ = false;
        dup_->ReleaseFrame();
        SetEvent(released_);
    }
    return texture_.get();
}

void DuplicationCapture::Release()
{
    if (!texture_)
        return; // nothing handed out (a frame may be waiting: it stays)
    texture_ = nullptr;
    held_ = nullptr;
    ready_ = false;
    dup_->ReleaseFrame();
    SetEvent(released_);
}

} // namespace atrium
