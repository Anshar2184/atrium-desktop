// Experimental mirror of Windows notifications through
// UserNotificationListener (off by default, D23). Polls the toast list on a
// worker thread and posts new ones to the UI thread; it never dismisses or
// changes anything in Windows, so the system banners still appear.
#pragma once

#include "ui/notifications.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

namespace atrium {

class NotificationListener {
public:
    // Posts `msg` to `notify` with a heap-allocated Notice* in lParam (the receiver deletes it).
    NotificationListener(HWND notify, UINT msg);
    ~NotificationListener();
    NotificationListener(const NotificationListener&) = delete;
    NotificationListener& operator=(const NotificationListener&) = delete;

    // Identity, access status, readable count, event subscription: one log line (P05 re-check).
    static std::wstring Probe();

private:
    void Run();
    HWND notify_;
    UINT msg_;
    std::atomic<bool> stop_{false};
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

} // namespace atrium
