#include "wifi_glyph.h"

#include "round_stroke.h"

#include <winrt/base.h>

#include <cmath>

namespace atrium {

int WifiSteps(int signal) { return signal >= 60 ? 3 : signal >= 30 ? 2 : 1; }

void DrawWifiFan(ID2D1DeviceContext* d2d, ID2D1SolidColorBrush* brush, float cx, float cy, float radius, int lit, D2D1_COLOR_F on,
                 D2D1_COLOR_F off)
{
    constexpr float kDeg = 3.14159265f / 180.f;
    constexpr float kSpan = 78, kFrom = 270 - kSpan / 2; // about 4:3, like the iPhone's
    const float R = radius, t = R * 0.19f, oy = cy + R * 0.5f; // the point; the shape is centred on its box
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    ID2D1StrokeStyle* round = RoundStroke(d2d);
    auto point = [&](float r, float a) { return D2D1::Point2F(cx + r * std::cos(a * kDeg), oy + r * std::sin(a * kDeg)); };
    // The wedge, its outline stroked with round joins to soften its corners.
    winrt::com_ptr<ID2D1PathGeometry> g;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(factory->CreatePathGeometry(g.put())) && SUCCEEDED(g->Open(sink.put()))) {
        const float r0 = R * 0.37f;
        sink->BeginFigure(D2D1::Point2F(cx, oy), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(point(r0, kFrom));
        sink->AddArc(D2D1::ArcSegment(point(r0, kFrom + kSpan), D2D1::SizeF(r0, r0), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        brush->SetColor(lit >= 1 ? on : off);
        d2d->FillGeometry(g.get(), brush);
        d2d->DrawGeometry(g.get(), brush, t * 0.3f, round);
    }
    // The bands.
    const float radii[2] = {R * 0.595f, R * 0.905f};
    for (int k = 0; k < 2; ++k) {
        winrt::com_ptr<ID2D1PathGeometry> band;
        winrt::com_ptr<ID2D1GeometrySink> s;
        if (FAILED(factory->CreatePathGeometry(band.put())) || FAILED(band->Open(s.put())))
            return;
        s->BeginFigure(point(radii[k], kFrom), D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(D2D1::ArcSegment(point(radii[k], kFrom + kSpan), D2D1::SizeF(radii[k], radii[k]), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                   D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        brush->SetColor(lit >= k + 2 ? on : off);
        d2d->DrawGeometry(band.get(), brush, t, round);
    }
}

} // namespace atrium
