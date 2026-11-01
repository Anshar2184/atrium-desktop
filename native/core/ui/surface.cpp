#include "ui/surface.h"

#include "clock.h"
#include "log.h"
#include "render/glass_renderer.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "dcomp.lib")

namespace atrium {

namespace {

constexpr wchar_t kClass[] = L"AtriumSurface";
constexpr UINT_PTR kHideTimer = 0x5A01, kFrameTimer = 0x5A02, kGlassWaitTimer = 0x5A03;
constexpr UINT kHideMs = 140;     // the closing pop (PopVisual) lasts 130 ms
constexpr double kGlassWaitMs = 300; // then a plain panel stands in for missing glass

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }

uint32_t g_tint = 0xFFFFFF;
double g_transparency = 0.95;

float MonitorScale(RECT r)
{
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY);
    UINT x = 96, y = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y);
    return float(x) / 96.f;
}

} // namespace

GlassHost* Surface::host_ = nullptr;
bool Surface::capturable_ = false;

void Theme::SetGlass(uint32_t tint, double transparency)
{
    g_tint = tint;
    g_transparency = transparency;
}

Theme Theme::For(bool darkInk)
{
    // The bar's inks and washes (BarPainter::DrawContent).
    Theme t;
    t.light = darkInk;
    PanelTone tone = PanelToneFor(g_tint, g_transparency);
    t.panel = Rgba(tone.tint, 0.96f);
    if (darkInk) {
        t.border = Rgba(0x000000, 0.10f);
        t.text = Rgba(0x111317, 0.92f);
        t.subtext = Rgba(0x111317, 0.66f);
        t.faint = Rgba(0x111317, 0.46f);
        t.hover = Rgba(0x000000, 0.10f);
        t.press = Rgba(0x000000, 0.16f);
        t.divider = Rgba(0x000000, 0.12f);
        t.card = Rgba(0x000000, 0.05f);
        t.cardHot = Rgba(0x000000, 0.10f);
        t.halo = Rgba(0xFFFFFF, 0.35f);
    } else {
        t.border = Rgba(0xFFFFFF, 0.30f);
        t.text = Rgba(0xFFFFFF, 0.96f);
        t.subtext = Rgba(0xFFFFFF, 0.76f);
        t.faint = Rgba(0xFFFFFF, 0.56f);
        t.hover = Rgba(0xFFFFFF, 0.20f);
        t.press = Rgba(0xFFFFFF, 0.28f);
        t.divider = Rgba(0xFFFFFF, 0.24f);
        t.card = Rgba(0xFFFFFF, 0.08f);
        t.cardHot = Rgba(0xFFFFFF, 0.20f);
        t.halo = Rgba(0x000000, 0.32f);
    }
    t.accent = Rgba(0x0A84FF, 1);
    t.accentText = Rgba(0xFFFFFF, 1);
    return t;
}

Theme Theme::Panel()
{
    Theme t = For(PanelToneFor(g_tint, g_transparency).light);
    t.halo = D2D1::ColorF(0, 0, 0, 0); // an opaque panel needs none
    return t;
}

// ---- Canvas ------------------------------------------------------------------

