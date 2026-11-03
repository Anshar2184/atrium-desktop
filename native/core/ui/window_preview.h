// The windows of an app, previewed over the bar (as the Windows taskbar does): the pointer rests on
// an app that has windows, and a piece of the bar's glass above it shows each window live (DWM
// thumbnails), with its title and a close button. A click brings that window to the front; a
// middle click closes it. The preview stays while the pointer moves from the app onto it, and goes
// a moment after the pointer has left both.
#pragma once

#include "motion.h"
#include "ui/surface.h"

#include <dwmapi.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace atrium {

class WindowPreview {
public:
    explicit WindowPreview(HINSTANCE instance);
    ~WindowPreview();
    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;

    // Shows (or updates) the windows of app `key` centred above `anchor` (screen px: the icon's
    // centre, the top of what the bar draws); `icon` stands in when DWM has no thumbnail.
    void Show(const std::wstring& key, const std::vector<HWND>& windows, POINT anchor, const IconImage& icon);
    // The pointer left the app on the bar: the preview goes soon, unless the pointer comes onto it.
    void Leave();
    void Close();
    bool Visible() const { return surface_.Visible(); }
    const std::wstring& Key() const { return key_; }
    HWND Hwnd() const { return surface_.Hwnd(); }

    std::function<void(HWND)> onActivate; // a window was picked (the preview has closed)

private:
    struct Card {
        HWND window = nullptr;
        std::wstring title;
        HTHUMBNAIL thumb = nullptr;
        SIZE source{};        // the thumbnail's source size, px
        RECT sourceRect{};    // the window without its invisible borders, in the source's px
        D2D1_RECT_F rect{};   // the card, content DIP
        D2D1_RECT_F image{};  // where the window's picture goes, content DIP
        D2D1_RECT_F close{};  // its close button, content DIP
        motion::Follow hover;
    };

    void Paint(Canvas& c);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    void Layout(float scale, float maxWidthDip);
    void Place();          // the surface's rectangle for the cards, above the anchor
    void UpdateThumbs();   // DWM: where the pictures go, how visible
    void DropThumbs();
    int CardAt(POINT content) const;

    Surface surface_;
    std::wstring key_;
    std::vector<Card> cards_;
    IconImage icon_;
    POINT anchor_{};
    D2D1_SIZE_F size_{};   // DIP
    int hover_ = -1, pressed_ = -1;
    bool overClose_ = false;
    bool tracking_ = false, inside_ = false;
    float thumbAlpha_ = 0; // the pictures fade in once the glass has popped out
    double fadeStart_ = 0;
};

} // namespace atrium
