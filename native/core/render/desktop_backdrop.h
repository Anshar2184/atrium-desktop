// The backdrop of the desktop widgets (D35, D38): the desktop itself, never
// the windows above it. Two ways:
//  - "desktop": Windows.Graphics.Capture of Explorer's desktop window
//    (Progman). A window capture holds the window's own content even where
//    other windows cover it; frames come only when the desktop changes (a new
//    wallpaper, a slideshow), so the widgets' glass is drawn again only then;
//  - "wallpaper" (the setting widgetBackdrop, and the fallback): the
//    wallpaper files laid out as Windows lays them out (IDesktopWallpaper:
//    fill, fit, stretch, tile, centre, span; the background colour), composed
//    on a worker thread and checked again every few seconds. No capture runs.
// Render thread only, except the worker.
#pragma once

#include "gpu.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

class DesktopBackdrop {
public:
    // `frameEvent` is set when the desktop capture has a new frame.
    DesktopBackdrop(const Gpu& gpu, HANDLE frameEvent);
    ~DesktopBackdrop();
    DesktopBackdrop(const DesktopBackdrop&) = delete;
    DesktopBackdrop& operator=(const DesktopBackdrop&) = delete;

    // "desktop", "wallpaper" or "off" (no widgets: nothing runs); takes the
    // newest frame or picture. Cheap when nothing changed.
    void Update(const std::wstring& mode);
    // The desktop, screen px from Origin(); null until the first frame.
    ID3D11Texture2D* Texture() const { return texture_.get(); }
    POINT Origin() const { return origin_; }
    // Bumped with every new picture.
    unsigned Version() const { return version_; }
    const wchar_t* Source() const;

private:
    bool StartCapture();
    void StopCapture();
    void StartWallpaper();
    void StopWallpaper();
    void Store(ID3D11Texture2D* frame);
    void WallpaperLoop(); // worker

    const Gpu& gpu_;
    HANDLE frameEvent_;
    std::wstring mode_;
    winrt::com_ptr<ID3D11Texture2D> texture_;
    POINT origin_{};
    unsigned version_ = 0;

    // The desktop window's capture.
    HWND desktop_ = nullptr;
    double retryMs_ = 0; // when to try the capture again (Explorer restarting)
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::SizeInt32 size_{};
    std::atomic<bool> closed_{false};

    // The wallpaper, composed by the worker (BGRA, premultiplied, opaque).
    struct Picture {
        int width = 0, height = 0;
        POINT origin{};
        std::vector<uint8_t> pixels;
    };
    std::thread worker_;
    HANDLE stop_ = nullptr;
    std::mutex pictureLock_;
    std::unique_ptr<Picture> picture_; // new, not yet taken
};

} // namespace atrium
