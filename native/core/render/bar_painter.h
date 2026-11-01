// Draws the bar's content over the glass with Direct2D: app icons with soft
// shadows, the dock-style magnification wave under the pointer, launch
// bounces, running dots, a separator and the clock. The base geometry arrives
// precomputed in BarScene (window pixels); magnification only displaces and
// scales it, so hit testing on the base layout stays exact (D25).
#pragma once

#include "../shell/task_model.h"

#include <d2d1_3.h>
#include <dwrite_3.h>
#include <winrt/base.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace atrium {

struct SceneItem {
    std::wstring key;
    D2D1_RECT_F icon{};        // icon square, window px
    D2D1_RECT_F plate{};       // hover/active plate around the icon, same centre
    std::shared_ptr<const IconImage> image; // may be null while loading
    uint64_t imageId = 0;      // identity of `image` for the bitmap cache
    bool running = false;
    bool active = false;
    bool attention = false;
    std::wstring glyph;        // Segoe Fluent Icons glyph instead of an image (bar entries)
    int badge = 0;             // > 0: count badge on the icon (unread notifications)
    double bounceMs = 0;       // ClockMs() when a launch bounce started; 0 = none
    int bounces = 0;           // at most this many hops (fewer once the app runs)
};

struct BarScene {
    D2D1_RECT_F bar{};         // the glass shape inside the window
    float scale = 1;
    std::vector<SceneItem> items;  // apps only (drag and drop works on these)
    std::vector<SceneItem> extras; // Atrium logo (#start), overflow (#more), notifications (#notify)
    float separatorX = -1;     // < 0: none
    D2D1_RECT_F clock{};       // empty: no clock
    float timePx = 0, datePx = 0;
    float plateRadius = 0;     // concentric with the bar's corners
    std::wstring hoverKey;     // item (or L"#clock") under the pointer
    std::wstring pressedKey;
    std::wstring dragKey;      // item being dragged: follows the pointer, drawn on top
    // Magnification: icons near the pointer grow up to `magnify` times, over a
    // wave `magnifyRange` px wide on each side; the bar stretches to fit.
    float magnify = 1;         // 1 = off
    float magnifyRange = 1;
    float pointerX = -1;       // window px; < 0 = the pointer is not on the bar
    // Press ripple in the glass (window px), started at ClockMs() `rippleMs`.
    float rippleX = 0, rippleY = 0;
    double rippleMs = 0;
};

// What the glass needs from the content each frame: the stretched shape and
// the ripple.
struct BarFrame {
    D2D1_RECT_F shape{};
    float rippleX = 0, rippleY = 0, rippleR = 0, rippleA = 0;
    float magnified = 0; // strength of the magnification, 0..1
    float hop = 0;       // highest launch hop, 0..1
};

// Ink colour per region: near-black on light glass, white on dark glass.
struct Ink {
    bool darkClock = false;
    bool darkApps = false;
};

class BarPainter {
public:
    BarPainter(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite);

    // Advances animations to `nowMs` (ClockMs); true while any animation is still moving.
    bool Animate(const BarScene& scene, double nowMs);
    const BarFrame& Frame() const { return frame_; }
    // Draws the content. It is rendered into a cached layer and only redrawn
    // when `contentKey` changes or an animation runs; a present caused by the
    // backdrop alone just composites the cached layer.
    void Draw(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date,
              const std::wstring& contentKey, bool animating);

private:
    void DrawContent(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date);
    ID2D1Bitmap1* BitmapFor(ID2D1DeviceContext2* d2d, const SceneItem& item);
    IDWriteTextFormat* Format(float px, DWRITE_FONT_WEIGHT weight, const wchar_t* family = L"Segoe UI Variable Text");
    void DrawLogo(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, float opacity);
    float MapX(float u) const; // base x -> displayed x under the magnification

    struct Motion {
        float hover = 0;   // 0..1
        float press = 0;   // 0..1
        float appear = 0;  // 0..1, fades new items in
        float lift = 0;    // 0..1, while dragged
        float x = 0;       // displayed plate centre; slides to the layout position
        bool placed = false;
        double bounceMs = 0; // the bounce being played
        int bounceEnd = 0;   // hops to play (shortened when the app starts running)
        float hop = 0;       // current bounce height, px
    };
    void DrawItem(ID2D1DeviceContext2* d2d, const BarScene& scene, const SceneItem& it, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink,
                  D2D1_COLOR_F wash);
    winrt::com_ptr<IDWriteFactory3> dwrite_;
    winrt::com_ptr<ID2D1Effect> shadow_;
    std::map<std::wstring, Motion> motion_;
    std::map<uint64_t, winrt::com_ptr<ID2D1Bitmap1>> bitmaps_;
    std::set<uint64_t> usedBitmaps_; // during one DrawContent
    std::map<std::tuple<int, int, std::wstring>, winrt::com_ptr<IDWriteTextFormat>> formats_;
    double lastMs_ = 0;
    D2D1_RECT_F lastBar_{};
    // Magnification state: strength 0..1 (eases in and out), wave centre and
    // its parameters of the current frame.
    float amount_ = 0, centre_ = 0, mag_ = 1, range_ = 1;
    BarFrame frame_;
    winrt::com_ptr<ID2D1Bitmap1> layer_;
    std::wstring layerKey_;
};

} // namespace atrium
