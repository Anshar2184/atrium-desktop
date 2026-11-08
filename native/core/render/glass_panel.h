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

// One piece of glass inside a popup window, window px.
struct GlassShape {
    float left = 0, top = 0, right = 0, bottom = 0;
    float radius = 0;
    float fade = 0; // 0 = as is, 1 = gone
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
    float tailX = 0, tailHalfWidth = 0, tailHeight = 0, tailInset = 0; // Dock label pointer, window px; height 0 disables it
    bool operator==(const GlassShape&) const = default;
};

struct GlassPanel {
    HWND hwnd = nullptr;
    RECT window{};            // screen px
    int bufferW = 0, bufferH = 0; // resize gesture: backing capacity, independently of the visible window
    float inset = 0;          // px from the window edge to the glass shape (shadow room)
    float radius = 0;         // px
    // Separate pieces of glass, drawn back to front; empty: one shape, the
    // window minus `inset`, with `radius`.
    std::vector<GlassShape> shapes;
    bool visible = false;     // false removes the layer
    bool desktop = false;     // a widget: its backdrop is the desktop (DesktopBackdrop), not the screen
    bool animating = false;   // content publishes this panel's geometry on each animation frame
    unsigned pop = 0;         // bumped to play a pop animation
    bool popOpen = true;
    float popX = 0, popY = 0; // fixed point of the pop, window px
    float rippleX = 0, rippleY = 0; // press ripple centre, window px
    double rippleMs = 0;      // ClockMs() of the press; 0 = none
    unsigned gen = 0;         // bumped with every change: aligns content with the presented glass
    POINT offset{};          // texture origin inside a stable desktop host, committed with Present
};

// A popup's content and its glass come from two threads. So that both show
// in the same composition, a ready message releases the matching content
// buffer without blocking the UI thread.
void GlassPresented(HWND hwnd, unsigned gen);              // render thread
void ForgetGlass(HWND hwnd);                              // render thread; layer removed
bool IsGlassPresented(HWND hwnd, unsigned gen);            // UI thread; never waits for a frame

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
constexpr UINT WM_GLASS_PRESENTED = WM_APP + 0x43; // wParam: the presented generation

// The ink a popup's content needs, region by region: a coarse grid over the
// window (window px), 1 = dark ink (the glass shows something bright there).
// The bar's render thread publishes it, then posts WM_GLASS_INK; the popup
// reads it while painting, so each text follows what is under it.
struct InkGrid {
    int w = 0, h = 0;
    float cellX = 1, cellY = 1;
    std::vector<uint8_t> dark;
    bool operator==(const InkGrid&) const = default;
};
void PublishInk(HWND hwnd, const InkGrid& grid); // render thread
InkGrid InkFor(HWND hwnd);                        // UI thread

// Ink changes fade instead of switching: the amount of dark ink (0 light,
// 1 dark) eases out (cubic) over kInkFadeMs. A change while fading
// starts from where the fade is; the first target is taken at once.
constexpr double kInkFadeMs = 450;
struct InkFade {
    float from = 0, to = 0;
    double start = 0;
    bool set = false;
    void Target(float target, double nowMs);
    float Value(double nowMs) const;
    bool Moving(double nowMs) const { return set && from != to && nowMs - start < kInkFadeMs; }
};

// An InkGrid as shown: each cell's amount of dark ink (0..1) while it fades.
struct InkField {
    int w = 0, h = 0;
    float cellX = 1, cellY = 1;
    std::vector<float> dark;
    // The amount for a rectangle (window px): the median of the cells under it
    // (at rest, what most of them need, as InkGrid::DarkAt); `fallback` without cells.
    float DarkAt(float x0, float y0, float x1, float y1, float fallback) const;
};

// Tone of the plain panel that stands in while the glass is unavailable: the
// user's tint when the bar is clearly tinted, otherwise the system theme.
struct PanelTone {
    bool light = false;  // light panel, dark text
    uint32_t tint = 0;   // 0xRRGGBB
};
PanelTone PanelToneFor(uint32_t userTint, double transparency);

} // namespace atrium
