// Atrium's own start menu (opened from the logo on the bar): pinned apps, all
// apps, app search, power actions, and a footer with network, volume, battery
// and the system tray. No recommendations, recent files or file search.
#pragma once

#include "shell/app_catalog.h"
#include "shell/system_status.h"
#include "shell/task_model.h"
#include "ui/surface.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class StartMenu {
public:
    StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog);

    // barRect: the bar's glass shape on screen; the menu opens above its left end.
    // previous: the window to give the focus back to when closed without launching.
    void Open(RECT barRect, HWND previous);
    void Close(bool launched);
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // the pins, an icon or the catalog changed
    HWND Hwnd() const { return surface_.Hwnd(); }
    // Test hooks (Atrium.Core.exe --command): type through the real WM_CHAR
    // path, and log what the menu shows.
    void Type(const std::wstring& text);
    void LogState() const;

    std::function<void(const PinnedApp&)> onPin;
    std::function<void(const std::wstring& key)> onUnpin;
    std::function<void(const std::wstring& what)> onSystem; // "network", "volume", "battery", "tray"

private:
    enum class View { Pinned, All };
    struct Hit {
        D2D1_RECT_F rect; // DIP
        std::wstring id;
    };
    struct Entry { // one launchable row or tile
        std::wstring key;   // pinned: task key; catalog: app id
        std::wstring name;
        bool pinned = false;
        size_t catalog = 0; // index into apps_ for catalog entries
    };

    void Paint(Canvas& c);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::vector<Entry> Entries() const; // what the content area shows now
    void Activate(const std::wstring& id);
    void Launch(const Entry& e);
    void ContextMenu(const std::wstring& id, POINT screen);
    void PowerMenu();
    std::wstring HitAt(POINT client) const;
    void MoveSelection(int dx, int dy);
    void EnsureVisible();
    void PlaceIme();
    float W() const;
    float H() const;

    Surface surface_;
    TaskModel& tasks_;
    AppCatalog& catalog_;
    std::vector<CatalogApp> apps_;
    View view_ = View::Pinned;
    std::wstring query_;
    float scroll_ = 0, contentH_ = 0, viewH_ = 0;
    int sel_ = -1;
    std::wstring hover_, pressed_;
    std::vector<Hit> hits_;
    SystemStatus status_;
    HWND previous_ = nullptr;
    bool menuOpen_ = false;
    bool tracking_ = false;
    ULONGLONG lastClosed_ = 0;
};

} // namespace atrium
