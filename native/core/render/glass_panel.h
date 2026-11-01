// The glass layer under a popup window (start menu, notifications). The bar's
// render thread draws it with the bar's material from the same monitor
// capture, into the window's lower composition layer; the popup draws its
// content into the upper one (D25).
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace atrium {

// One piece of glass inside a popup window, window px.
struct GlassShape {
    float left = 0, top = 0, right = 0, bottom = 0;
    float radius = 0;
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

// Tone of the plain panel that stands in while the glass is unavailable: the
// user's tint when the bar is clearly tinted, otherwise the system theme.
struct PanelTone {
    bool light = false;  // light panel, dark text
    uint32_t tint = 0;   // 0xRRGGBB
};
PanelTone PanelToneFor(uint32_t userTint, double transparency);

} // namespace atrium
