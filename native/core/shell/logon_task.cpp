#include "shell/logon_task.h"

#include "log.h"

#include <windows.h>
#include <sddl.h>
#include <taskschd.h>

#include <winrt/base.h>

namespace atrium {

namespace {

struct ComScope {
    bool owned;
    ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

struct Bstr {
    BSTR b;
    explicit Bstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), UINT(s.size()))) {}
    ~Bstr() { SysFreeString(b); }
    operator BSTR() const { return b; }
};

std::wstring UserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    std::wstring result;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    wchar_t* text = nullptr;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
        result = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return result;
}

// One task per user, as OneDrive names its own.
std::wstring TaskName(const std::wstring& sid) { return L"Atrium Startup-" + sid; }

winrt::com_ptr<ITaskFolder> RootFolder()
{
    winrt::com_ptr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service.put()))))
        return nullptr;
    VARIANT none{};
    if (FAILED(service->Connect(none, none, none, none)))
        return nullptr;
    winrt::com_ptr<ITaskFolder> folder;
    service->GetFolder(Bstr(L"\\"), folder.put());
    return folder;
}

std::wstring Xml(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s)
        out += c == L'&' ? L"&amp;" : c == L'<' ? L"&lt;" : c == L'>' ? L"&gt;" : std::wstring(1, c);
    return out;
}

} // namespace

bool RegisterLogonTask(const std::wstring& exe)
{
    ComScope com;
    const std::wstring sid = UserSid();
    auto folder = sid.empty() ? nullptr : RootFolder();
    if (!folder)
        return false;
    // Normal priority (a task's default is below normal), on battery too, never stopped.
    const std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
                             L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
                             L"<RegistrationInfo><Description>Starts Atrium when you sign in.</Description></RegistrationInfo>"
                             L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId></LogonTrigger></Triggers>"
                             L"<Principals><Principal id=\"Author\"><UserId>" + sid +
                             L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>"
                             L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
                             L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
                             L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority>"
                             L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings></Settings>"
                             L"<Actions Context=\"Author\"><Exec><Command>" + Xml(exe) + L"</Command></Exec></Actions></Task>";
    VARIANT none{};
    winrt::com_ptr<IRegisteredTask> task;
    HRESULT hr = folder->RegisterTask(Bstr(TaskName(sid)),
                                      Bstr(xml), TASK_CREATE_OR_UPDATE, none, none,
                                      TASK_LOGON_INTERACTIVE_TOKEN, none, task.put());
    if (FAILED(hr))
        Log(L"startup: the logon task could not be registered (0x%08X)", unsigned(hr));
    return SUCCEEDED(hr);
}

std::wstring LogonTaskProgram()
{
    ComScope com;
    const std::wstring sid = UserSid();
    auto folder = sid.empty() ? nullptr : RootFolder();
    winrt::com_ptr<IRegisteredTask> task;
    winrt::com_ptr<ITaskDefinition> definition;
    winrt::com_ptr<IActionCollection> actions;
    winrt::com_ptr<IAction> action;
    if (!folder || FAILED(folder->GetTask(Bstr(TaskName(sid)), task.put())) ||
        FAILED(task->get_Definition(definition.put())) || FAILED(definition->get_Actions(actions.put())) ||
        FAILED(actions->get_Item(1, action.put())))
        return {};
    auto exec = action.try_as<IExecAction>();
    BSTR path = nullptr;
    if (!exec || FAILED(exec->get_Path(&path)) || !path)
        return {};
    std::wstring result(path, SysStringLen(path));
    SysFreeString(path);
    if (result.size() >= 2 && result.front() == L'"' && result.back() == L'"')
        result = result.substr(1, result.size() - 2);
    return result;
}

bool RemoveLogonTask()
{
    ComScope com;
    const std::wstring sid = UserSid();
    auto folder = sid.empty() ? nullptr : RootFolder();
    if (!folder)
        return false;
    HRESULT hr = folder->DeleteTask(Bstr(TaskName(sid)), 0);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

} // namespace atrium
