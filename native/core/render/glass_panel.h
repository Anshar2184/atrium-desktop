// The glass layer under a popup window (start menu, notifications). The bar's
// render thread draws it with the bar's material from the same monitor
// capture, into the window's lower composition layer; the popup draws its
// content into the upper one (D25).
#pragma once

#include <windows.h>

#include <cstdint>

namespace atrium {

struct GlassPanel {
    HWND hwnd = nullptr;
    RECT window{};            // screen px
    float inset = 0;          // px from the window edge to the glass shape (shadow room)
    float radius = 0;         // px
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

// Tone of popup glass: the user's tint when the bar is clearly tinted,
// otherwise the system theme's light or dark. `minMix` is the least tint the
// panels use; the render thread raises it where the backdrop would make text
// hard to read.
struct PanelTone {
    bool light = false;  // light glass, dark text
    uint32_t tint = 0;   // 0xRRGGBB
    float minMix = 0.4f;
};
PanelTone PanelToneFor(uint32_t userTint, double transparency);

} // namespace atrium
