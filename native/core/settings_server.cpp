#include "settings_server.h"

#include "log.h"
#include "ui/desktop_widgets.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr int kProtocol = 1;
constexpr DWORD kBuffer = 64 * 1024;

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Reply(const wchar_t* op, const wchar_t* message = nullptr)
{
    json::JsonObject o;
    o.SetNamedValue(L"op", json::JsonValue::CreateStringValue(op));
    if (message)
        o.SetNamedValue(L"message", json::JsonValue::CreateStringValue(message));
    return Narrow(std::wstring(o.Stringify()));
}

// Waits for an overlapped operation or the quit event. False on quit or error.
bool Wait(HANDLE pipe, OVERLAPPED& ov, HANDLE quit, DWORD& bytes, DWORD& error)
{
    HANDLE hs[] = {ov.hEvent, quit};
    if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        error = ERROR_OPERATION_ABORTED;
        return false;
    }
    error = GetOverlappedResult(pipe, &ov, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    return error == ERROR_SUCCESS || error == ERROR_MORE_DATA;
}

} // namespace

std::wstring SettingsPipeName()
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\Atrium.Core." + std::to_wstring(session);
}

SettingsServer::SettingsServer(HWND host, const Settings& saved) : host_(host), saved_(saved), current_(saved)
{
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
}

SettingsServer::~SettingsServer()
{
    SetEvent(quit_);
    if (thread_.joinable())
        thread_.join();
    CloseHandle(quit_);
}

Settings SettingsServer::Current()
{
    std::lock_guard g(lock_);
    return current_;
}

bool SettingsServer::Modify(const std::function<void(Settings&)>& change)
{
    std::lock_guard t(txn_);
    Settings saved, current;
    {
        std::lock_guard g(lock_);
        saved = saved_;
        current = current_;
    }
    change(saved);
    saved.Clamp();
    if (!SaveSettings(saved)) {
        Log(L"settings: could not save %s", SettingsPath().c_str());
        return false;
    }
    change(current);
    current.Clamp();
    {
        std::lock_guard g(lock_);
        saved_ = saved;
    }
    SetCurrent(current);
    return true;
}

void SettingsServer::SetCurrent(const Settings& s)
{
    {
        std::lock_guard g(lock_);
        current_ = s;
    }
    PostMessageW(host_, WM_ATRIUM_SETTINGS, 0, 0);
}

void SettingsServer::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wstring name = SettingsPipeName();
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        // First instance only: another process cannot pre-create our pipe name.
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kBuffer,
                                       kBuffer, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"pipe: CreateNamedPipe failed (%lu)", GetLastError());
            if (WaitForSingleObject(quit_, 2000) != WAIT_TIMEOUT)
                break;
            continue;
        }
        ready_ = true;
        Serve(pipe);
        CloseHandle(pipe);

        // A client that leaves without committing gets its preview reverted.
        std::lock_guard t(txn_);
        Settings saved, current;
        {
            std::lock_guard g(lock_);
            saved = saved_;
            current = current_;
        }
        if (current.ToJson() != saved.ToJson()) {
            Log(L"pipe: client left with an unsaved preview, reverted");
            SetCurrent(saved);
        }
    }
    winrt::uninit_apartment();
}

void SettingsServer::Serve(HANDLE pipe)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD bytes = 0, error = 0;
    if (!ConnectNamedPipe(pipe, &ov)) {
        error = GetLastError();
        if (error == ERROR_IO_PENDING && !Wait(pipe, ov, quit_, bytes, error)) {
            CloseHandle(ov.hEvent);
            return;
        }
        if (error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED && error != ERROR_SUCCESS) {
            CloseHandle(ov.hEvent);
            return;
        }
    }
    std::string buffer(kBuffer, '\0');
    for (;;) {
        std::string message;
        do {
            ResetEvent(ov.hEvent);
            bytes = 0;
            if (!ReadFile(pipe, buffer.data(), kBuffer, &bytes, &ov)) {
                error = GetLastError();
                if (error == ERROR_IO_PENDING) {
                    if (!Wait(pipe, ov, quit_, bytes, error))
                        goto done;
                } else if (error != ERROR_MORE_DATA) {
                    goto done; // client gone
                }
            } else {
                error = ERROR_SUCCESS;
            }
            message.append(buffer.data(), bytes);
        } while (error == ERROR_MORE_DATA && message.size() < 1024 * 1024);

        std::string reply = Handle(message);
        ResetEvent(ov.hEvent);
        if (!WriteFile(pipe, reply.data(), DWORD(reply.size()), &bytes, &ov)) {
            error = GetLastError();
            if (error != ERROR_IO_PENDING || !Wait(pipe, ov, quit_, bytes, error))
                goto done;
        }
    }
