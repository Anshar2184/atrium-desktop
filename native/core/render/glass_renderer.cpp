#include "glass_renderer.h"

#include "compare_cs.h"
#include "glass_ps.h"
#include "glass_vs.h"

#include <algorithm>

namespace atrium {

namespace {

constexpr UINT kCompareGroup = 16;

void Check(HRESULT hr) { winrt::check_hresult(hr); }

} // namespace

GlassRenderer::GlassRenderer(const Gpu& gpu, HWND hwnd, int width, int height) : gpu_(gpu), w_(width), h_(height)
{
    auto* dev = gpu_.device.get();
    Check(dev->CreateVertexShader(g_GlassVS, sizeof(g_GlassVS), nullptr, vs_.put()));
    Check(dev->CreatePixelShader(g_GlassPS, sizeof(g_GlassPS), nullptr, ps_.put()));
    Check(dev->CreateComputeShader(g_CompareCS, sizeof(g_CompareCS), nullptr, cs_.put()));

    D3D11_BUFFER_DESC cbd{sizeof(GlassParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    Check(dev->CreateBuffer(&cbd, nullptr, cb_.put()));
    D3D11_BUFFER_DESC ccbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    Check(dev->CreateBuffer(&ccbd, nullptr, cmpCb_.put()));
    // Out[0] changed pixels, Out[1] max difference (per-tile slots unused here).
    D3D11_BUFFER_DESC ob{4 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
    Check(dev->CreateBuffer(&ob, nullptr, cmpOut_.put()));
    D3D11_BUFFER_DESC sb{4 * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    Check(dev->CreateBuffer(&sb, nullptr, cmpStaging_.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 4;
    Check(dev->CreateUnorderedAccessView(cmpOut_.get(), &ud, cmpUav_.put()));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    Check(dev->CreateSamplerState(&sd, sampler_.put()));
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Check(dev->CreateBlendState(&bd, blend_.put()));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(w_);
    desc.Height = UINT(h_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    Check(gpu_.factory->CreateSwapChainForComposition(dev, &desc, nullptr, swap_.put()));

    auto dxgi = gpu_.device.as<IDXGIDevice>();
    Check(DCompositionCreateDevice(dxgi.get(), IID_PPV_ARGS(dcomp_.put())));
    Check(dcomp_->CreateTargetForHwnd(hwnd, TRUE, target_.put()));
    Check(dcomp_->CreateVisual(visual_.put()));
    Check(visual_->SetContent(swap_.get()));
    Check(target_->SetRoot(visual_.get()));
    Check(dcomp_->Commit());

    D2D1_FACTORY_OPTIONS fo{};
    Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &fo, d2dFactory_.put_void()));
    Check(d2dFactory_->CreateDevice(dxgi.get(), d2dDevice_.put()));
    Check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put()));
    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(dwrite_.put())));

    CreateSizeDependent();
}

void GlassRenderer::CreateSizeDependent()
{
    auto* dev = gpu_.device.get();
    UINT rw = UINT(w_ + 2 * kPad), rh = UINT(h_ + 2 * kPad);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = rw;
    td.Height = rh;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    roi_ = nullptr;
    roiSrv_ = nullptr;
    Check(dev->CreateTexture2D(&td, nullptr, roi_.put()));
    Check(dev->CreateShaderResourceView(roi_.get(), nullptr, roiSrv_.put()));

    td.MipLevels = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = 0;
    presented_ = nullptr;
    presentedSrv_ = nullptr;
    Check(dev->CreateTexture2D(&td, nullptr, presented_.put()));
    Check(dev->CreateShaderResourceView(presented_.get(), nullptr, presentedSrv_.put()));

    const UINT cp[4] = {rw, rh, 1, 1u << 30}; // one tile covering everything
    gpu_.context->UpdateSubresource(cmpCb_.get(), 0, nullptr, cp, 0, 0);
    hasPresented_ = false;
    hasBackdrop_ = false;
}

void GlassRenderer::Resize(int width, int height)
{
    if (width == w_ && height == h_)
        return;
    w_ = width;
    h_ = height;
    d2d_->SetTarget(nullptr);
    Check(swap_->ResizeBuffers(2, UINT(w_), UINT(h_), DXGI_FORMAT_B8G8R8A8_UNORM, 0));
    CreateSizeDependent();
}

void GlassRenderer::UpdateBackdrop(ID3D11Texture2D* frame, int x, int y)
{
    D3D11_TEXTURE2D_DESC fd{};
    frame->GetDesc(&fd);
    int x0 = x - kPad, y0 = y - kPad, x1 = x + w_ + kPad, y1 = y + h_ + kPad;
    int cx0 = std::max(0, x0), cy0 = std::max(0, y0), cx1 = std::min(int(fd.Width), x1), cy1 = std::min(int(fd.Height), y1);
    if (cx1 <= cx0 || cy1 <= cy0)
        return;
    D3D11_BOX box{UINT(cx0), UINT(cy0), 0, UINT(cx1), UINT(cy1), 1};
    gpu_.context->CopySubresourceRegion(roi_.get(), 0, UINT(cx0 - x0), UINT(cy0 - y0), 0, frame, 0, &box);
    gpu_.context->GenerateMips(roiSrv_.get());
    hasBackdrop_ = true;
}

bool GlassRenderer::BackdropChanged()
{
    if (!hasPresented_)
        return true;
    auto* ctx = gpu_.context.get();
    const UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(cmpUav_.get(), zero);
    ctx->CSSetShader(cs_.get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {roiSrv_.get(), presentedSrv_.get()};
    ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uav = cmpUav_.get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ID3D11Buffer* cb = cmpCb_.get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->Dispatch(UINT(w_ + 2 * kPad + kCompareGroup - 1) / kCompareGroup, UINT(h_ + 2 * kPad + kCompareGroup - 1) / kCompareGroup, 1);
    ID3D11ShaderResourceView* noSrv[2] = {};
    ctx->CSSetShaderResources(0, 2, noSrv);
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    ctx->CopyResource(cmpStaging_.get(), cmpOut_.get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cmpStaging_.get(), 0, D3D11_MAP_READ, 0, &m)))
        return true;
    bool changed = static_cast<const UINT*>(m.pData)[0] != 0;
    ctx->Unmap(cmpStaging_.get(), 0);
    return changed;
}

bool GlassRenderer::Present(const GlassParams& in, const TextOverlay& text)
{
    auto* ctx = gpu_.context.get();
    GlassParams p = in;
    p.sizeX = float(w_);
    p.sizeY = float(h_);
    p.roiOriginX = p.roiOriginY = float(kPad);
    p.roiSizeX = float(w_ + 2 * kPad);
    p.roiSizeY = float(h_ + 2 * kPad);
    ctx->UpdateSubresource(cb_.get(), 0, nullptr, &p, 0, 0);

    winrt::com_ptr<ID3D11Texture2D> back;
    Check(swap_->GetBuffer(0, IID_PPV_ARGS(back.put())));
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    Check(gpu_.device->CreateRenderTargetView(back.get(), nullptr, rtv.put()));
    ID3D11RenderTargetView* rt = rtv.get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->OMSetBlendState(blend_.get(), nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(ps_.get(), nullptr, 0);
    ID3D11Buffer* cb = cb_.get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv = roiSrv_.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* s = sampler_.get();
    ctx->PSSetSamplers(0, 1, &s);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    if (!text.primary.empty()) {
        auto surface = back.as<IDXGISurface>();
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> target;
        Check(d2d_->CreateBitmapFromDxgiSurface(surface.get(), &bp, target.put()));
        d2d_->SetTarget(target.get());
        d2d_->BeginDraw();
        winrt::com_ptr<ID2D1SolidColorBrush> fg, shade;
        d2d_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.96f), fg.put());
        d2d_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.30f), shade.put());
        auto draw = [&](const std::wstring& s, float px, float top, float bottom, DWRITE_FONT_WEIGHT weight) {
            winrt::com_ptr<IDWriteTextFormat> f;
            if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, px, L"", f.put())))
                return;
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            float o = std::max(1.0f, px / 14);
            d2d_->DrawText(s.c_str(), UINT32(s.size()), f.get(), D2D1::RectF(o, top + o, float(w_) + o, bottom + o), shade.get());
            d2d_->DrawText(s.c_str(), UINT32(s.size()), f.get(), D2D1::RectF(0, top, float(w_), bottom), fg.get());
        };
        float mid = float(h_) / 2;
        if (text.secondary.empty()) {
            draw(text.primary, text.primaryPx, 0, float(h_), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        } else {
            float total = text.primaryPx * 1.25f + text.secondaryPx * 1.25f;
            float top = mid - total / 2;
            draw(text.primary, text.primaryPx, top, top + text.primaryPx * 1.25f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            draw(text.secondary, text.secondaryPx, top + text.primaryPx * 1.25f, top + total, DWRITE_FONT_WEIGHT_NORMAL);
        }
        HRESULT hr = d2d_->EndDraw();
        d2d_->SetTarget(nullptr);
        if (hr == D2DERR_RECREATE_TARGET)
            return false;
    }

    ctx->CopySubresourceRegion(presented_.get(), 0, 0, 0, 0, roi_.get(), 0, nullptr);
    hasPresented_ = hasBackdrop_;
    HRESULT hr = swap_->Present(1, 0);
    return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

} // namespace atrium