D2D1_RECT_F Canvas::Px(D2D1_RECT_F r) const { return D2D1::RectF(r.left * scale, r.top * scale, r.right * scale, r.bottom * scale); }

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F c)
{
    c.a *= alpha_; // the fade of the enclosing groups
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
    if (IDWriteTextFormat* f = Format(size * scale, w, false, align, wrap)) {
        D2D1_RECT_F p = Px(r);
        if (halo && theme.halo.a > 0) { // the bar's halo: a soft copy just below the ink
            float o = std::max(1.0f, scale * 0.6f);
            D2D1_COLOR_F hc = theme.halo;
            hc.a *= c.a;
            d2d_->DrawText(s.c_str(), UINT32(s.size()), f, D2D1::RectF(p.left, p.top + o, p.right, p.bottom + o), Brush(hc), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        d2d_->DrawText(s.c_str(), UINT32(s.size()), f, p, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Canvas::Glyph(const std::wstring& g, D2D1_RECT_F r, float size, D2D1_COLOR_F c)
{
    if (IDWriteTextFormat* f = Format(size * scale, DWRITE_FONT_WEIGHT_NORMAL, true, 1, false)) {
        D2D1_RECT_F p = Px(r);
        if (halo && theme.halo.a > 0) {
            float o = std::max(1.0f, scale * 0.6f);
            D2D1_COLOR_F hc = theme.halo;
            hc.a *= c.a;
            d2d_->DrawText(g.c_str(), UINT32(g.size()), f, D2D1::RectF(p.left, p.top + o, p.right, p.bottom + o), Brush(hc));
        }
        d2d_->DrawText(g.c_str(), UINT32(g.size()), f, p, Brush(c));
    }
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
    d2d_->DrawBitmap(b.get(), Px(r), opacity * alpha_, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

bool Canvas::Cached(const std::wstring& key) const
{
    auto it = s_.bitmaps_.find(key);
    return it != s_.bitmaps_.end() && it->second;
}

void Canvas::ImageCircle(const std::wstring& key, const IconImage& image, D2D1_RECT_F r)
{
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1EllipseGeometry> g;
    D2D1_RECT_F p = Px(r);
    if (FAILED(f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F((p.left + p.right) / 2, (p.top + p.bottom) / 2), (p.right - p.left) / 2,
                                                      (p.bottom - p.top) / 2),
                                        g.put())))
        return;
    d2d_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    Image(key, image, r);
    d2d_->PopLayer();
}

void Canvas::Ripple(D2D1_RECT_F r, float radius, float x, float y, float t, D2D1_COLOR_F c)
{
    t = std::clamp(t, 0.f, 1.f);
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1RoundedRectangleGeometry> g;
    if (FAILED(f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(Px(r), radius * scale, radius * scale), g.put())))
        return;
    float reach = std::hypot(r.right - r.left, r.bottom - r.top) * scale;
    float rr = std::max(1.f, reach * (1 - (1 - t) * (1 - t) * (1 - t)));
    float a = c.a * alpha_ * (1 - t) * (1 - t);
    D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(c.r, c.g, c.b, a * 0.55f)}, {0.75f, D2D1::ColorF(c.r, c.g, c.b, a)}, {1.f, D2D1::ColorF(c.r, c.g, c.b, 0)}};
    winrt::com_ptr<ID2D1GradientStopCollection> coll;
    winrt::com_ptr<ID2D1RadialGradientBrush> brush;
    D2D1_POINT_2F centre = D2D1::Point2F(x * scale, y * scale);
    if (FAILED(d2d_->CreateGradientStopCollection(stops, 3, coll.put())) ||
        FAILED(d2d_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(), rr, rr), coll.get(), brush.put())))
        return;
    d2d_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    d2d_->FillEllipse(D2D1::Ellipse(centre, rr, rr), brush.get());
    d2d_->PopLayer();
}

void Canvas::Clip(D2D1_RECT_F r) { d2d_->PushAxisAlignedClip(Px(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::Unclip() { d2d_->PopAxisAlignedClip(); }

void Canvas::Continue() { s_.frameWanted_ = true; }

// Opacity multiplies into every colour drawn inside the group instead of a
// D2D layer: layers allocate intermediate surfaces, which WARP keeps cached
// (tens of MB with a fading tile per row). Overlapping shapes in a group fade
// individually, which these groups never notice.
void Canvas::BeginGroup(float opacity, float k, float cx, float cy, float dx, float dy)
{
    D2D1_MATRIX_3X2_F current;
    d2d_->GetTransform(&current);
    groups_.push_back({current, alpha_});
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(cx * scale, cy * scale)) * D2D1::Matrix3x2F::Translation(dx * scale, dy * scale);
    d2d_->SetTransform(local * current);
    alpha_ *= std::clamp(opacity, 0.f, 1.f);
}

void Canvas::EndGroup()
{
    if (groups_.empty())
        return;
    d2d_->SetTransform(groups_.back().first);
    alpha_ = groups_.back().second;
    groups_.pop_back();
}

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

Surface::Surface(HINSTANCE instance, const wchar_t* title, bool activatable, float insetDip) : activatable_(activatable), insetDip_(insetDip)
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
    // The glass below the content samples the screen: the window must not be in it.
    if (!capturable_ && !SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE))
        Log(L"surface: WDA_EXCLUDEFROMCAPTURE failed (%lu); the glass would show this popup", GetLastError());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put()));
}

