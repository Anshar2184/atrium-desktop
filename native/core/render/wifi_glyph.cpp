#include "wifi_glyph.h"

#include "round_stroke.h"

#include <winrt/base.h>

#include <array>
#include <cmath>

namespace atrium {

int WifiSteps(int signal) { return signal >= 60 ? 3 : signal >= 30 ? 2 : 1; }

void DrawWifiFan(ID2D1DeviceContext* d2d, ID2D1SolidColorBrush* brush, float cx, float cy, float radius, int lit, D2D1_COLOR_F on,
                 D2D1_COLOR_F off)
{
    constexpr float kDeg = 3.14159265f / 180.f;
    constexpr float kSpan = 78, kFrom = 270 - kSpan / 2; // about 4:3, like the iPhone's
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    // Three unit-size paths per drawing thread. Replacing the factory drops the old paths.
    struct Glyph {
        winrt::com_ptr<ID2D1Factory> factory;
        std::array<winrt::com_ptr<ID2D1PathGeometry>, 3> paths;
    };
    thread_local Glyph glyph;
    if (glyph.factory != factory) {
        glyph = Glyph{};
        auto point = [](float r, float a) { return D2D1::Point2F(r * std::cos(a * kDeg), 0.5f + r * std::sin(a * kDeg)); };
        const float radii[] = {0.37f, 0.595f, 0.905f};
        for (int k = 0; k < 3; ++k) {
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(glyph.paths[k].put())) || FAILED(glyph.paths[k]->Open(sink.put())))
                return;
            sink->BeginFigure(k == 0 ? D2D1::Point2F(0, 0.5f) : point(radii[k], kFrom),
                              k == 0 ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            if (k == 0)
                sink->AddLine(point(radii[k], kFrom));
            sink->AddArc(D2D1::ArcSegment(point(radii[k], kFrom + kSpan), D2D1::SizeF(radii[k], radii[k]), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(k == 0 ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            sink->Close();
        }
        glyph.factory = factory;
    }
    D2D1_MATRIX_3X2_F previous;
    d2d->GetTransform(&previous);
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(radius, radius) * D2D1::Matrix3x2F::Translation(cx, cy) * previous);
    ID2D1StrokeStyle* round = RoundStroke(d2d);
    for (int k = 0; k < 3; ++k) {
        brush->SetColor(lit >= k + 1 ? on : off);
        if (k == 0)
            d2d->FillGeometry(glyph.paths[k].get(), brush);
        d2d->DrawGeometry(glyph.paths[k].get(), brush, 0.19f * (k == 0 ? 0.3f : 1.f), round);
    }
    d2d->SetTransform(previous);
}

} // namespace atrium
