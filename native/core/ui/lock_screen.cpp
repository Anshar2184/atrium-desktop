#include "ui/lock_screen.h"

#include "clock.h"
#include "log.h"

#define SECURITY_WIN32
#include <security.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <limits>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "msimg32.lib")

namespace atrium {

namespace {

constexpr wchar_t kBackdropClass[] = L"AtriumLockBackdrop";
constexpr UINT WM_LOCK_KEY = WM_APP + 0x60;
constexpr UINT_PTR kTickTimer = 1, kShowTimer = 2;
constexpr UINT kShowDelayMs = 140; // the wallpaper first, then the glass over it (the capture has it by then)
constexpr float kSpread = 28; // px of distance kept around the digits (shadow and rim)

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
float Ease(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

// Squared Euclidean distance transform along one line (Felzenszwalb and
// Huttenlocher): f holds 0 at features and a large value elsewhere.
void Edt1D(const float* f, int n, float* d, int* v, float* z)
{
    const float inf = std::numeric_limits<float>::max();
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; ++q) {
        float s = ((f[q] + float(q) * float(q)) - (f[v[k]] + float(v[k]) * float(v[k]))) / (2.f * float(q - v[k]));
        while (s <= z[k]) {
            --k;
            s = ((f[q] + float(q) * float(q)) - (f[v[k]] + float(v[k]) * float(v[k]))) / (2.f * float(q - v[k]));
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < float(q))
            ++k;
        d[q] = (float(q) - float(v[k])) * (float(q) - float(v[k])) + f[v[k]];
    }
}

// Distance (px) from each pixel to the nearest pixel where `feature` is true.
std::vector<float> Distance(const std::vector<uint8_t>& feature, int w, int h)
{
    const float big = 1e12f;
    std::vector<float> g(size_t(w) * size_t(h));
    for (size_t i = 0; i < g.size(); ++i)
        g[i] = feature[i] ? 0.f : big;
    int n = std::max(w, h);
    const size_t len = size_t(n);
    std::vector<float> f(len), d(len), z(len + 1);
    std::vector<int> v(len);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y)
            f[size_t(y)] = g[size_t(y) * size_t(w) + size_t(x)];
        Edt1D(f.data(), h, d.data(), v.data(), z.data());
        for (int y = 0; y < h; ++y)
            g[size_t(y) * size_t(w) + size_t(x)] = d[size_t(y)];
    }
    for (int y = 0; y < h; ++y) {
        float* row = g.data() + size_t(y) * size_t(w);
        std::copy(row, row + w, f.begin());
        Edt1D(f.data(), w, d.data(), v.data(), z.data());
        for (int x = 0; x < w; ++x)
            row[x] = std::sqrt(d[size_t(x)]);
    }
    return g;
}

std::wstring ClockText()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[16];
    swprintf_s(buf, L"%d:%02d", t.wHour, t.wMinute); // as on macOS: no leading zero
    return buf;
}

std::wstring DateText()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    static const wchar_t* const kDays[] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
    return std::to_wstring(t.wMonth) + L"月" + std::to_wstring(t.wDay) + L"日 星期" + kDays[t.wDayOfWeek % 7];
}

} // namespace

HHOOK LockScreen::hook_ = nullptr;
HWND LockScreen::target_ = nullptr;

LockScreen::LockScreen(HINSTANCE instance) : instance_(instance), surface_(instance, L"Atrium Lock", true, 0)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = BackdropProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBackdropClass;
    RegisterClassExW(&wc);
    // The wallpaper under the glass: an ordinary window, so the screen capture
    // the glass samples shows it.
    backdrop_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBackdropClass, L"Atrium Lock Backdrop", WS_POPUP, 0, 0, 1, 1,
                                nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(backdrop_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

LockScreen::~LockScreen()
{
    if (hook_)
        UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    if (backdrop_)
        DestroyWindow(backdrop_);
    if (wallpaper_)
        DeleteObject(wallpaper_);
}

void LockScreen::LoadWallpaper()
{
    if (wallpaper_)
        DeleteObject(wallpaper_);
    wallpaper_ = nullptr;
    wchar_t path[MAX_PATH * 2] = {};
    if (!GetEnvironmentVariableW(L"ATRIUM_LOCK_WALLPAPER", path, DWORD(std::size(path))))
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, DWORD(std::size(path)), path, 0);
    if (!path[0])
        return;
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        Log(L"lock: the wallpaper could not be read");
        return;
    }
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = LONG(w);
    bi.bmiHeader.biHeight = -LONG(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits && SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, static_cast<BYTE*>(bits)))) {
        wallpaper_ = bmp;
        wallpaperSize_ = SIZE{LONG(w), LONG(h)};
    } else if (bmp) {
        DeleteObject(bmp);
    }
}

