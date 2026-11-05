// Periodic release checks belong to the resident process, not the settings window.
#pragma once

#include <windows.h>
#include <thread>

namespace atrium {

class UpdateChecker {
public:
    UpdateChecker(HWND notify, UINT message);
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

private:
    void Run();
    HWND notify_;
    UINT message_;
    HANDLE stop_;
    std::thread worker_;
};

} // namespace atrium
