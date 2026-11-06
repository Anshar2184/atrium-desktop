// Atrium's own start menu (opened from the logo on the bar), a launcher of
// two separate pieces of the bar's glass: a panel with the search field and
// the apps pinned to the Windows start menu (pages of 7 x 3; typing searches
// all apps instead), and below it a capsule shaped like the bar with the
// account, network, volume, battery, the system tray and power. The power
// choices open in a third piece of glass. No all-apps list, recommendations,
// recent files or file search. Tiles lift under the pointer, presses ripple.
#pragma once

#include "motion.h"
#include "shell/app_catalog.h"
#include "shell/system_status.h"
#include "shell/task_model.h"
#include "ui/glass_menu.h"
#include "ui/surface.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class StartMenu {
public:
    StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog);

    // barRect: the bar's glass shape on screen; the menu opens above its left
    // end, out of `logo` (the logo's rectangle).
    // previous: the window to give the focus back to when closed without launching.
    void Open(RECT barRect, RECT logo, HWND previous);
    void Close(bool launched);
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // an icon or the catalog changed
    void SetStatus(const SystemStatus& status);
    static int TileIconPx(float scale); // the pins' icon size (AppCatalog::PrefetchStartIcons)
    HWND Hwnd() const { return surface_.Hwnd(); }
    // Corner radius (DIP), the bar's; the panel uses it, the capsule is round like the bar.
    void SetRadius(float dip) { radius_ = dip; surface_.SetRadius(dip); }
    // Height of the bar (DIP): the status capsule has the same.
    void SetBarHeight(float dip) { barH_ = dip; }

    std::function<void(const PinnedApp&)> onPin;            // pin to the bar (the dock)
    std::function<void(const std::wstring& what)> onSystem; // "network", "volume", "battery", "tray", "account"
    std::function<void()> onStatusRequest;
    GlassMenu* menu = nullptr;                               // a tile's menu (main's)

private:
    struct Hit {
        D2D1_RECT_F rect; // DIP, content coordinates
        std::wstring id;
    };

    void Paint(Canvas& c);
    void PaintSearch(Canvas& c, const Theme& t);
    void PaintPins(Canvas& c, const Theme& t);
    void PaintResults(Canvas& c, const Theme& t);
    void PaintShelf(Canvas& c, const Theme& t);
    void PaintPower(Canvas& c, const Theme& t);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::vector<CatalogApp> Results() const; // search matches, best first
    const std::vector<CatalogApp>& Shown() const; // what the arrow keys and Enter work on
    int Pages() const;
    void SetPage(int page);
    void Activate(const std::wstring& id);
    void Launch(const CatalogApp& app);
    void ContextMenu(const std::wstring& id, POINT screen);
    void RunPower(int index);
    std::wstring HitAt(POINT client) const;
    void MoveSelection(int dx, int dy);
    void EnsureVisible();
    void PlaceIme();
    void Restage(); // the content changed: tiles slide in again
    void LoadAccount();
    float Hover(Canvas& c, const std::wstring& id, bool on); // eased 0..1, per element
    float W() const;
    float H() const;
    D2D1_RECT_F MainRect() const;
    D2D1_RECT_F ShelfRect() const;
    D2D1_RECT_F PowerRect() const; // the power card when fully open
    float ButtonSize() const;      // round buttons in the capsule
    float Covered(const D2D1_RECT_F& r, double now) const; // 1, or less while the power card covers r

    Surface surface_;
    TaskModel& tasks_;
    AppCatalog& catalog_;
    std::vector<CatalogApp> apps_, pins_, results_;
    std::wstring query_;
    int page_ = 0;
    float pageShown_ = 0;               // displayed page, gliding to page_
    float wheelAcc_ = 0;
    float scroll_ = 0, scrollTarget_ = 0, contentH_ = 0;
    int sel_ = -1;
    std::wstring hover_, pressed_;
    std::vector<Hit> hits_;
    SystemStatus status_;
    HWND previous_ = nullptr;
    bool menuOpen_ = false;
    bool tracking_ = false;
    ULONGLONG lastClosed_ = 0;
    float radius_ = 30, barH_ = 60;
    // Animation.
    std::map<std::wstring, motion::Follow> hoverAnim_;
    double stageMs_ = 0;    // content (re)appeared: tiles slide in staggered
    double lastPaint_ = 0;
    std::wstring rippleId_;
    float rippleX_ = 0, rippleY_ = 0;
    double rippleMs_ = 0;
    bool powerOpen_ = false;
    int powerSel_ = -1;
    double powerMs_ = 0;
    // Account shown on the shelf.
    std::wstring userName_;
    IconImage avatar_;
    bool private_ = false; // ATRIUM_PRIVACY: no account or network names
};

} // namespace atrium
