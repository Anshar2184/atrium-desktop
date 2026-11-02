// The round-capped, round-joined stroke every arc and capsule uses: made once
// per Direct2D factory instead of on every draw.
#pragma once

#include <d2d1_1.h>
#include <winrt/base.h>

namespace atrium {

inline ID2D1StrokeStyle* RoundStroke(ID2D1RenderTarget* target)
{
    // One factory per thread at a time (the popups' shared device, the bar's
    // render session): a new one replaces the old, which can then go.
    thread_local winrt::com_ptr<ID2D1Factory> made;
    thread_local winrt::com_ptr<ID2D1StrokeStyle> style;
    winrt::com_ptr<ID2D1Factory> factory;
    target->GetFactory(factory.put());
    if (factory != made || !style) {
        style = nullptr;
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                   nullptr, 0, style.put());
        made = factory;
    }
    return style.get();
}

} // namespace atrium
