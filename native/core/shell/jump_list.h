// The app's own jump list: what the Windows taskbar shows when its icon is
// right-clicked (pinned places, "recent folders and workspaces", tasks).
//
// The system keeps it in two files per app under the Recent folder:
//   AutomaticDestinations\<hash>.automaticDestinations-ms  (DestList: MRU entries, pins)
//   CustomDestinations\<hash>.customDestinations-ms       (categories: custom, known, tasks)
// The file name is a hash of the AppUserModelID that the shell does not publish,
// so the entry point here finds the files by their own contents.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace atrium {

// One row of the system menu.
struct JumpItem {
    std::wstring name;    // what the menu shows, e.g. "HID_keyborad (工作区)"
    std::wstring target;  // file or folder the entry points at (may be a shell name)
    std::wstring program; // the program the entry starts (empty: open `target`)
    std::wstring args;    // arguments for `program`
    bool folder = false;
};

struct JumpCategory {
    std::wstring title; // empty: the category has no title of its own
    std::vector<JumpItem> items;
};

struct JumpListData {
    std::vector<JumpItem> pinned;          // shown first, as "已固定"
    std::vector<JumpCategory> categories;  // app-defined categories, in their own order
    std::vector<JumpItem> recent;          // the automatic list when no category covers it
};

// Reads the app's jump list. Null when it has none or it cannot be found.
// `exePath` (the resolved program) is used to tell the files apart.
std::shared_ptr<const JumpListData> LoadJumpList(const std::wstring& appId, const std::wstring& exePath);

// True when a path lives on a network share. Asking whether such a path exists
// can block for the SMB timeout (21 s seen here) when the server is gone.
bool IsRemotePath(const std::wstring& path);

// Indexes the jump list files of the profile once, on a worker thread: telling
// an app's file apart means reading them, which must not happen on a click.
void StartJumpListIndex(bool enabled);

} // namespace atrium
