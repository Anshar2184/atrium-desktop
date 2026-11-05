#include "shell/update_checker.h"
#include "log.h"
#include "settings.h"
#include "ui/notifications.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace atrium {
namespace {
namespace json = winrt::Windows::Data::Json;
constexpr int64_t kIntervalSeconds = 6 * 60 * 60;
constexpr wchar_t kLatest[] = L"https://api.github.com/repos/wynter-yaxley/atrium-desktop/releases/latest";
using Version = std::array<unsigned, 4>;

std::optional<Version> ParseVersion(std::wstring_view tag)
{
    if (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V')) tag.remove_prefix(1);
    tag = tag.substr(0, tag.find(L'+'));
    Version version{};
    size_t part = 0;
    bool digit = false;
    for (wchar_t c : tag) {
        if (c == L'.' && digit && part < 3) {
            ++part;
            digit = false;
        } else if (c >= L'0' && c <= L'9') {
            version[part] = version[part] * 10 + (c - L'0');
            if (version[part] > 65535) return {};
            digit = true;
        } else return {};
    }
    return digit && part >= 1 ? std::optional<Version>(version) : std::nullopt;
}

Version CurrentVersion()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD unused = 0;
    std::vector<BYTE> data(GetFileVersionInfoSizeW(exe, &unused));
    VS_FIXEDFILEINFO* info = nullptr;
    UINT size = 0;
    if (data.empty() || !GetFileVersionInfoW(exe, 0, DWORD(data.size()), data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size))
        throw winrt::hresult_error(E_FAIL, L"Missing application version");
    return {HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

DWORD Delay(int64_t last, int64_t now)
{
    // A clock correction must not postpone checks indefinitely.
    return last > 0 && last <= now && now - last < kIntervalSeconds ? DWORD((kIntervalSeconds - (now - last)) * 1000) : 0;
}

std::unique_ptr<Notice> ReleaseNotice(const json::JsonObject& release, const Version& current, const std::wstring& notified)
{
    std::wstring tag = release.GetNamedString(L"tag_name").c_str();
    auto latest = ParseVersion(tag);
    if (!latest) throw winrt::hresult_invalid_argument(L"Invalid release version");
    auto previous = ParseVersion(notified);
    if (*latest <= current || (previous && *latest <= *previous) ||
        release.GetNamedBoolean(L"draft", false) || release.GetNamedBoolean(L"prerelease", false)) return {};
    auto notice = std::make_unique<Notice>();
    notice->source = L"atrium";
    notice->app = L"Atrium";
    notice->title = L"有可用更新";
    notice->body = tag + L" 已发布，点击前往更新。";
    notice->action = L"settings:updates";
    return notice;
}
} // namespace

UpdateChecker::UpdateChecker(HWND notify, UINT message) : notify_(notify), message_(message), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (!stop_) winrt::throw_last_error();
    worker_ = std::thread([this] { Run(); });
}

UpdateChecker::~UpdateChecker()
{
    SetEvent(stop_);
    worker_.join();
    CloseHandle(stop_);
}

void UpdateChecker::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        const Version current = CurrentVersion();
        std::wstring path = SettingsPath();
        path = path.substr(0, path.find_last_of(L'\\') + 1) + L"update-state.json";
        json::JsonObject state;
        if (!json::JsonObject::TryParse(ReadTextFile(path), state)) state = json::JsonObject();
        int64_t checked = int64_t(state.GetNamedNumber(L"checked", 0));
        std::wstring notified = state.GetNamedString(L"notified", L"").c_str();
        while (WaitForSingleObject(stop_, Delay(checked, Now())) == WAIT_TIMEOUT) {
            checked = Now();
            try {
                winrt::Windows::Web::Http::HttpClient http;
                http.DefaultRequestHeaders().UserAgent().TryParseAdd(L"Atrium-Desktop/1.0");
                http.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");
                auto request = http.GetStringAsync(winrt::Windows::Foundation::Uri(kLatest));
                const auto start = GetTickCount64();
                while (request.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
                    if (WaitForSingleObject(stop_, 100) == WAIT_OBJECT_0 || GetTickCount64() - start >= 15000) {
                        request.Cancel();
                        break;
                    }
                }
                if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
                auto release = json::JsonObject::Parse(request.GetResults());
                std::wstring tag = release.GetNamedString(L"tag_name").c_str();
                if (auto notice = ReleaseNotice(release, current, notified)) {
                    if (PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(notice.get()))) {
                        notice.release();
                        notified = tag;
                        Log(L"update: available %s", tag.c_str());
                    }
                }
                Log(L"update: checked latest release %s", tag.c_str());
            } catch (const winrt::hresult_error& error) {
                Log(L"update: check failed (0x%08X); retry in 6 hours", unsigned(error.code().value));
            }
            state.SetNamedValue(L"checked", json::JsonValue::CreateNumberValue(double(checked)));
            state.SetNamedValue(L"notified", json::JsonValue::CreateStringValue(notified));
            if (!WriteTextFile(path, state.Stringify().c_str())) Log(L"update: could not save check state");
        }
    } catch (const winrt::hresult_error& error) {
        Log(L"update: checker stopped (0x%08X)", unsigned(error.code().value));
    }
    winrt::uninit_apartment();
}
} // namespace atrium
