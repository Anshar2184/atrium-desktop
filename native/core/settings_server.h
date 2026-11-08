// Named-pipe endpoint for Atrium.Settings (docs/SETTINGS-PROTOCOL.md).
// One client at a time; previews only change memory, commit writes the file,
// and a client that disconnects without committing has its preview reverted.
#pragma once

#include "settings.h"

#include <windows.h>

#include <functional>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace atrium {

// Posted to the host window when the effective settings changed; lParam is
// unused, the host reads SettingsServer::Current().
constexpr UINT WM_ATRIUM_SETTINGS = WM_APP + 1;
// Sent by the pipe thread: WidgetKind in wParam, WidgetSize in lParam, both
// scalar values. A timeout must not leave pointers to the sender's stack.
constexpr UINT WM_ATRIUM_ADD_WIDGET = WM_APP + 9;

std::wstring SettingsPipeName();

class SettingsServer {
public:
    SettingsServer(HWND host, const Settings& saved);
    ~SettingsServer();
    SettingsServer(const SettingsServer&) = delete;
    SettingsServer& operator=(const SettingsServer&) = delete;

    Settings Current();
    bool Ready() const { return ready_.load(); }
    // A change made by the core itself (e.g. pinning from the bar's menu):
    // applied to the saved settings and to the current preview, then written.
    bool Modify(const std::function<void(Settings&)>& change);

private:
    void Run();
    void Serve(HANDLE pipe);
    std::string Handle(const std::string& request);
    void SetCurrent(const Settings& s);

    HWND host_;
    // txn_: one change at a time, from reading the settings to writing the
    // file (the pipe's thread and the UI thread's Modify); lock_ guards the fields.
    std::mutex txn_, lock_;
    Settings saved_, current_;
    HANDLE quit_;
    std::atomic_bool ready_ = false;
    std::thread thread_;
};

} // namespace atrium
