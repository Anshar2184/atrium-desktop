#pragma once

#include <string>

namespace atrium {

// The current user's Task Scheduler task that starts Atrium at sign-in. Not the Run key:
// Explorer starts Run entries one after another once the desktop is up, so the system
// taskbar would stay for seconds; a logon task starts with the session.
bool RegisterLogonTask(const std::wstring& exe);
// The program the task starts, empty when there is no task.
std::wstring LogonTaskProgram();
bool RemoveLogonTask();

} // namespace atrium