done:
    DisconnectNamedPipe(pipe);
    CloseHandle(ov.hEvent);
}

std::string SettingsServer::Handle(const std::string& request)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(Widen(request), o) || !o.HasKey(L"op"))
        return Reply(L"error", L"malformed request");
    std::wstring op(o.GetNamedString(L"op", L""));

    if (op == L"add-widget") {
        // Not a settings change (no txn_: the UI thread may be waiting for it in Modify).
        std::wstring kind(o.GetNamedString(L"kind", L"")), size(o.GetNamedString(L"size", L"small"));
        WidgetKind widgetKind;
        WidgetSize widgetSize;
        if (!DesktopWidgets::ParseNames(kind, size, widgetKind, widgetSize)) return Reply(L"error", L"unknown widget kind");
        DWORD_PTR added = 0;
        SendMessageTimeoutW(host_, WM_ATRIUM_ADD_WIDGET, WPARAM(widgetKind), LPARAM(widgetSize), SMTO_ABORTIFHUNG, 3000,
                            &added);
        return added ? Reply(L"ok") : Reply(L"error", L"no room for another widget");
    }

    std::lock_guard t(txn_);
    Settings saved, current;
    {
        std::lock_guard g(lock_);
        saved = saved_;
        current = current_;
    }
    if (op == L"get") {
        json::JsonObject r;
        r.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"state"));
        r.SetNamedValue(L"protocol", json::JsonValue::CreateNumberValue(kProtocol));
        r.SetNamedValue(L"saved", json::JsonObject::Parse(saved.ToJson()));
        r.SetNamedValue(L"current", json::JsonObject::Parse(current.ToJson()));
        r.SetNamedValue(L"path", json::JsonValue::CreateStringValue(SettingsPath()));
        return Narrow(std::wstring(r.Stringify()));
    }
    if (op == L"cancel") {
        Log(L"pipe: cancel, back to the saved settings");
        SetCurrent(saved);
        return Reply(L"ok");
    }
    if (op == L"preview" || op == L"commit") {
        if (!o.HasKey(L"settings") || o.GetNamedValue(L"settings").ValueType() != json::JsonValueType::Object)
            return Reply(L"error", L"missing settings object");
        Settings next;
        if (!Settings::FromJson(std::wstring(o.GetNamedObject(L"settings").Stringify()), current, next))
            return Reply(L"error", L"invalid settings");
        if (op == L"commit") {
            std::wstring warning;
            if (!SaveSettings(next, &warning)) {
                Log(L"pipe: could not save settings (%s)", SettingsPath().c_str());
                return Reply(L"error", L"could not save settings");
            }
            Log(L"pipe: commit saved");
            {
                std::lock_guard g(lock_);
                saved_ = next;
            }
            SetCurrent(next);
            if (!warning.empty()) {
                json::JsonObject reply;
                reply.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"ok"));
                reply.SetNamedValue(L"warning", json::JsonValue::CreateStringValue(warning));
                return Narrow(std::wstring(reply.Stringify()));
            }
            return Reply(L"ok");
        } // lock released before SetCurrent takes it again
        SetCurrent(next);
        return Reply(L"ok");
    }
    return Reply(L"error", L"unknown op");
}

} // namespace atrium
