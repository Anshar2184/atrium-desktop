// Atrium's lock screen, after macOS's: the wallpaper, the date above a large
// clock whose digits are the bar's liquid glass (a distance-field shape, so
// they refract and catch the light like the dock), the latest notifications
// (app and title only) and the account at the bottom.
//
// It is a cover over the unlocked desktop, not a replacement of Windows' own
// lock screen (D34): while it shows, a low-level keyboard hook keeps keys from
// reaching the desktop, and the first key or click hands over to the real
// Windows lock (LockWorkStation), where Windows Hello or the PIN unlock. The
// wallpaper is a separate window under the glass, so the glass refracts it.
// ATRIUM_LOCK_DRYRUN=1 only logs the hand-over (tests);
// ATRIUM_LOCK_WALLPAPER=<image> replaces the wallpaper (screenshots).
#pragma once

#include "ui/notifications.h"
#include "ui/surface.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class LockScreen {
public:
    explicit LockScreen(HINSTANCE instance);
    ~LockScreen();

    void Open(std::vector<Notice> notices);
    // Ends the cover: hands over to Windows' lock unless `lock` is false.
    void Dismiss(bool lock = true);
    bool Visible() const { return open_; }
    void LogState() const;

    std::function<void(bool shown)> onShown; // the bar steps aside while it shows
    std::function<void()> onBackdrop;        // the wallpaper is on screen (the glass appears just after)

private:
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    void Paint(Canvas& c);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    void BuildClock(); // the digits' distance field for the current time
    void LoadWallpaper();
    static LRESULT CALLBACK BackdropProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK KeyHook(int code, WPARAM w, LPARAM l);

    HINSTANCE instance_;
    Surface surface_;
    HWND backdrop_ = nullptr;
    HBITMAP wallpaper_ = nullptr;
    SIZE wallpaperSize_{};
    RECT monitor_{};
    bool open_ = false;
    bool private_ = false;
    std::vector<Notice> notices_;
    std::wstring user_;
    std::wstring clockText_;
    std::shared_ptr<const SdfImage> clock_;
    D2D1_RECT_F clockRect_{}; // DIP, the digits' glass
    double openedMs_ = 0;
    double closingMs_ = 0;
    float hint_ = 0; // the unlock hint brightens while the pointer moves
    double movedMs_ = 0;
    bool pendingShow_ = false;
    static HHOOK hook_;
    static HWND target_;
};

} // namespace atrium
