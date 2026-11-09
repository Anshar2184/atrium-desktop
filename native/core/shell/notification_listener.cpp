#include "shell/notification_listener.h"

#include "log.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include <map>

namespace atrium {

namespace wun = winrt::Windows::UI::Notifications;
namespace wunm = winrt::Windows::UI::Notifications::Management;

namespace {

const wchar_t* AccessName(wunm::UserNotificationListenerAccessStatus s)
{
    switch (s) {
    case wunm::UserNotificationListenerAccessStatus::Allowed:
        return L"Allowed";
    case wunm::UserNotificationListenerAccessStatus::Denied:
        return L"Denied";
    default:
        return L"Unspecified";
    }
}

// A toast's lines (title, then the body), each poll: senders update a toast in place. Reading its
// app (AppInfo, DisplayInfo: the app's resources) costs far more, so only a toast to post does.
std::wstring TextOf(const wun::UserNotification& un)
{
    std::wstring all;
    try {
        if (auto binding = un.Notification().Visual().GetBinding(wun::KnownNotificationBindings::ToastGeneric()))
            for (const auto& text : binding.GetTextElements())
                all += std::wstring(text.Text()) + L"\x1f";
    } catch (...) {
    }
    return all;
}

Notice ToNotice(const wun::UserNotification& un)
{
    Notice n;
    n.source = L"windows";
    n.externalId = un.Id();
    try {
        auto info = un.AppInfo();
        n.appId = std::wstring(info.AppUserModelId());
        n.app = std::wstring(info.DisplayInfo().DisplayName());
    } catch (...) {
    }
    try {
        auto binding = un.Notification().Visual().GetBinding(wun::KnownNotificationBindings::ToastGeneric());
        if (binding) {
            bool first = true;
            for (const auto& text : binding.GetTextElements()) {
                std::wstring s(text.Text());
                if (first) {
                    n.title = s;
                    first = false;
                } else {
                    n.body += (n.body.empty() ? L"" : L"\n") + s;
                }
            }
        }
    } catch (...) {
    }
    if (!n.appId.empty())
        n.action = L"app:" + n.appId;
    return n;
}

} // namespace

NotificationListener::NotificationListener(HWND notify, UINT msg) : notify_(notify), msg_(msg)
{
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
}

NotificationListener::~NotificationListener()
{
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

void NotificationListener::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        auto listener = wunm::UserNotificationListener::Current();
        auto access = listener.GetAccessStatus();
        if (access != wunm::UserNotificationListenerAccessStatus::Allowed) {
            // Asking needs a user prompt, which belongs to the settings, not to a
            // background thread; the user grants it in Windows privacy settings.
            Log(L"listener: access %s, not mirroring Windows notifications", AccessName(access));
        }
        // NotificationChanged fails for unpackaged processes:
        // poll instead. The first pass only records what is already there;
        // later a toast is posted when it appears and again when its text
        // changes (senders update a toast in place).
        std::map<uint32_t, size_t> seen; // id -> hash of its text
        bool first = true;
        int posted = 0;
        if (access == wunm::UserNotificationListenerAccessStatus::Allowed)
            Log(L"listener: mirroring Windows notifications (polling every second)");
        while (!stop_) {
            try {
                // The user can withdraw the access in Windows' privacy settings at any time.
                auto nowAccess = listener.GetAccessStatus();
                if (nowAccess != access) {
                    Log(L"listener: access %s", AccessName(nowAccess));
                    access = nowAccess;
                }
                if (access == wunm::UserNotificationListenerAccessStatus::Allowed) {
                    auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
                    std::map<uint32_t, size_t> now;
                    for (const auto& un : list) {
                        size_t hash = std::hash<std::wstring>{}(TextOf(un));
                        now[un.Id()] = hash;
                        auto old = seen.find(un.Id());
                        if (first || (old != seen.end() && old->second == hash))
                            continue;
                        Notice n = ToNotice(un);
                        std::wstring app = n.app; // the UI thread owns the notice once posted
                        auto* p = new Notice(std::move(n));
                        if (!PostMessageW(notify_, msg_, 0, reinterpret_cast<LPARAM>(p)))
                            delete p;
                        else if (++posted <= 3 || posted % 50 == 0)
                            Log(L"listener: %s toast from %s (%d so far)", old == seen.end() ? L"new" : L"updated", app.c_str(), posted);
                    }
                    seen = std::move(now);
                    first = false;
                } else {
                    seen.clear();
                    first = true; // access restored later: existing notifications form a new baseline
                }
            } catch (const winrt::hresult_error& e) {
                Log(L"listener: read failed 0x%08X", unsigned(e.code()));
            }
            WaitForSingleObject(wake_, access == wunm::UserNotificationListenerAccessStatus::Allowed ? 1000 : 10000);
        }
    } catch (const winrt::hresult_error& e) {
        Log(L"listener: unavailable 0x%08X", unsigned(e.code()));
    }
    winrt::uninit_apartment();
}

} // namespace atrium