Surface::~Surface()
{
    if (hwnd_) {
        if (host_) {
            GlassPanel p;
            p.hwnd = hwnd_;
            host_->SetPanel(p); // not visible: removes the layer
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void Surface::SetGlassHost(GlassHost* host) { host_ = host; }
void Surface::SetCapturable(bool capturable) { capturable_ = capturable; }

int Surface::InsetPx() const { return int(std::lround(insetDip_ * scale_)); }

POINT Surface::ScreenToContent(POINT p) const { return POINT{p.x - rect_.left, p.y - rect_.top}; }
POINT Surface::ContentToScreen(POINT p) const { return POINT{p.x + rect_.left, p.y + rect_.top}; }

void Surface::SetRadius(float dip)
{
    if (radius_ == dip)
        return;
    radius_ = dip;
    if (visible_)
        PublishPanel();
}

void Surface::SetShapes(const std::vector<ShapeDip>& shapes)
{
    auto same = [](const ShapeDip& a, const ShapeDip& b) {
        return a.radius == b.radius && a.rect.left == b.rect.left && a.rect.top == b.rect.top && a.rect.right == b.rect.right && a.rect.bottom == b.rect.bottom;
    };
    if (shapes.size() == shapes_.size() && std::equal(shapes.begin(), shapes.end(), shapes_.begin(), same))
        return;
    shapes_ = shapes;
    if (visible_ || hiding_)
        PublishPanel();
}

void Surface::PublishPanel()
{
    if (!host_)
        return;
    GlassPanel p;
    p.hwnd = hwnd_;
    p.window = window_;
    p.inset = float(InsetPx());
    p.radius = radius_ * scale_;
    for (const ShapeDip& s : shapes_) {
        // Whole px, so the glass and the content's washes line up.
        auto px = [&](float v) { return std::round(v * scale_) + p.inset; };
        p.shapes.push_back({px(s.rect.left), px(s.rect.top), px(s.rect.right), px(s.rect.bottom), s.radius * scale_});
    }
    p.visible = visible_ || hiding_;
    p.pop = pop_;
    p.popOpen = popOpen_;
    p.popX = float(anchor_.x - window_.left);
    p.popY = float(anchor_.y - window_.top);
    p.rippleX = rippleX_;
    p.rippleY = rippleY_;
    p.rippleMs = rippleMs_;
    host_->SetPanel(p);
}

void Surface::Ripple(float x, float y)
{
    int inset = InsetPx();
    rippleX_ = x * scale_ + float(inset);
    rippleY_ = y * scale_ + float(inset);
    rippleMs_ = ClockMs();
    PublishPanel();
}

LRESULT CALLBACK Surface::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_TIMER:
        if (w == kHideTimer) {
            self->FinishHide();
            return 0;
        }
        if (w == kFrameTimer) {
            KillTimer(h, kFrameTimer);
            self->Paint();
            return 0;
        }
        if (w == kGlassWaitTimer) {
            KillTimer(h, kGlassWaitTimer);
            self->Invalidate(); // a plain panel if the glass still has not come
            return 0;
        }
        break;
    case WM_GLASS_READY:
        if (!self->glassReady_ && self->visible_) {
            self->glassReady_ = true;
            KillTimer(h, kGlassWaitTimer);
            self->Invalidate();
        }
        return 0;
    case WM_GLASS_INK:
        if (self->darkInk_ != (w != 0)) {
            self->darkInk_ = w != 0;
            self->Invalidate();
        }
        return 0;
    }
    // Mouse positions in content coordinates; presses on the margin are the caller's to decide.
    const bool mouse = (m >= WM_MOUSEFIRST && m <= WM_MBUTTONDBLCLK && m != WM_MOUSEWHEEL) || m == WM_MOUSEHWHEEL;
    if (mouse && m != WM_MOUSEHWHEEL) {
        int inset = self->InsetPx();
        int x = GET_X_LPARAM(l) - inset, y = GET_Y_LPARAM(l) - inset;
        bool outside = x < 0 || y < 0 || x >= self->rect_.right - self->rect_.left || y >= self->rect_.bottom - self->rect_.top;
        if (outside && (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN)) {
            if (self->onMargin)
                self->onMargin();
            return 0;
        }
        if (self->hiding_)
            return 0;
        l = MAKELPARAM(x, y);
    }
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

void Surface::Place(RECT rect)
{
    rect_ = rect;
    scale_ = MonitorScale(rect);
    int inset = InsetPx();
    window_ = RECT{rect.left - inset, rect.top - inset, rect.right + inset, rect.bottom + inset};
    SetWindowPos(hwnd_, HWND_TOPMOST, window_.left, window_.top, window_.right - window_.left, window_.bottom - window_.top, SWP_NOACTIVATE);
}

void Surface::Show(RECT rect, bool activate, POINT anchor)
{
    KillTimer(hwnd_, kHideTimer);
    Place(rect);
    anchor_ = anchor;
    bool wasHiding = hiding_;
    hiding_ = false;
    if (!visible_) {
        visible_ = true;
        firstPaint_ = !wasHiding;
        if (!wasHiding) {
            glassReady_ = false;
            shownMs_ = ClockMs();
            SetTimer(hwnd_, kGlassWaitTimer, UINT(kGlassWaitMs) + 20, nullptr);
        }
        ShowWindow(hwnd_, activate && activatable_ ? SW_SHOW : SW_SHOWNOACTIVATE);
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | (activate && activatable_ ? 0 : SWP_NOACTIVATE));
    if (activate && activatable_ && !SetForegroundWindow(hwnd_))
        SwitchToThisWindow(hwnd_, TRUE);
    // Content and glass pop out of the anchor together (DWM runs both
    // animations). The animation is in place before the first frame shows.
    ++pop_;
    popOpen_ = true;
    if (EnsureDevice())
        PopVisual(shared_->dcomp.get(), visual_.get(), true, float(anchor.x - window_.left), float(anchor.y - window_.top));
    Paint();
    PublishPanel();
}

void Surface::Move(RECT rect)
{
    if (EqualRect(&rect, &rect_))
        return;
    Place(rect);
    if (visible_) {
        Invalidate();
        PublishPanel();
    }
}

void Surface::Hide(bool animate)
{
    if (!visible_)
        return;
    visible_ = false;
    KillTimer(hwnd_, kFrameTimer);
    if (!animate || !visual_ || !shared_) {
        FinishHide();
        return;
    }
    hiding_ = true;
    ++pop_;
    popOpen_ = false;
    PopVisual(shared_->dcomp.get(), visual_.get(), false, float(anchor_.x - window_.left), float(anchor_.y - window_.top));
    PublishPanel();
    SetTimer(hwnd_, kHideTimer, kHideMs, nullptr);
}

void Surface::FinishHide()
{
    KillTimer(hwnd_, kHideTimer);
    KillTimer(hwnd_, kGlassWaitTimer);
    hiding_ = false;
    visible_ = false;
    glassReady_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    PublishPanel(); // not visible: the glass layer goes too
    ResetDevice();  // the swap chain is recreated on the next Show
}

void Surface::Invalidate()
{
    if (visible_)
        Paint();
}

void Surface::RequestFrame()
{
    if (visible_)
        SetTimer(hwnd_, kFrameTimer, 10, nullptr);
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
    int w = std::max(1, int(window_.right - window_.left)), h = std::max(1, int(window_.bottom - window_.top));
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
    int w = std::max(1, int(window_.right - window_.left)), h = std::max(1, int(window_.bottom - window_.top));
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
    frameWanted_ = false;
    if (paint) {
        float inset = float(InsetPx());
        D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation(inset, inset);
        d2d_->SetTransform(base);
        Canvas c(*this, d2d_.get(), scale_, base);
        c.now = ClockMs();
        c.panel = !host_ || (!glassReady_ && c.now - shownMs_ > kGlassWaitMs);
        c.theme = c.panel ? Theme::Panel() : Theme::For(darkInk_);
        paint(c);
        d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
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
        return;
    }
    if (frameWanted_)
        RequestFrame();
}

} // namespace atrium
