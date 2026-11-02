// All installed apps, as the Windows start menu lists them (the shell's
// AppsFolder: Store apps and start menu shortcuts), and the apps pinned to the
// Windows start menu, in its order. Enumerated and iconified on a worker
// thread; the UI thread reads snapshots.
#pragma once

#include "shell/task_model.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct CatalogApp {
    std::wstring name;    // display name
    std::wstring id;      // parsing name inside AppsFolder (AUMID or known-folder path), or a shortcut's full path
    std::wstring aumid;   // System.AppUserModel.ID, may be empty
    std::wstring target;  // link target exe, may be empty
};

class AppCatalog {
public:
    // Posts `readyMsg` to `notify` when the list or an icon becomes available.
    AppCatalog(HWND notify, UINT readyMsg);
    ~AppCatalog();
    AppCatalog(const AppCatalog&) = delete;
    AppCatalog& operator=(const AppCatalog&) = delete;

    void Refresh();                       // enumerate again (asynchronous)
    bool Loaded() const;
    std::vector<CatalogApp> Apps() const; // sorted by name (user locale)
    // Pinned to the Windows start menu (not the taskbar: those are the bar's pins).
    std::vector<CatalogApp> StartPins() const;
    IconImage Icon(const std::wstring& id, int px); // empty until loaded
    // The start pins' icons at `px` are read after every refresh, so the start
    // menu's first opening shows them at once (requests for icons on screen go first).
    void PrefetchStartIcons(int px);

    static bool Launch(const CatalogApp& app);
    static PinnedApp PinFor(const CatalogApp& app);
    // Case-insensitive match of `query` against the name (and words in it).
    static bool Matches(const CatalogApp& app, const std::wstring& query);

private:
    void Worker();
    HWND notify_;
    UINT msg_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<CatalogApp> apps_, startPins_;
    // Worker only: the exported layout and the layout file's time it is from.
    std::vector<std::pair<std::wstring, std::wstring>> layout_; // key (packagedAppId | desktopAppLink), value
    FILETIME layoutTime_{};
    bool loaded_ = false, refresh_ = false, stop_ = false;
    int prefetchPx_ = 0;
    void QueuePrefetch(); // under mutex_
    std::deque<std::pair<std::wstring, int>> iconJobs_;
    std::map<std::pair<std::wstring, int>, IconImage> icons_;
    std::map<std::pair<std::wstring, int>, bool> requested_;
    std::thread thread_;
};

} // namespace atrium
