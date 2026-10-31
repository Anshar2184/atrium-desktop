#include "ui/surface.h"

#include "log.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "dcomp.lib")

namespace atrium {

namespace {

constexpr wchar_t kClass[] = L"AtriumSurface";

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }

bool SystemLight()
{
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

} // namespace

Theme Theme::Current()
{
    Theme t;
    t.light = SystemLight();
    if (t.light) {
        t.panel = Rgba(0xF3F3F6, 0.975f);
        t.border = Rgba(0x000000, 0.10f);
        t.text = Rgba(0x16181D, 0.95f);
        t.subtext = Rgba(0x16181D, 0.62f);
        t.faint = Rgba(0x16181D, 0.40f);
        t.hover = Rgba(0x000000, 0.055f);
        t.press = Rgba(0x000000, 0.09f);
        t.divider = Rgba(0x000000, 0.08f);
    } else {
        t.panel = Rgba(0x202127, 0.97f);
        t.border = Rgba(0xFFFFFF, 0.10f);
        t.text = Rgba(0xFFFFFF, 0.95f);
        t.subtext = Rgba(0xFFFFFF, 0.62f);
        t.faint = Rgba(0xFFFFFF, 0.40f);
        t.hover = Rgba(0xFFFFFF, 0.07f);
        t.press = Rgba(0xFFFFFF, 0.11f);
        t.divider = Rgba(0xFFFFFF, 0.08f);
    }
    t.accent = Rgba(0x0A84FF, 1);
    t.accentText = Rgba(0xFFFFFF, 1);
    return t;
}

// ---- Canvas ------------------------------------------------------------------

D2D1_RECT_F Canvas::Px(D2D1_RECT_F r) const { return D2D1::RectF(r.left * scale, r.top * scale, r.right * scale, r.bottom * scale); }

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F c)
{
    if (!s_.brush_)
        d2d_->CreateSolidColorBrush(c, s_.brush_.put());
    else
        s_.brush_->SetColor(c);
    return s_.brush_.get();
}

void Canvas::Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c)
{
    if (radius > 0)
        d2d_->FillRoundedRectangle(D2D1::RoundedRect(Px(r), radius * scale, radius * scale), Brush(c));
    else
        d2d_->FillRectangle(Px(r), Brush(c));
}

void Canvas::Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width)
{
    float h = width * scale / 2;
    D2D1_RECT_F p = Px(r);
    p = D2D1::RectF(p.left + h, p.top + h, p.right - h, p.bottom - h);
    d2d_->DrawRoundedRectangle(D2D1::RoundedRect(p, std::max(0.f, radius * scale - h), std::max(0.f, radius * scale - h)), Brush(c), width * scale);
}

void Canvas::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width)
{
    d2d_->DrawLine(D2D1::Point2F(x0 * scale, y0 * scale), D2D1::Point2F(x1 * scale, y1 * scale), Brush(c), width * scale);
}

void Canvas::Circle(float cx, float cy, float radius, D2D1_COLOR_F c)
{
    d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx * scale, cy * scale), radius * scale, radius * scale), Brush(c));
}

IDWriteTextFormat* Canvas::Format(float px, DWRITE_FONT_WEIGHT w, bool glyph, int align, bool wrap)
{
    auto key = std::make_tuple(int(std::lround(px * 8)), int(w), glyph, align, wrap);
    auto& f = s_.formats_[key];
    if (!f) {
        s_.dwrite_->CreateTextFormat(glyph ? L"Segoe Fluent Icons" : L"Segoe UI Variable Text", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, px, L"", f.put());
        if (!f)
            return nullptr;
        f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!glyph) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            winrt::com_ptr<IDWriteInlineObject> ellipsis;
            s_.dwrite_->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
    }
    return f.get();
}

void Canvas::Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w, int align, bool wrap)
{
    if (s.empty())
        return;
    if (IDWriteTextFormat* f = Format(size * scale, w, false, align, wrap))
        d2d_->DrawText(s.c_str(), UINT32(s.size()), f, Px(r), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Canvas::Glyph(const std::wstring& g, D2D1_RECT_F r, float size, D2D1_COLOR_F c)
{
    if (IDWriteTextFormat* f = Format(size * scale, DWRITE_FONT_WEIGHT_NORMAL, true, 1, false))
        d2d_->DrawText(g.c_str(), UINT32(g.size()), f, Px(r), Brush(c));
}

float Canvas::TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w)
{
    IDWriteTextFormat* f = Format(size * scale, w, false, 0, false);
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!f || FAILED(s_.dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), f, 100000, 1000, layout.put())))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace / scale;
}

void Canvas::Image(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, float opacity)
{
    auto& b = s_.bitmaps_[key];
    if (!b) {
        if (image.empty())
            return;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d_->CreateBitmap(D2D1::SizeU(UINT32(image.width), UINT32(image.height)), image.bgra.data(), UINT32(image.width * 4), &bp,
                                      b.put())))
            return;
    }
    d2d_->DrawBitmap(b.get(), Px(r), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Canvas::Clip(D2D1_RECT_F r) { d2d_->PushAxisAlignedClip(Px(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::Unclip() { d2d_->PopAxisAlignedClip(); }

// ---- Surface -----------------------------------------------------------------

struct Surface::Shared {
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<IDXGIFactory2> dxgi;
    winrt::com_ptr<IDCompositionDevice> dcomp;
    winrt::com_ptr<ID2D1Factory1> d2dFactory;
    winrt::com_ptr<ID2D1Device> d2d;
    bool lost = false;
};

std::shared_ptr<Surface::Shared> Surface::SharedDevice()
{
    static std::weak_ptr<Shared> cache;
    if (auto s = cache.lock(); s && !s->lost)
        return s;
    auto s = std::make_shared<Shared>();
    // WARP (software): popups are small 2D content, and a second hardware
    // device costs 70-100 MB of driver memory and wakes the GPU (V16).
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                            D3D11_SDK_VERSION, s->d3d.put(), nullptr, nullptr))) {
        Log(L"surface: no D3D11 device");
        return nullptr;
    }
    auto dxgiDevice = s->d3d.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(adapter.put());
    adapter->GetParent(IID_PPV_ARGS(s->dxgi.put()));
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, s->d2dFactory.put_void())) ||
        FAILED(s->d2dFactory->CreateDevice(dxgiDevice.get(), s->d2d.put())) ||
        FAILED(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(s->dcomp.put())))) {
        Log(L"surface: device setup failed");
        return nullptr;
    }
    cache = s;
    return s;
}

