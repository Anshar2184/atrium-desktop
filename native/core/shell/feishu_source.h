// Feishu (Lark) messages as Atrium cards. Feishu does not publish Windows
// notifications either: it shows its own popup, a small untitled Chromium
// window (Chrome_WidgetWin_1) at the screen's corner, for about ten seconds.
// Its text is readable through UI Automation, read only: the first text is
// the chat or sender, the next the message. A worker thread looks for such a
// window of Feishu's processes a few times a second and posts a card for each
// new one (D33). Nothing in Feishu is changed; its popup stays as it is.
#pragma once

#include "ui/notifications.h"

#include <windows.h>

#include <atomic>
#include <thread>

namespace atrium {

class FeishuSource {
public:
    // Posts `msg` to `notify` with a heap-allocated Notice* in lParam (the receiver deletes it).
    FeishuSource(HWND notify, UINT msg);
    ~FeishuSource();
    FeishuSource(const FeishuSource&) = delete;
    FeishuSource& operator=(const FeishuSource&) = delete;

private:
    void Run();
    HWND notify_;
    UINT msg_;
    std::atomic<bool> stop_{false};
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

} // namespace atrium
