// The glass layer under a popup window (start menu, notifications). The bar's
// render thread draws it with the bar's material from the same monitor
// capture, into the window's lower composition layer; the popup draws its
// content into the upper one (D25).
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace atrium {

// A shape given as a signed distance field (px, negative inside), one value
// per px of the shape's rectangle: the lock screen's clock digits.
struct SdfImage {
    int w = 0, h = 0;
    std::vector<float> d;
};

// One piece of glass inside a popup window, window px.
struct GlassShape {
    float left = 0, top = 0, right = 0, bottom = 0;
    float radius = 0;
    float fade = 0; // 0 = as is, 1 = gone
    std::shared_ptr<const SdfImage> sdf; // the shape inside the rectangle; null: a rounded rectangle
    // Instead of one rectangle: up to kMaxGlassParts rounded rectangles that
    // melt into one (a smooth union `melt` px wide), each with its own corner
    // radii. A page's pieces of glass becoming one while it changes.
    struct Part {
        float left, top, right, bottom;
        float radii[4]; // top left, top right, bottom right, bottom left
        bool operator==(const Part&) const = default;
    };
    std::vector<Part> parts;
    float melt = 0;
    float partMix = 0; // 0: the parts, 1: the rounded rectangle (left..bottom, radius) they become
    bool operator==(const GlassShape&) const = default;
};

struct GlassPanel {
    HWND hwnd = nullptr;
    RECT window{};            // screen px
    float inset = 0;          // px from the window edge to the glass shape (shadow room)
    float radius = 0;         // px
    // Separate pieces of glass, drawn back to front; empty: one shape, the
    // window minus `inset`, with `radius`.
    std::vector<GlassShape> shapes;
    bool visible = false;     // false removes the layer
    unsigned pop = 0;         // bumped to play a pop animation
    bool popOpen = true;
    float popX = 0, popY = 0; // fixed point of the pop, window px
    float rippleX = 0, rippleY = 0; // press ripple centre, window px
    double rippleMs = 0;      // ClockMs() of the press; 0 = none
};

class GlassHost {
public:
    virtual ~GlassHost() = default;
    virtual void SetPanel(const GlassPanel& panel) = 0; // UI thread
};

// Posted to the popup window once its glass has been presented.
constexpr UINT WM_GLASS_READY = WM_APP + 0x40;
// Posted to the popup window when the ink its glass needs changes: wParam 1 =
// dark ink (the glass shows something bright), 0 = light ink. The popups use
// the bar's material unchanged, so, like the bar, they adapt the ink, not the glass.
constexpr UINT WM_GLASS_INK = WM_APP + 0x41;

// The ink a popup's content needs, region by region: a coarse grid over the
// window (window px), 1 = dark ink (the glass shows something bright there).
// The bar's render thread publishes it, then posts WM_GLASS_INK; the popup
// reads it while painting, so each text follows what is under it.
struct InkGrid {
    int w = 0, h = 0;
    float cellX = 1, cellY = 1;
    std::vector<uint8_t> dark;
    bool operator==(const InkGrid&) const = default;
    // Dark ink for a rectangle (window px): what most of the cells under it need; `fallback` without a grid.
    bool DarkAt(float x0, float y0, float x1, float y1, bool fallback) const;
};
void PublishInk(HWND hwnd, const InkGrid& grid); // render thread
InkGrid InkFor(HWND hwnd);                        // UI thread

// Tone of the plain panel that stands in while the glass is unavailable: the
// user's tint when the bar is clearly tinted, otherwise the system theme.
struct PanelTone {
    bool light = false;  // light panel, dark text
    uint32_t tint = 0;   // 0xRRGGBB
};
PanelTone PanelToneFor(uint32_t userTint, double transparency);

} // namespace atrium
