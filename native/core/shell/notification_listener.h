// Windows notifications as Atrium cards, through UserNotificationListener
// (on by default since v0.6, D24). Unpackaged processes cannot subscribe to its
// change event, so a worker thread reads the toast list every second and posts
// new toasts, and toasts whose text changed, to the UI thread. The first read
// only records what is already there. It never dismisses or changes anything
// in Windows, so the system banners still appear unless the user turns an
// app's banners off in Windows (keeping it in the notification centre).
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