LRESULT CALLBACK LockScreen::BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<LockScreen*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_PAINT && self) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (self->wallpaper_) {
            // Fills the screen, cropped to its shape (as the desktop's "fill").
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, self->wallpaper_);
            float sw = float(self->wallpaperSize_.cx), sh = float(self->wallpaperSize_.cy);
            float k = std::max(float(rc.right) / sw, float(rc.bottom) / sh);
            int cw = int(float(rc.right) / k), ch = int(float(rc.bottom) / k);
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, 0, 0, rc.right, rc.bottom, mem, int((sw - float(cw)) / 2), int((sh - float(ch)) / 2), cw, ch, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        } else {
            TRIVERTEX v[2] = {{0, 0, 0x1800, 0x1E00, 0x4400, 0}, {rc.right, rc.bottom, 0x6E00, 0x3400, 0x8000, 0}};
            GRADIENT_RECT gr{0, 1};
            GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK LockScreen::KeyHook(int code, WPARAM w, LPARAM l)
{
    // While the cover shows, keys do not reach the desktop (the Windows key,
    // Alt+Tab, ...); the first one hands over to Windows' lock. Releases pass,
    // so no key stays held down.
    if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)) {
        if (target_)
            PostMessageW(target_, WM_LOCK_KEY, reinterpret_cast<const KBDLLHOOKSTRUCT*>(l)->vkCode, 0);
        return 1;
    }
    return CallNextHookEx(hook_, code, w, l);
}

void LockScreen::BuildClock()
{
    clockText_ = ClockText();
    const float s = PopupScale(monitor_);
    const float H = float(monitor_.bottom - monitor_.top) / s, W = float(monitor_.right - monitor_.left) / s;
    const float fontPx = std::round(H * 0.21f * s);

    winrt::com_ptr<IDWriteFactory> dw;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put()));
    winrt::com_ptr<IDWriteTextFormat> format;
    if (!dw || FAILED(dw->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, fontPx, L"", format.put())))
        return;
    winrt::com_ptr<IDWriteTextLayout> layout;
    dw->CreateTextLayout(clockText_.c_str(), UINT32(clockText_.size()), format.get(), 10000, 10000, layout.put());
    DWRITE_TEXT_METRICS tm{};
    DWRITE_OVERHANG_METRICS om{};
    layout->GetMetrics(&tm);
    layout->GetOverhangMetrics(&om);
    const int pad = int(kSpread);
    const int w = int(std::ceil(tm.widthIncludingTrailingWhitespace)) + 2 * pad, h = int(std::ceil(tm.height)) + 2 * pad;

    // The digits drawn in white, their coverage read back.
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmap> bmp;
    winrt::com_ptr<ID2D1Factory> d2f;
    winrt::com_ptr<ID2D1RenderTarget> rt;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateBitmap(UINT(w), UINT(h), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put())) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2f.put())) ||
        FAILED(d2f->CreateWicBitmapRenderTarget(bmp.get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                                                rt.put())))
        return;
    winrt::com_ptr<ID2D1SolidColorBrush> white;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.put());
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->DrawTextLayout(D2D1::Point2F(float(pad), float(pad)), layout.get(), white.get());
    if (FAILED(rt->EndDraw()))
        return;
    std::vector<BYTE> px(size_t(w) * size_t(h) * 4);
    if (FAILED(bmp->CopyPixels(nullptr, UINT(w * 4), UINT(px.size()), px.data())))
        return;
    std::vector<uint8_t> inside(size_t(w) * size_t(h)), outside(inside.size());
    std::vector<float> cover(inside.size());
    int inkTop = h, inkBottom = 0;
    for (size_t i = 0; i < inside.size(); ++i) {
        cover[i] = float(px[i * 4 + 3]) / 255.f;
        inside[i] = cover[i] >= 0.5f;
        outside[i] = !inside[i];
        if (cover[i] > 0.1f) {
            int y = int(i / size_t(w));
            inkTop = std::min(inkTop, y);
            inkBottom = std::max(inkBottom, y);
        }
    }
    std::vector<float> toInside = Distance(inside, w, h), toOutside = Distance(outside, w, h);
    auto img = std::make_shared<SdfImage>();
    img->w = w;
    img->h = h;
    img->d.resize(inside.size());
    for (size_t i = 0; i < inside.size(); ++i) {
        float d = inside[i] ? -(toOutside[i] - 0.5f) : toInside[i] - 0.5f;
        if (cover[i] > 0.02f && cover[i] < 0.98f)
            d = 0.5f - cover[i]; // the anti-aliased edge, to a fraction of a px
        img->d[i] = std::clamp(d, -kSpread, kSpread);
    }
    clock_ = img;
    // Centred, its digits' top at 19% of the screen's height.
    float left = (W - float(w) / s) / 2, top = H * 0.19f - float(inkTop) / s;
    clockRect_ = D2D1::RectF(left, top, left + float(w) / s, top + float(h) / s);
    (void)inkBottom;
    (void)om;
}

