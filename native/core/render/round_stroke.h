// The round-capped, round-joined stroke every arc and capsule uses: made once
// per Direct2D factory and thread instead of on every draw.
#pragma once

#include <d2d1_1.h>
#include <winrt/base.h>

#include <utility>
#include <vector>

namespace atrium {

inline ID2D1StrokeStyle* RoundStroke(ID2D1RenderTarget* target)
{
    // The style keeps its factory alive, so a factory's address is never reused here.
    thread_local std::vector<std::pair<winrt::com_ptr<ID2D1Factory>, winrt::com_ptr<ID2D1StrokeStyle>>> made;
    winrt::com_ptr<ID2D1Factory> factory;
    target->GetFactory(factory.put());
    for (const auto& [f, s] : made)
        if (f == factory)
            return s.get();
    winrt::com_ptr<ID2D1StrokeStyle> style;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, style.put());
    made.emplace_back(factory, style);
    return style.get();
}

} // namespace atrium
