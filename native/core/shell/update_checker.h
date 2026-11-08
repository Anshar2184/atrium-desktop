// Periodic release checks belong to the resident process, not the settings window.
#pragma once

#include <windows.h>
#include <thread>
#include <string>

namespace atrium {
class Notifications;

class UpdateChecker {
public:
    explicit UpdateChecker(HWND notify);
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;
    void NotifyPrepared(Notifications& notifications); // UI thread; completion or next startup

private:
    void Run();
    HWND notify_;
    HANDLE stop_;
    std::wstring cache_;
    std::thread worker_;
};

} // namespace atrium
