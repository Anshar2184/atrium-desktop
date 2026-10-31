// Draws the bar's content over the glass with Direct2D: app icons with soft
// shadows, hover and press animation, running indicators, a separator and the
// clock. All geometry arrives precomputed in BarScene (window pixels).
#pragma once

#include "../shell/task_model.h"

#include <d2d1_3.h>
#include <dwrite_3.h>
#include <winrt/base.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

struct SceneItem {
    std::wstring key;
    D2D1_RECT_F icon{};        // icon square, window px
    std::shared_ptr<const IconImage> image; // may be null while loading
    uint64_t imageId = 0;      // identity of `image` for the bitmap cache
    bool running = false;
    bool active = false;
    bool attention = false;
};

struct BarScene {
    D2D1_RECT_F bar{};         // the glass shape inside the window
    float scale = 1;
    std::vector<SceneItem> items;
    float separatorX = -1;     // < 0: none
    D2D1_RECT_F clock{};       // empty: no clock
    float timePx = 0, datePx = 0;
    std::wstring hoverKey;     // item (or L"#clock") under the pointer
    std::wstring pressedKey;
};

// Ink colour per region: near-black on light glass, white on dark glass.
struct Ink {
    bool darkClock = false;
    bool darkApps = false;
};

class BarPainter {
public:
    BarPainter(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite);

    // Advances animations to `nowMs`; true while any animation is still moving.
    bool Animate(const BarScene& scene, double nowMs);
    // Draws the content. It is rendered into a cached layer and only redrawn
    // when `contentKey` changes or an animation runs; a present caused by the
    // backdrop alone just composites the cached layer.
    void Draw(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date,
              const std::wstring& contentKey, bool animating);

private:
    void DrawContent(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date);
    ID2D1Bitmap1* BitmapFor(ID2D1DeviceContext2* d2d, const SceneItem& item);
    IDWriteTextFormat* Format(float px, DWRITE_FONT_WEIGHT weight);

    struct Motion {
        float hover = 0;   // 0..1
        float press = 0;   // 0..1
        float appear = 0;  // 0..1, fades new items in
    };
    winrt::com_ptr<IDWriteFactory3> dwrite_;
    winrt::com_ptr<ID2D1Effect> shadow_;
    std::map<std::wstring, Motion> motion_;
    std::map<uint64_t, winrt::com_ptr<ID2D1Bitmap1>> bitmaps_;
    std::map<std::pair<int, int>, winrt::com_ptr<IDWriteTextFormat>> formats_;
    double lastMs_ = 0;
    winrt::com_ptr<ID2D1Bitmap1> layer_;
    std::wstring layerKey_;
};

} // namespace atrium
