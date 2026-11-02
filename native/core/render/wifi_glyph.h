// The iPhone's Wi-Fi symbol, drawn as geometry (no font has its shape): a
// wedge and two bands, lit from the wedge up by the signal. Used by the bar's
// status ring and the control centre.
#pragma once

#include <d2d1_1.h>

namespace atrium {

// Steps lit for a signal 0..100: 1..3.
int WifiSteps(int signal);

// The symbol centred on (cx, cy), `radius` px from its point to the outer
// band's edge (its height; it is about 4:3 wide). `lit` steps (0..3) in `on`,
// the rest in `off`. Sets `brush`'s colour.
void DrawWifiFan(ID2D1DeviceContext* d2d, ID2D1SolidColorBrush* brush, float cx, float cy, float radius, int lit, D2D1_COLOR_F on,
                 D2D1_COLOR_F off);

} // namespace atrium
