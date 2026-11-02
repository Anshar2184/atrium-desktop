#include "desktop_backdrop.h"

#include "../log.h"
#include "../clock.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cmath>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace atrium {

namespace {

constexpr double kRetryMs = 2000;   // the desktop window is gone (Explorer restarting)
constexpr DWORD kWallpaperPollMs = 3000;

std::wstring TranscodedWallpaper()
{
    PWSTR roaming = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)))
        p = std::wstring(roaming) + L"\\Microsoft\\Windows\\Themes\\TranscodedWallpaper";
    CoTaskMemFree(roaming);
    return p;
}

std::wstring FileStamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return L"-";
    return std::to_wstring(a.ftLastWriteTime.dwLowDateTime) + L"." + std::to_wstring(a.nFileSizeLow);
}

// One monitor's wallpaper as Windows lays it out, drawn into `canvas`
// (BGRA, `stride` bytes a row) at `target` (canvas px).
bool Place(IWICImagingFactory* wic, const std::wstring& path, RECT target, DESKTOP_WALLPAPER_POSITION pos, uint8_t* canvas, int stride)
{
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT iw = 0, ih = 0;
    converter->GetSize(&iw, &ih);
    int tw = target.right - target.left, th = target.bottom - target.top;
    if (!iw || !ih || tw <= 0 || th <= 0)
        return false;
    double sx = 1, sy = 1;
    switch (pos) {
    case DWPOS_FILL:
    case DWPOS_SPAN:
        sx = sy = std::max(double(tw) / iw, double(th) / ih);
        break;
    case DWPOS_FIT:
        sx = sy = std::min(double(tw) / iw, double(th) / ih);
        break;
    case DWPOS_STRETCH:
        sx = double(tw) / iw;
        sy = double(th) / ih;
        break;
    default: // centre, tile: the picture's own size
        break;
    }
    UINT sw = std::max(1u, UINT(std::lround(iw * sx))), sh = std::max(1u, UINT(std::lround(ih * sy)));
    winrt::com_ptr<IWICBitmapSource> source = converter.as<IWICBitmapSource>();
    if (sw != iw || sh != ih) {
        winrt::com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(scaler.put())) || FAILED(scaler->Initialize(converter.get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        source = scaler.as<IWICBitmapSource>();
    }
    // Copies the picture placed at (dx, dy) of the target, clipped to it.
    auto blit = [&](int dx, int dy) {
        int x0 = std::max(0, dx), y0 = std::max(0, dy), x1 = std::min(tw, dx + int(sw)), y1 = std::min(th, dy + int(sh));
        if (x1 <= x0 || y1 <= y0)
            return true;
        WICRect rc{x0 - dx, y0 - dy, x1 - x0, y1 - y0};
        uint8_t* at = canvas + size_t(target.top + y0) * size_t(stride) + size_t(target.left + x0) * 4;
        return SUCCEEDED(source->CopyPixels(&rc, UINT(stride), UINT(size_t(stride) * size_t(y1 - y0 - 1) + size_t(x1 - x0) * 4), at));
    };
    if (pos == DWPOS_TILE) {
        for (int y = 0; y < th; y += int(sh))
            for (int x = 0; x < tw; x += int(sw))
                if (!blit(x, y))
                    return false;
        return true;
    }
    return blit((tw - int(sw)) / 2, (th - int(sh)) / 2);
}

} // namespace

DesktopBackdrop::DesktopBackdrop(const Gpu& gpu, HANDLE frameEvent) : gpu_(gpu), frameEvent_(frameEvent)
{
    auto dxgi = gpu.device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    device_ = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();
}

DesktopBackdrop::~DesktopBackdrop()
{
    StopCapture();
    StopWallpaper();
}

const wchar_t* DesktopBackdrop::Source() const
{
    return session_ ? L"desktop window" : worker_.joinable() ? L"wallpaper" : L"none";
}

