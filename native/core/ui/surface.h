// A popup window drawn with Direct2D into a DirectComposition swap chain with
// premultiplied alpha (the start menu and the notification panels). Lives on
// the UI thread; paints synchronously when invalidated.
#pragma once

#include "shell/task_model.h"

#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>

namespace atrium {

// Colours of the placeholder material: a plain translucent panel following
// the system light/dark setting (no glass shader for popups yet).
struct Theme {
    bool light = false;
    D2D1_COLOR_F panel, border, text, subtext, faint, hover, press, accent, accentText, divider;
    static Theme Current();
};

class Surface;

// Drawing helpers bound to one surface's device context.
class Canvas {
public:
    Canvas(Surface& surface, ID2D1DeviceContext* d2d, float scale) : s_(surface), d2d_(d2d), scale(scale) {}
    float scale; // px per DIP; every coordinate below is in DIP

    D2D1_RECT_F Px(D2D1_RECT_F dip) const;
    void Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
    void Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1);
    void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width = 1);
    void Circle(float cx, float cy, float radius, D2D1_COLOR_F c);
    // Text in `r`; align: 0 left, 1 centre, 2 right. One line, trimmed with an ellipsis unless wrap.
    void Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, int align = 0,
              bool wrap = false);
    void Glyph(const std::wstring& g, D2D1_RECT_F r, float size, D2D1_COLOR_F c);
    float TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL);
    // An icon by a stable cache key; `image` is only read the first time.
    void Image(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, float opacity = 1);
    void Clip(D2D1_RECT_F r);
    void Unclip();
    ID2D1DeviceContext* D2D() const { return d2d_; }

private:
    IDWriteTextFormat* Format(float px, DWRITE_FONT_WEIGHT w, bool glyph, int align, bool wrap);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
    Surface& s_;
    ID2D1DeviceContext* d2d_;
};

class Surface {
public:
    // paint: draws the content (DIP coordinates, the window is width x height DIP).
    // message: window messages before default handling; return true if handled (result in `result`).
    Surface(HINSTANCE instance, const wchar_t* title, bool activatable);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    std::function<void(Canvas&)> paint;
    std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> message;

    // Shows the window at `rect` (screen px); activatable surfaces take the foreground.
    void Show(RECT rect, bool activate);
    void Move(RECT rect);
    void Hide();
    bool Visible() const { return visible_; }
    void Invalidate();
    HWND Hwnd() const { return hwnd_; }
    float Scale() const;
    RECT Rect() const { return rect_; }
    void ExcludeFromCapture(bool on);

private:
    friend class Canvas;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void Paint();
    bool EnsureDevice();
    void ResetDevice();

    HWND hwnd_ = nullptr;
    bool activatable_;
    bool visible_ = false;
    bool firstPaint_ = false;
    RECT rect_{};
    int bufW_ = 0, bufH_ = 0;
    // One D3D11/D2D/DirectComposition device for all popups (a device per
    // window costs tens of MB and dozens of driver threads); per window only
    // the swap chain, its composition target and a device context, released
    // while the window is hidden.
    struct Shared;
    static std::shared_ptr<Shared> SharedDevice();
    std::shared_ptr<Shared> shared_;
    winrt::com_ptr<IDXGISwapChain1> swap_;
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<ID2D1DeviceContext> d2d_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    std::map<std::tuple<int, int, bool, int, bool>, winrt::com_ptr<IDWriteTextFormat>> formats_;
    std::map<std::wstring, winrt::com_ptr<ID2D1Bitmap>> bitmaps_;
};

} // namespace atrium
