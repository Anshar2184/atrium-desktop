// Atrium's own start menu (opened from the logo on the bar): pinned apps, all
// apps (grouped by initial, pinyin for Chinese names), app search, power
// actions, and a shelf with the account, network, volume, battery and the
// system tray. No recommendations, recent files or file search. Glass of the
// bar's material below (Surface); tiles lift under the pointer, presses
// ripple, views slide and fade in.
#pragma once

#include "shell/app_catalog.h"
#include "shell/system_status.h"
#include "shell/task_model.h"
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
    void Refresh(); // the pins, an icon or the catalog changed
    HWND Hwnd() const { return surface_.Hwnd(); }
    // Corner radius (DIP), the bar's; inner shapes are concentric with it.
    void SetRadius(float dip) { radius_ = dip; surface_.SetRadius(dip); }
    // Test hooks (Atrium.Core.exe --command): type through the real WM_CHAR
    // path, and log what the menu shows (positions in screen px).
    void Type(const std::wstring& text);
    void LogState() const;

    std::function<void(const PinnedApp&)> onPin;
    std::function<void(const std::wstring& key)> onUnpin;
    std::function<void(const std::wstring& what)> onSystem; // "network", "volume", "battery", "tray", "account"

private:
    enum class View { Pinned, All };
    struct Hit {
        D2D1_RECT_F rect; // DIP, content coordinates
        std::wstring id;
    };
    struct Entry { // one launchable row or tile
        std::wstring key;   // pinned: task key; catalog: app id
        std::wstring name;
        bool pinned = false;
        size_t catalog = 0; // index into apps_ for catalog entries
        std::wstring group; // initial for the all-apps list
    };
    struct Row { // laid-out content: tiles, rows and group headers
        D2D1_RECT_F rect; // DIP, unscrolled (y from the content top)
        int entry = -1;   // -1: a group header
        std::wstring label;
    };

    void Paint(Canvas& c);
    void PaintShelf(Canvas& c, const Theme& t, float w, float h);
    void PaintPower(Canvas& c, const Theme& t, float w, float h);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::vector<Entry> Entries() const; // what the content area shows now
    std::vector<Row> Layout(const std::vector<Entry>& entries, float width) const;
    bool Grid() const { return query_.empty() && view_ == View::Pinned; }
    void Activate(const std::wstring& id);
    void Launch(const Entry& e);
    void ContextMenu(const std::wstring& id, POINT screen);
    void RunPower(int index);
    std::wstring HitAt(POINT client) const;
    void MoveSelection(int dx, int dy);
    void EnsureVisible();
    void PlaceIme();
    void Restage(); // the content changed: tiles slide in again
    void LoadAccount();
    float Hover(Canvas& c, const std::wstring& id, bool on); // eased 0..1, per element
    void Rebuild();  // sorted and grouped catalog
    float W() const;
    float H() const;

    Surface surface_;
    TaskModel& tasks_;
    AppCatalog& catalog_;
    std::vector<CatalogApp> apps_;
    std::vector<size_t> order_;         // apps_ sorted by group, then name
    std::vector<std::wstring> groups_;  // group of apps_[i]
    View view_ = View::Pinned;
    std::wstring query_;
    float scroll_ = 0, scrollTarget_ = 0, contentH_ = 0, viewH_ = 0;
    int sel_ = -1;
    std::wstring hover_, pressed_;
    std::vector<Hit> hits_;
    SystemStatus status_;
    HWND previous_ = nullptr;
    bool menuOpen_ = false;
    bool tracking_ = false;
    ULONGLONG lastClosed_ = 0;
    float radius_ = 24;
    // Animation.
    std::map<std::wstring, std::pair<float, double>> hoverAnim_; // value, last update
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