void LockScreen::Open(std::vector<Notice> notices)
{
    if (open_)
        return;
    notices_ = std::move(notices);
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    monitor_ = mi.rcMonitor;
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
    user_ = L"用户";
    if (!private_) {
        wchar_t buf[256];
        ULONG n = 256;
        DWORD m = 256;
        if (GetUserNameExW(NameDisplay, buf, &n) && buf[0])
            user_ = buf;
        else if (GetUserNameW(buf, &m))
            user_ = buf;
    }
    LoadWallpaper();
    if (onShown)
        onShown(true);
    SetWindowPos(backdrop_, HWND_TOPMOST, monitor_.left, monitor_.top, monitor_.right - monitor_.left, monitor_.bottom - monitor_.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(backdrop_, nullptr, FALSE);
    UpdateWindow(backdrop_);
    open_ = true;
    closingMs_ = 0;
    BuildClock();
    target_ = surface_.Hwnd();
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, instance_, 0);
    pendingShow_ = true;
    SetTimer(surface_.Hwnd(), kShowTimer, kShowDelayMs, nullptr);
    SetTimer(surface_.Hwnd(), kTickTimer, 1000, nullptr);
    Log(L"lock: cover shown (%zu notices, keyboard hook %s)", notices_.size(), hook_ ? L"on" : L"failed");
}

void LockScreen::Dismiss(bool lock)
{
    if (!open_)
        return;
    open_ = false;
    if (hook_)
        UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    target_ = nullptr;
    KillTimer(surface_.Hwnd(), kTickTimer);
    KillTimer(surface_.Hwnd(), kShowTimer);
    pendingShow_ = false;
    wchar_t dry[8] = {};
    bool dryRun = GetEnvironmentVariableW(L"ATRIUM_LOCK_DRYRUN", dry, 8) && dry[0] == L'1';
    if (lock && !dryRun)
        LockWorkStation(); // Windows' own lock and sign-in (Hello, PIN) take over
    Log(L"lock: cover ended%s", !lock ? L"" : dryRun ? L", the hand-over to Windows' lock logged only (dry run)" : L", Windows' lock engaged");
    surface_.Hide();
    ShowWindow(backdrop_, SW_HIDE);
    if (onShown)
        onShown(false);
}

