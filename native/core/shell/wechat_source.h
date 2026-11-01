// WeChat (Weixin) messages as Atrium cards. WeChat does not publish Windows
// notifications, but its main window's chat list is readable through UI
// Automation, read only: each row (mmui::ChatSessionCell) names the chat, the
// unread count "[N条]", the newest message's summary and whether the chat is
// muted ("消息免打扰"). A worker thread reads the list every two seconds;
// the first read is the baseline, then a rising unread count in a chat that
// is not muted posts a card. Only the rows WeChat has realised (about ten,
// the top of the list, where new messages land) are seen; nothing in WeChat
// is changed, no accessibility switches are turned on (D29).
#pragma once

#include "ui/notifications.h"

#include <windows.h>

#include <atomic>
#include <thread>

namespace atrium {

class WeChatSource {
public:
    // Posts `msg` to `notify` with a heap-allocated Notice* in lParam (the receiver deletes it).
    WeChatSource(HWND notify, UINT msg);
    ~WeChatSource();
    WeChatSource(const WeChatSource&) = delete;
    WeChatSource& operator=(const WeChatSource&) = delete;

private:
    void Run();
    HWND notify_;
    UINT msg_;
    std::atomic<bool> stop_{false};
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

} // namespace atrium