void DesktopBackdrop::Update(const std::wstring& mode)
{
    const std::wstring want = mode == L"wallpaper" || mode == L"off" ? mode : L"desktop";
    if (want != mode_) {
        StopCapture();
        StopWallpaper();
        if (mode_ == L"off" || want == L"off" || want == L"wallpaper")
            texture_ = nullptr; // not a picture of what comes next
        mode_ = want;
        retryMs_ = 0;
        if (mode_ == L"wallpaper")
            StartWallpaper();
    }
    if (mode_ == L"desktop") {
        if (session_ && (closed_ || !IsWindow(desktop_))) {
            Log(L"render: the desktop window went away; capturing it again soon");
            StopCapture();
            retryMs_ = ClockMs() + kRetryMs;
        }
        if (!session_ && ClockMs() >= retryMs_) {
            if (StartCapture()) {
                StopWallpaper();
            } else {
                retryMs_ = ClockMs() + kRetryMs;
                if (!worker_.joinable())
                    StartWallpaper(); // meanwhile, the wallpaper files
            }
        }
        if (session_) {
            wgc::Direct3D11CaptureFrame latest{nullptr};
            while (auto f = pool_.TryGetNextFrame()) {
                if (latest)
                    latest.Close();
                latest = f;
            }
            if (latest) {
                auto cs = latest.ContentSize();
                if (cs.Width != size_.Width || cs.Height != size_.Height) {
                    size_ = cs; // the desktop changed size: later frames come at the new one
                    pool_.Recreate(device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size_);
                }
                winrt::com_ptr<ID3D11Texture2D> tex;
                auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                    RECT r{};
                    GetWindowRect(desktop_, &r);
                    origin_ = POINT{r.left, r.top};
                    Store(tex.get());
                }
                latest.Close();
            }
        }
    }
    std::unique_ptr<Picture> pic;
    {
        std::lock_guard g(pictureLock_);
        pic = std::move(picture_);
    }
    if (pic && (mode_ == L"wallpaper" || !session_)) {
        D3D11_TEXTURE2D_DESC d{};
        if (texture_)
            texture_->GetDesc(&d);
        if (!texture_ || int(d.Width) != pic->width || int(d.Height) != pic->height) {
            texture_ = nullptr;
            D3D11_TEXTURE2D_DESC nd{UINT(pic->width), UINT(pic->height), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
            if (FAILED(gpu_.device->CreateTexture2D(&nd, nullptr, texture_.put())))
                return;
        }
        gpu_.context->UpdateSubresource(texture_.get(), 0, nullptr, pic->pixels.data(), UINT(pic->width * 4), 0);
        origin_ = pic->origin;
        ++version_;
    }
}

bool DesktopBackdrop::StartCapture()
{
    desktop_ = FindWindowW(L"Progman", nullptr);
    if (!desktop_)
        return false;
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(desktop_, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_)));
        size_ = item_.Size();
        closed_ = false;
        item_.Closed([this](auto&&, auto&&) {
            closed_ = true;
            SetEvent(frameEvent_);
        });
        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size_);
        HANDLE ev = frameEvent_;
        pool_.FrameArrived([ev](auto&&, auto&&) { SetEvent(ev); });
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(false);
        try {
            session_.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) {
        }
        session_.StartCapture();
    } catch (const winrt::hresult_error& e) {
        Log(L"render: the desktop window cannot be captured (0x%08X); widgets use the wallpaper", static_cast<unsigned>(e.code()));
        StopCapture();
        return false;
    }
    Log(L"render: widgets' backdrop: the desktop window, %dx%d", size_.Width, size_.Height);
    return true;
}

void DesktopBackdrop::StopCapture()
{
    if (session_)
        session_.Close();
    if (pool_)
        pool_.Close();
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
    closed_ = false;
}