void LockScreen::LogState() const
{
    const RECT r = surface_.Rect();
    Log(L"lock: state visible=%d hook=%d clock='%s' sdf=%dx%d clockRect=%.0f,%.0f,%.0f,%.0f notices=%zu shapes=%zu rect=%ld,%ld,%ld,%ld", open_ ? 1 : 0,
        hook_ ? 1 : 0, clockText_.c_str(), clock_ ? clock_->w : 0, clock_ ? clock_->h : 0, clockRect_.left, clockRect_.top, clockRect_.right,
        clockRect_.bottom, notices_.size(), surface_.Shapes(), r.left, r.top, r.right, r.bottom);
}

void LockScreen::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const float W = float(monitor_.right - monitor_.left) / c.scale, H = float(monitor_.bottom - monitor_.top) / c.scale;
    std::vector<ShapeDip> shapes;
    const float e = Ease(float((c.now - openedMs_) / 520));
    if (e < 1)
        c.Continue();

    // The clock: the digits are glass (they settle in as the cover appears).
    if (clock_)
        shapes.push_back({clockRect_, 0, 1 - e, clock_});

    c.BeginGroup(e, 1, W / 2, H / 2, 0, -14 * (1 - e));
    // The date above it.
    const float s = c.scale;
    float dateBottom = clockRect_.top + kSpread / s + 6;
    c.Text(DateText(), D2D1::RectF(0, dateBottom - 40, W, dateBottom), 24, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);

    // The latest notifications: app and title, each on its own glass.
    float y = clockRect_.bottom + 10;
    const float cw = 380, ch = 62;
    for (size_t i = 0; i < notices_.size() && i < 3; ++i) {
        const Notice& n = notices_[i];
        D2D1_RECT_F card = D2D1::RectF((W - cw) / 2, y, (W + cw) / 2, y + ch);
        shapes.push_back({card, 22, 1 - e});
        c.Text(n.app.empty() ? L"Atrium" : n.app, D2D1::RectF(card.left + 18, card.top + 10, card.right - 18, card.top + 28), 12, t.subtext,
               DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(private_ ? L"新通知" : n.title, D2D1::RectF(card.left + 18, card.top + 29, card.right - 18, card.top + 50), 14, t.text,
               DWRITE_FONT_WEIGHT_SEMI_BOLD);
        y += ch + 10;
    }

    // The account, and how to go on.
    float cy = H - 150;
    c.Circle(W / 2, cy, 30, Alpha(t.text, 0.22f));
    c.Text(user_.substr(0, 1), D2D1::RectF(W / 2 - 30, cy - 30, W / 2 + 30, cy + 30), 24, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(user_, D2D1::RectF(0, cy + 38, W, cy + 62), 16, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    float moved = float(std::clamp((c.now - movedMs_) / 1200.0, 0.0, 1.0));
    if (moved < 1)
        c.Continue();
    c.Text(L"按任意键或点击以解锁", D2D1::RectF(0, cy + 66, W, cy + 86), 12.5f, Alpha(t.subtext, 0.55f + 0.45f * (1 - moved)),
           DWRITE_FONT_WEIGHT_NORMAL, 1);
    c.EndGroup();
    surface_.SetShapes(shapes);
}

bool LockScreen::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    (void)l;
    (void)result;
    switch (m) {
    case WM_LOCK_KEY:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
        if (open_)
            Dismiss(true);
        return true;
    case WM_MOUSEMOVE:
        movedMs_ = ClockMs();
        surface_.Invalidate();
        return true;
    case WM_TIMER:
        if (w == kShowTimer) {
            KillTimer(surface_.Hwnd(), kShowTimer);
            if (open_ && pendingShow_) {
                pendingShow_ = false;
                if (onBackdrop)
                    onBackdrop();
                openedMs_ = ClockMs();
                surface_.Show(monitor_, true, POINT{(monitor_.left + monitor_.right) / 2, (monitor_.top + monitor_.bottom) / 2}, false);
            }
            return true;
        }
        if (w == kTickTimer) {
            if (ClockText() != clockText_)
                BuildClock();
            surface_.Invalidate();
            return true;
        }
        return false;
    case WM_ACTIVATE:
        return true; // stays up when another window takes the focus
    }
    return false;
}

} // namespace atrium
