// What the shell knows about a program that Atrium's menus need: its
// AppUserModelID (to find the jump list it files its entries under) and the
// document lists it publishes when it has no jump list of its own.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace atrium {

// The standard property sheet of a file (the shell's "properties" entry).
bool ShowShellProperties(HWND owner, const std::wstring& path);

// One entry of an app's "recent" list (the jump list Explorer shows above the
// system menu's own entries).
struct ShellRecent {
    std::wstring name; // the document's display name
    std::wstring path; // what to open
};

// The AppUserModelID a shortcut or a program carries; empty when it has none.
std::wstring ShellAppId(const std::wstring& path);
// The app's recent documents, newest first; empty when it publishes none.
std::vector<ShellRecent> ShellRecentDocuments(const std::wstring& appId, int max);
// The same list of another kind: ADLT_RECENT (documents) or ADLT_FREQUENT (places).
std::vector<ShellRecent> ShellDocumentList(const std::wstring& appId, int kind, int max);
// The AppUserModelID of the running process whose image is `exePath` (Explorer and
// other programs that set theirs at run time only).
std::wstring ShellAppIdOfProcess(const std::wstring& exePath);

} // namespace atrium
