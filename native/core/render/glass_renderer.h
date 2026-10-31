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

#include <deque>
#include <functional>
#include <optional>
#include <vector>

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

// Result of one backdrop comparison, read back without stalling the CPU.
struct Readback {
    bool changed = false; // ROI differed from the last presented ROI
    // Backdrop luminance (0..1) on a coarse grid over the ROI; one cell covers
    // cellX x cellY ROI pixels. The ROI is the window plus GlassRenderer::kPad.
    std::vector<float> luma;
    int gridW = 0, gridH = 0;
    float cellX = 1, cellY = 1;
    // Mean luminance over a rectangle in ROI pixels.
    float RegionLuma(float x0, float y0, float x1, float y1) const;
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

    // Copies the backdrop under the surface (monitor-relative px) into the ROI
    // and queues its comparison with the ROI used for the last present (D04).
    void UpdateBackdrop(ID3D11Texture2D* monitorFrame, int x, int y);
    bool HasPresented() const { return hasPresented_; }
    // Completed comparisons since the last call, merged (changed = any changed,
    // colour = newest), or nullopt if none finished yet. Never waits.
    std::optional<Readback> Poll();
    bool ReadbackPending() const { return !inFlight_.empty(); }

    // Draws the glass, then `overlay` with D2D on the same back buffer, and
    // presents. Returns false if the device was lost.
    bool Present(const GlassParams& params, const std::function<void(ID2D1DeviceContext2*)>& overlay);

    ID2D1DeviceContext2* D2D() const { return d2d_.get(); }
    IDWriteFactory3* DWrite() const { return dwrite_.get(); }

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
    // Readback ring: comparison counters and a small mip (luminance grid).
    struct Slot {
        winrt::com_ptr<ID3D11Buffer> counters;
        winrt::com_ptr<ID3D11Texture2D> grid;
    };
    static constexpr int kSlots = 3;
    Slot slots_[kSlots];
    std::deque<int> inFlight_;
    int nextSlot_ = 0;
    UINT gridMip_ = 0;
    int gridW_ = 1, gridH_ = 1;
    bool ReadSlot(int slot, bool wait, Readback& out);

    winrt::com_ptr<ID3D11Buffer> cb_, cmpCb_, cmpOut_;
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
