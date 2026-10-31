#include "shell/notification_listener.h"

#include "log.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include <set>

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

std::wstring NotificationListener::Probe()
{
    std::wstring out;
    try {
        (void)winrt::Windows::ApplicationModel::Package::Current();
        out += L"identity=packaged";
    } catch (...) {
        out += L"identity=none";
    }
    try {
        auto listener = wunm::UserNotificationListener::Current();
        auto access = listener.GetAccessStatus();
        out += std::wstring(L" access=") + AccessName(access);
        if (access == wunm::UserNotificationListenerAccessStatus::Allowed) {
            auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
            out += L" readable=" + std::to_wstring(list.Size());
            try {
                auto token = listener.NotificationChanged([](auto&&, auto&&) {});
                listener.NotificationChanged(token);
                out += L" event=ok";
            } catch (const winrt::hresult_error& e) {
                wchar_t hr[16];
                swprintf_s(hr, L"0x%08X", unsigned(e.code()));
                out += std::wstring(L" event=") + hr;
            }
        }
    } catch (const winrt::hresult_error& e) {
        wchar_t hr[16];
        swprintf_s(hr, L"0x%08X", unsigned(e.code()));
        out += std::wstring(L" error=") + hr;
    }
    return out;
}

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
            winrt::uninit_apartment();
            return;
        }
        // NotificationChanged fails for unpackaged processes (VERIFICATION 8.3):
        // poll instead. The first pass only records what is already there.
        std::set<uint32_t> seen;
        bool first = true;
        Log(L"listener: mirroring Windows notifications (polling every 2 s)");
        while (!stop_) {
            try {
                auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
                std::set<uint32_t> now;
                for (const auto& un : list) {
                    now.insert(un.Id());
                    if (first || seen.count(un.Id()))
                        continue;
                    auto* n = new Notice(ToNotice(un));
                    if (!PostMessageW(notify_, msg_, 0, reinterpret_cast<LPARAM>(n)))
                        delete n;
                }
                seen = std::move(now);
                first = false;
            } catch (const winrt::hresult_error& e) {
                Log(L"listener: read failed 0x%08X", unsigned(e.code()));
            }
            WaitForSingleObject(wake_, 2000);
        }
    } catch (const winrt::hresult_error& e) {
        Log(L"listener: unavailable 0x%08X", unsigned(e.code()));
    }
    winrt::uninit_apartment();
}

} // namespace atrium