void DesktopBackdrop::Store(ID3D11Texture2D* frame)
{
    D3D11_TEXTURE2D_DESC fd{}, have{};
    frame->GetDesc(&fd);
    UINT w = std::min(fd.Width, UINT(size_.Width)), h = std::min(fd.Height, UINT(size_.Height));
    if (texture_)
        texture_->GetDesc(&have);
    if (!texture_ || have.Width != w || have.Height != h) {
        texture_ = nullptr;
        D3D11_TEXTURE2D_DESC d{w, h, 1, 1, fd.Format, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        if (FAILED(gpu_.device->CreateTexture2D(&d, nullptr, texture_.put())))
            return;
    }
    D3D11_BOX box{0, 0, 0, w, h, 1};
    gpu_.context->CopySubresourceRegion(texture_.get(), 0, 0, 0, 0, frame, 0, &box);
    ++version_;
}

void DesktopBackdrop::StartWallpaper()
{
    if (worker_.joinable())
        return;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_ = std::thread([this] { WallpaperLoop(); });
    Log(L"render: widgets' backdrop: the wallpaper");
}

void DesktopBackdrop::StopWallpaper()
{
    if (!worker_.joinable())
        return;
    SetEvent(stop_);
    worker_.join();
    CloseHandle(stop_);
    stop_ = nullptr;
    std::lock_guard g(pictureLock_);
    picture_ = nullptr;
}

void DesktopBackdrop::WallpaperLoop()
{
    SetThreadDescription(GetCurrentThread(), L"Atrium wallpaper");
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return;
    std::wstring composed; // what the last picture was made of
    do {
        winrt::com_ptr<IDesktopWallpaper> dw;
        winrt::com_ptr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(dw.put()))) ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))))
            continue;
        DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
        COLORREF bg = 0;
        dw->GetPosition(&pos);
        dw->GetBackgroundColor(&bg);
        struct Screen {
            RECT rect;
            std::wstring path;
        };
        std::vector<Screen> screens;
        UINT n = 0;
        dw->GetMonitorDevicePathCount(&n);
        for (UINT i = 0; i < n; ++i) {
            LPWSTR id = nullptr, path = nullptr;
            RECT r{};
            if (SUCCEEDED(dw->GetMonitorDevicePathAt(i, &id)) && SUCCEEDED(dw->GetMonitorRECT(id, &r)) && r.right > r.left) {
                dw->GetWallpaper(id, &path);
                std::wstring p = path ? path : L"";
                if (p.empty() || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
                    p = TranscodedWallpaper(); // the picture Windows shows (Spotlight, a file moved away)
                screens.push_back({r, p});
            }
            CoTaskMemFree(id);
            CoTaskMemFree(path);
        }
        RECT virt{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        std::wstring sig = std::to_wstring(int(pos)) + L"|" + std::to_wstring(bg) + L"|" + std::to_wstring(virt.left) + L"," +
                           std::to_wstring(virt.top) + L"," + std::to_wstring(virt.right) + L"," + std::to_wstring(virt.bottom);
        for (const Screen& s : screens)
            sig += L"|" + std::to_wstring(s.rect.left) + L"," + std::to_wstring(s.rect.top) + L"," + std::to_wstring(s.rect.right) + L"," +
                   std::to_wstring(s.rect.bottom) + L":" + s.path + L"@" + FileStamp(s.path);
        if (sig == composed)
            continue;
        auto pic = std::make_unique<Picture>();
        pic->width = virt.right - virt.left;
        pic->height = virt.bottom - virt.top;
        pic->origin = POINT{virt.left, virt.top};
        if (pic->width <= 0 || pic->height <= 0)
            continue;
        pic->pixels.resize(size_t(pic->width) * size_t(pic->height) * 4);
        for (size_t i = 0; i < pic->pixels.size(); i += 4) {
            pic->pixels[i] = GetBValue(bg);
            pic->pixels[i + 1] = GetGValue(bg);
            pic->pixels[i + 2] = GetRValue(bg);
            pic->pixels[i + 3] = 255;
        }
        bool ok = true;
        auto local = [&](RECT r) { return RECT{r.left - virt.left, r.top - virt.top, r.right - virt.left, r.bottom - virt.top}; };
        if (pos == DWPOS_SPAN && !screens.empty())
            ok = Place(wic.get(), screens[0].path, local(virt), pos, pic->pixels.data(), pic->width * 4);
        else
            for (const Screen& s : screens)
                ok &= Place(wic.get(), s.path, local(s.rect), pos, pic->pixels.data(), pic->width * 4);
        for (size_t i = 3; i < pic->pixels.size(); i += 4)
            pic->pixels[i] = 255; // opaque, as the desktop is
        composed = sig;
        Log(L"render: wallpaper composed %dx%d (%zu screens, position %d)%s", pic->width, pic->height, screens.size(), int(pos),
            ok ? L"" : L", a picture could not be read");
        {
            std::lock_guard g(pictureLock_);
            picture_ = std::move(pic);
        }
        SetEvent(frameEvent_);
    } while (WaitForSingleObject(stop_, kWallpaperPollMs) == WAIT_TIMEOUT);
    CoUninitialize();
}

} // namespace atrium
