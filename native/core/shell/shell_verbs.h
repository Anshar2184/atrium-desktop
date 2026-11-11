// The shell's own verbs for one file (the system's entries in Atrium's menus).
// A program's context menu in Explorer holds entries only the shell can run --
// "run as administrator", "properties", a program's own shell extensions -- so
// that menu is built once per right click and kept alive while Atrium shows the
// chosen verbs in its glass menu and runs the pick.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace atrium {

struct ShellVerb {
    std::wstring verb; // the shell's name for it, e.g. L"runas"
    std::wstring text; // the label the shell gives it, accelerators removed
    UINT command = 0;  // offset inside the shell menu: what InvokeCommand takes
    bool disabled = false;
};

class ShellVerbs {
public:
    ~ShellVerbs();
    ShellVerbs(const ShellVerbs&) = delete;
    ShellVerbs& operator=(const ShellVerbs&) = delete;

    // Builds the shell's menu for `path` (an exe, a shortcut or a shell: name).
    // Null when the path does not resolve or the shell offers no verbs.
    static std::shared_ptr<ShellVerbs> For(const std::wstring& path, HWND owner);

    const std::vector<ShellVerb>& All() const { return verbs_; }
    const ShellVerb* Find(const wchar_t* verb) const;
    // Runs one verb through the shell; the object must stay alive until it returns.
    bool Invoke(const ShellVerb& verb);

private:
    ShellVerbs() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<ShellVerb> verbs_;
};

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

} // namespace atrium
