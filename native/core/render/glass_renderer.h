// Renders the glass material for one surface: copies a padded backdrop ROI from
// the capture, builds mips for the blur, draws one full-window triangle and the
// text overlay into a premultiplied composition swap chain hosted by
// DirectComposition. Exact echo detection compares the ROI with the ROI that
// was last presented (VERIFICATION 1.7).
#pragma once

#include "gpu.h"

#include <d2d1_3.h>
#include <dcomp.h>
#include <dwrite_3.h>

#include <string>

namespace atrium {

// Mirrors cbuffer GlassParams in shaders/glass.hlsl (96 bytes). Lengths in px.
struct GlassParams {
    float sizeX, sizeY, roiOriginX, roiOriginY;
    float roiSizeX, roiSizeY, radius, edge;
    float refract, dispersion, blurLod, tintMix;
    float tintR, tintG, tintB, tintA;
    float highlight, shadow, lightX, lightY;
    float inset, blurMix, pad0, pad1;
};
static_assert(sizeof(GlassParams) == 96);

struct TextOverlay {
    std::wstring primary;   // e.g. the time
    std::wstring secondary; // e.g. the date, drawn under the primary line
    float primaryPx = 0;
    float secondaryPx = 0;
};

class GlassRenderer {
public:
    static constexpr int kPad = 64; // backdrop margin for refraction and blur

    GlassRenderer(const Gpu& gpu, HWND hwnd, int width, int height);
    GlassRenderer(const GlassRenderer&) = delete;
    GlassRenderer& operator=(const GlassRenderer&) = delete;

    void Resize(int width, int height);
    int Width() const { return w_; }
    int Height() const { return h_; }

    // Copies the backdrop under the surface (monitor-relative px) into the ROI.
    void UpdateBackdrop(ID3D11Texture2D* monitorFrame, int x, int y);
    // True if the ROI differs from the one used for the last present.
    bool BackdropChanged();
    // Draws and presents; returns false if the device was lost.
    bool Present(const GlassParams& params, const TextOverlay& text);

private:
    void CreateSizeDependent();

    Gpu gpu_;
    int w_, h_;
    winrt::com_ptr<IDXGISwapChain1> swap_;
    winrt::com_ptr<IDCompositionDevice> dcomp_;
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11ComputeShader> cs_;
    winrt::com_ptr<ID3D11Buffer> cb_, cmpCb_, cmpOut_, cmpStaging_;
    winrt::com_ptr<ID3D11UnorderedAccessView> cmpUav_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> blend_;
    winrt::com_ptr<ID3D11Texture2D> roi_, presented_;
    winrt::com_ptr<ID3D11ShaderResourceView> roiSrv_, presentedSrv_;
    bool hasPresented_ = false;
    bool hasBackdrop_ = false;

    winrt::com_ptr<ID2D1Factory3> d2dFactory_;
    winrt::com_ptr<ID2D1Device2> d2dDevice_;
    winrt::com_ptr<ID2D1DeviceContext2> d2d_;
    winrt::com_ptr<IDWriteFactory3> dwrite_;
};

} // namespace atrium
