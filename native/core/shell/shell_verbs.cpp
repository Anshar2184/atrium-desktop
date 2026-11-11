// Shell verbs (see shell_verbs.h): SHParseDisplayName + IContextMenu, the same
// menu Explorer shows, read into Atrium's MenuEntry rows.

#include "shell/shell_verbs.h"

#include "log.h"

#include <propsys.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <winrt/base.h>

namespace atrium {

struct ShellVerbs::Impl {
    IContextMenu* context = nullptr;
    HMENU menu = nullptr;
    HWND owner = nullptr;
    std::wstring path;

    ~Impl()
    {
        if (menu)
            DestroyMenu(menu);
        if (context)
            context->Release();
    }
};

ShellVerbs::~ShellVerbs() = default;

std::shared_ptr<ShellVerbs> ShellVerbs::For(const std::wstring& path, HWND owner)
{
    if (path.empty())
        return nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
        Log(L"shell menu: '%s' does not resolve", path.c_str());
        return nullptr;
    }
    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    const HRESULT bind = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&folder), &child);
    auto result = std::shared_ptr<ShellVerbs>(new ShellVerbs);
    result->impl_ = std::make_unique<Impl>();
    result->impl_->owner = owner;
    result->impl_->path = path;
    if (SUCCEEDED(bind) && folder) {
        if (FAILED(folder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr,
                                         reinterpret_cast<void**>(&result->impl_->context)))) {
            result->impl_->context = nullptr;
        }
        folder->Release();
    }
    CoTaskMemFree(pidl);
    if (!result->impl_->context)
        return nullptr;

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return nullptr;
    result->impl_->menu = menu;
    // Command ids start at 1, so a row's command is its id minus one.
    const HRESULT query = result->impl_->context->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
    if (FAILED(query)) {
        Log(L"shell menu: QueryContextMenu failed for '%s' (0x%08lX)", path.c_str(), unsigned(query));
        return nullptr;
    }
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        wchar_t text[512]{};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_FTYPE;
        info.dwTypeData = text;
        info.cch = UINT(std::size(text) - 1);
        if (!GetMenuItemInfoW(menu, UINT(i), TRUE, &info))
            continue;
        const UINT type = info.fType & ~MFT_SEPARATOR;
        if (info.fType & MFT_SEPARATOR)
            continue; // Atrium draws its own separators
        std::wstring label = text;
        label.erase(std::remove(label.begin(), label.end(), L'&'), label.end()); // accelerators
        if (label.empty())
            continue;
        wchar_t verb[256]{};
        std::wstring name;
        if (SUCCEEDED(result->impl_->context->GetCommandString(info.wID - 1, GCS_VERBW, nullptr,
                                                               reinterpret_cast<char*>(verb), UINT(std::size(verb)))))
            name = verb;
        ShellVerb entry;
        entry.verb = std::move(name);
        entry.text = std::move(label);
        entry.command = info.wID - 1;
        entry.disabled = (info.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
        result->verbs_.push_back(std::move(entry));
        (void)type;
    }
    return result->verbs_.empty() ? nullptr : std::move(result);
}

const ShellVerb* ShellVerbs::Find(const wchar_t* verb) const
{
    if (!verb)
        return nullptr;
    for (const ShellVerb& v : verbs_)
        if (v.verb == verb)
            return &v;
    return nullptr;
}

bool ShellVerbs::Invoke(const ShellVerb& verb)
{
    if (!impl_ || !impl_->context)
        return false;
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = impl_->owner ? impl_->owner : GetForegroundWindow();
    info.lpVerb = MAKEINTRESOURCEA(verb.command);
    info.lpVerbW = MAKEINTRESOURCEW(verb.command);
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hr = impl_->context->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
    if (FAILED(hr))
        Log(L"shell menu: '%s' failed (0x%08lX)", verb.verb.c_str(), unsigned(hr));
    return SUCCEEDED(hr);
}

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

std::vector<ShellRecent> ShellRecentDocuments(const std::wstring& appId, int max)
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
    if (FAILED(lists->GetList(ADLT_RECENT, 0, IID_PPV_ARGS(array.put()))) || !array)
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
