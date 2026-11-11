// What the shell knows about a program (see shell_verbs.h): the AppUserModelID
// it files its jump list under, and the document lists it publishes.

#include "shell/shell_verbs.h"

#include "log.h"

#include <propsys.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <winrt/base.h>

namespace atrium {

bool ShowShellProperties(HWND owner, const std::wstring& path)
{
    if (path.empty())
        return false;
    // The documented entry point: the same property sheet Explorer opens.
    return SHObjectProperties(owner, SHOP_FILEPATH, path.c_str(), nullptr) != FALSE;
}

// PKEY_AppUserModel_ID, declared here so the shell's property key headers (and
// their PROPERTYKEY definitions) stay out of this file.
static const PROPERTYKEY kAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

std::wstring ShellAppId(const std::wstring& path)
{
    if (path.empty())
        return {};
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(store.put()))) || !store)
        return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring id;
    if (SUCCEEDED(store->GetValue(kAppUserModelId, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        id = value.pwszVal;
    PropVariantClear(&value);
    return id;
}

std::wstring ShellAppIdOfProcess(const std::wstring& exePath)
{
    if (exePath.empty())
        return {};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};
    std::wstring id;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && id.empty(); ok = Process32NextW(snapshot, &entry)) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t image[32768]{};
        DWORD size = DWORD(std::size(image));
        if (QueryFullProcessImageNameW(process, 0, image, &size) && _wcsicmp(image, exePath.c_str()) == 0) {
            wchar_t appId[256]{};
            UINT32 length = UINT32(std::size(appId));
            if (SUCCEEDED(GetApplicationUserModelId(process, &length, appId)) && length > 1)
                id = appId;
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return id;
}

std::vector<ShellRecent> ShellRecentDocuments(const std::wstring& appId, int max)
{
    return ShellDocumentList(appId, ADLT_RECENT, max);
}

std::vector<ShellRecent> ShellDocumentList(const std::wstring& appId, int kind, int max)
{
    std::vector<ShellRecent> out;
    if (appId.empty() || max <= 0)
        return out;
    winrt::com_ptr<IApplicationDocumentLists> lists;
    if (FAILED(CoCreateInstance(CLSID_ApplicationDocumentLists, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(lists.put()))) || !lists)
        return out;
    if (FAILED(lists->SetAppID(appId.c_str())))
        return out;
    winrt::com_ptr<IObjectArray> array;
    if (FAILED(lists->GetList(APPDOCLISTTYPE(kind), 0, IID_PPV_ARGS(array.put()))) || !array)
        return out;
    UINT count = 0;
    array->GetCount(&count);
    for (UINT i = 0; i < count && int(out.size()) < max; ++i) {
        winrt::com_ptr<IShellItem> item;
        if (FAILED(array->GetAt(i, IID_PPV_ARGS(item.put()))) || !item)
            continue;
        PWSTR name = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name &&
            SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path)) && path) {
            out.push_back({name, path});
        }
        CoTaskMemFree(name);
        CoTaskMemFree(path);
    }
    return out;
}

} // namespace atrium