Surface::Surface(HINSTANCE instance, const wchar_t* title, bool activatable) : activatable_(activatable)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.lpfnWndProc = Proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | (activatable ? 0 : WS_EX_NOACTIVATE);
    hwnd_ = CreateWindowExW(ex, kClass, title, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put()));
}

Surface::~Surface()
{
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

float Surface::Scale() const { return float(GetDpiForWindow(hwnd_)) / 96.f; }

void Surface::ExcludeFromCapture(bool on) { SetWindowDisplayAffinity(hwnd_, on ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE); }

LRESULT CALLBACK Surface::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, m, w, l);
    if (self->message) {
        LRESULT result = 0;
        if (self->message(m, w, l, result))
            return result;
    }
    switch (m) {
    case WM_MOUSEACTIVATE:
        return self->activatable_ ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_PAINT:
        ValidateRect(h, nullptr);
        self->Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED:
        self->Invalidate();
        return 0;
    case WM_CLOSE:
        self->Hide();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Surface::Show(RECT rect, bool activate)
{
    Move(rect);
    if (!visible_) {
        visible_ = true;
        firstPaint_ = true;
        ShowWindow(hwnd_, activate && activatable_ ? SW_SHOW : SW_SHOWNOACTIVATE);
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | (activate && activatable_ ? 0 : SWP_NOACTIVATE));
    if (activate && activatable_ && !SetForegroundWindow(hwnd_))
        SwitchToThisWindow(hwnd_, TRUE);
    Invalidate();
}

void Surface::Move(RECT rect)
{
    rect_ = rect;
    SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
    if (visible_)
        Invalidate();
}

void Surface::Hide()
{
    if (!visible_)
        return;
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    ResetDevice(); // the swap chain is recreated on the next Show
}

void Surface::Invalidate()
{
    if (visible_)
        Paint();
}

void Surface::ResetDevice()
{
    d2d_ = nullptr;
    if (target_)
        target_->SetRoot(nullptr);
    visual_ = nullptr;
    target_ = nullptr;
    swap_ = nullptr;
    brush_ = nullptr;
    bitmaps_.clear();
    bufW_ = bufH_ = 0;
    if (shared_)
        shared_->dcomp->Commit();
    shared_ = nullptr;
}

bool Surface::EnsureDevice()
{
    if (d2d_)
        return true;
    shared_ = SharedDevice();
    if (!shared_)
        return false;
    int w = std::max(1, int(rect_.right - rect_.left)), h = std::max(1, int(rect_.bottom - rect_.top));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    auto& s = *shared_;
    if (FAILED(s.dxgi->CreateSwapChainForComposition(s.d3d.get(), &desc, nullptr, swap_.put())) ||
        FAILED(s.dcomp->CreateTargetForHwnd(hwnd_, TRUE, target_.put())) || FAILED(s.dcomp->CreateVisual(visual_.put())) ||
        FAILED(visual_->SetContent(swap_.get())) || FAILED(target_->SetRoot(visual_.get())) || FAILED(s.dcomp->Commit()) ||
        FAILED(s.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put()))) {
        Log(L"surface: swap chain setup failed");
        s.lost = true; // a fresh shared device next time
        ResetDevice();
        return false;
    }
    bufW_ = w;
    bufH_ = h;
    return true;
}

void Surface::Paint()
{
    if (!visible_ || !EnsureDevice())
        return;
    int w = std::max(1, int(rect_.right - rect_.left)), h = std::max(1, int(rect_.bottom - rect_.top));
    if (w != bufW_ || h != bufH_) {
        d2d_->SetTarget(nullptr);
        if (FAILED(swap_->ResizeBuffers(2, UINT(w), UINT(h), DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            ResetDevice();
            return;
        }
        bufW_ = w;
        bufH_ = h;
    }
    winrt::com_ptr<IDXGISurface> back;
    winrt::com_ptr<ID2D1Bitmap1> target;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(back.put()))) || FAILED(d2d_->CreateBitmapFromDxgiSurface(back.get(), &bp, target.put()))) {
        ResetDevice();
        return;
    }
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    d2d_->SetTarget(target.get());
    d2d_->BeginDraw();
    d2d_->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2d_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (paint) {
        Canvas c(*this, d2d_.get(), Scale());
        paint(c);
    }
    HRESULT hr = d2d_->EndDraw();
    d2d_->SetTarget(nullptr);
    if (SUCCEEDED(hr))
        hr = swap_->Present(0, 0);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    if (firstPaint_ || ms > 16) {
        wchar_t title[64] = {};
        GetWindowTextW(hwnd_, title, 64);
        Log(L"surface: %s painted %dx%d in %.1f ms%s", title, w, h, ms, firstPaint_ ? L" (first after show)" : L"");
        firstPaint_ = false;
    }
    if (FAILED(hr)) {
        Log(L"surface: paint failed 0x%08X, recreating", unsigned(hr));
        if (shared_)
            shared_->lost = true;
        ResetDevice();
    }
}

} // namespace atrium
