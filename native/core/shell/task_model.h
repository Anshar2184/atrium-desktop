// Task model for the bar: pinned apps plus running app windows, grouped per
// application (D13). Lives on the UI thread (WinEvent hooks need its message
// loop); the bar reads immutable snapshots.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

// A pinned entry as stored in settings.json ("pins", docs/SETTINGS-PROTOCOL.md).
struct PinnedApp {
    std::wstring name;  // display name
    std::wstring path;  // .lnk, .exe or other launchable file; may be empty for Store apps
    std::wstring args;  // extra arguments when `path` is an .exe
    std::wstring aumid; // AppUserModelID, empty when unknown
    bool operator==(const PinnedApp&) const = default;
};

// Premultiplied BGRA, top-down, width * height * 4 bytes.
struct IconImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;
    bool empty() const { return bgra.empty(); }
};

struct TaskItem {
    std::wstring key;   // stable grouping key: lowercase AUMID if known, else lowercase exe path
    std::wstring name;  // pinned name, else window/app display name
    bool pinned = false;
    int pinIndex = -1;  // position in the pin list, -1 if not pinned
    std::vector<HWND> windows; // visible app windows of this group, most recently active first
    bool active = false;       // one of `windows` is the foreground window
    bool attention = false;    // a window is flashing (FlashWindowEx / HSHELL_FLASH)
    uint64_t iconVersion = 0;  // changes when the icon should be fetched again
};

// Icon of a shell item (a path or "shell:AppsFolder\\<id>") through
// IShellItemImageFactory, square, premultiplied. Blocking; call it off the UI
// thread. COM must be initialised on the calling thread.
// thumbnail: the picture itself (an image file) rather than its icon.
bool LoadShellImage(const std::wstring& parsingName, int sizePx, IconImage& out, bool thumbnail = false);

class TaskModel {
public:
    // Calls `onChanged` on the UI thread (coalesced, at most every ~30 ms)
    // whenever Snapshot() would return something different.
    TaskModel(HWND uiWindow, std::function<void()> onChanged);
    ~TaskModel();
    TaskModel(const TaskModel&) = delete;
    TaskModel& operator=(const TaskModel&) = delete;

    // Pinned items first in pin order, then running unpinned groups in the
    // order they first appeared. Unpinned groups are omitted if !showRunning.
    std::vector<TaskItem> Snapshot() const;

    void SetPins(const std::vector<PinnedApp>& pins);
    void SetShowRunning(bool show);

    // Icon for an item at `sizePx` (square), cached per key and size. Never
    // blocks for long: returns empty until loaded, then onChanged fires.
    IconImage Icon(const std::wstring& key, int sizePx);

    // Left click, like the Windows taskbar: no window -> launch; one window ->
    // minimize if it is the foreground window, otherwise restore and activate;
    // several windows -> activate the next one in turn.
    void Activate(const std::wstring& key);
    // Middle click / "new window": start another instance.
    void Launch(const std::wstring& key);
    // Politely close every window of the group (WM_CLOSE).
    void CloseAll(const std::wstring& key);
    // A pin for a running unpinned item (to append to settings).
    PinnedApp PinFor(const std::wstring& key) const;
    // Brings the group's most recent window to the front (never minimizes or
    // launches); used while files are dragged over the item.
    void Show(const std::wstring& key);
    // Moves a running unpinned group to `toIndex` among the unpinned groups.
    void MoveRunning(const std::wstring& key, int toIndex);
    // Files dropped on an item: open them with that app.
    bool CanOpenFiles(const std::wstring& key) const;
    bool OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files);
    // The desktop exe behind an item ("open file location"); empty for packaged apps.
    std::wstring FileLocation(const std::wstring& key) const;

    // The user's current Windows taskbar pins (desktop apps, from
    // %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar).
    static std::vector<PinnedApp> ImportWindowsPins();

    // Forwarded from the UI window procedure; returns true if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atrium
