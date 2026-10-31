// Named-pipe endpoint for Atrium.Settings (docs/SETTINGS-PROTOCOL.md).
// One client at a time; previews only change memory, commit writes the file,
// and a client that disconnects without committing has its preview reverted.
#pragma once

#include "settings.h"

#include <windows.h>

#include <mutex>
#include <string>
#include <thread>

namespace atrium {

// Posted to the host window when the effective settings changed; lParam is
// unused, the host reads SettingsServer::Current().
constexpr UINT WM_ATRIUM_SETTINGS = WM_APP + 1;

std::wstring SettingsPipeName();

class SettingsServer {
public:
    SettingsServer(HWND host, const Settings& saved);
    ~SettingsServer();
    SettingsServer(const SettingsServer&) = delete;
    SettingsServer& operator=(const SettingsServer&) = delete;

    Settings Current();

private:
    void Run();
    void Serve(HANDLE pipe);
    std::string Handle(const std::string& request);
    void SetCurrent(const Settings& s);

    HWND host_;
    std::mutex lock_;
    Settings saved_, current_;
    HANDLE quit_;
    std::thread thread_;
};

} // namespace atrium
