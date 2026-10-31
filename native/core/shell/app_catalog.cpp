#include "shell/app_catalog.h"

#include "log.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propkey.h>

#include <winrt/base.h>

#include <algorithm>

namespace atrium {

namespace {

std::wstring Fold(const std::wstring& s)
{
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), out.data(), int(out.size()), nullptr,
                      nullptr, 0);
    return out;
}

std::wstring PropString(IShellItem2* item, const PROPERTYKEY& key)
{
    PWSTR v = nullptr;
    std::wstring s;
    if (SUCCEEDED(item->GetString(key, &v)) && v)
        s = v;
    CoTaskMemFree(v);
    return s;
}

std::vector<CatalogApp> Enumerate()
{
    std::vector<CatalogApp> apps;
    winrt::com_ptr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(folder.put()))))
        return apps;
    winrt::com_ptr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put()))))
        return apps;
    for (;;) {
        winrt::com_ptr<IShellItem> item;
        if (items->Next(1, item.put(), nullptr) != S_OK)
            break;
        CatalogApp app;
        PWSTR s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s)
            app.name = s;
        CoTaskMemFree(s);
        s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &s)) && s)
            app.id = s;
        CoTaskMemFree(s);
        if (auto item2 = item.try_as<IShellItem2>()) {
            app.aumid = PropString(item2.get(), PKEY_AppUserModel_ID);
            app.target = PropString(item2.get(), PKEY_Link_TargetParsingPath);
        }
        if (!app.name.empty() && !app.id.empty())
            apps.push_back(std::move(app));
    }
    std::sort(apps.begin(), apps.end(), [](const CatalogApp& a, const CatalogApp& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.name.c_str(), int(a.name.size()), b.name.c_str(),
                               int(b.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

} // namespace

AppCatalog::AppCatalog(HWND notify, UINT readyMsg) : notify_(notify), msg_(readyMsg)
{
    refresh_ = true;
    thread_ = std::thread([this] { Worker(); });
}

AppCatalog::~AppCatalog()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void AppCatalog::Refresh()
{
    {
        std::lock_guard g(mutex_);
        refresh_ = true;
    }
    cv_.notify_all();
}

bool AppCatalog::Loaded() const
{
    std::lock_guard g(mutex_);
    return loaded_;
}

std::vector<CatalogApp> AppCatalog::Apps() const
{
    std::lock_guard g(mutex_);
    return apps_;
}

IconImage AppCatalog::Icon(const std::wstring& id, int px)
{
    std::lock_guard g(mutex_);
    auto key = std::make_pair(id, px);
    if (auto it = icons_.find(key); it != icons_.end())
        return it->second;
    if (!requested_[key]) {
        requested_[key] = true;
        iconJobs_.push_back(key);
        cv_.notify_all();
    }
    return {};
}

void AppCatalog::Worker()
{
    // STA: shell item image factories of some handlers need it.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    for (;;) {
        bool refresh = false;
        std::pair<std::wstring, int> job;
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || refresh_ || !iconJobs_.empty(); });
            if (stop_)
                break;
            if (refresh_) {
                refresh = true;
                refresh_ = false;
            } else {
                // Newest requests first: they are what is on screen now.
                job = iconJobs_.back();
                iconJobs_.pop_back();
            }
        }
        if (refresh) {
            ULONGLONG t0 = GetTickCount64();
            std::vector<CatalogApp> apps;
            try {
                apps = Enumerate();
            } catch (...) {
                Log(L"catalog: enumeration failed");
            }
            Log(L"catalog: %zu apps in %llu ms", apps.size(), GetTickCount64() - t0);
            {
                std::lock_guard g(mutex_);
                apps_ = std::move(apps);
                loaded_ = true;
            }
            PostMessageW(notify_, msg_, 0, 0);
            continue;
        }
        IconImage img;
        LoadShellImage(L"shell:AppsFolder\\" + job.first, job.second, img);
        {
            std::lock_guard g(mutex_);
            icons_[job] = std::move(img);
        }
        PostMessageW(notify_, msg_, 1, 0);
    }
    winrt::uninit_apartment();
}

bool AppCatalog::Launch(const CatalogApp& app)
{
    std::wstring path = L"shell:AppsFolder\\" + app.id;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        Log(L"catalog: launch of %s failed (%lu)", app.id.c_str(), GetLastError());
        return false;
    }
    Log(L"catalog: launched %s", app.id.c_str());
    return true;
}

PinnedApp AppCatalog::PinFor(const CatalogApp& app)
{
    PinnedApp p;
    p.name = app.name;
    // Desktop apps pin by their exe so the pin matches their windows; the rest
    // by the AppsFolder id, which also starts them (shell:AppsFolder\<id>).
    std::wstring lower = app.target;
    for (auto& c : lower)
        c = wchar_t(towlower(c));
    if (!app.target.empty() && lower.size() > 4 && lower.substr(lower.size() - 4) == L".exe")
        p.path = app.target;
    else
        p.aumid = !app.aumid.empty() ? app.aumid : app.id;
    return p;
}

bool AppCatalog::Matches(const CatalogApp& app, const std::wstring& query)
{
    if (query.empty())
        return true;
    std::wstring q = Fold(query);
    if (Fold(app.name).find(q) != std::wstring::npos)
        return true;
    // Localised names hide the English one ("计算器"); the id and the exe
    // usually carry it (Microsoft.WindowsCalculator_..., notepad.exe).
    auto tail = [](const std::wstring& s) { size_t p = s.find_last_of(L"\\/"); return p == std::wstring::npos ? s : s.substr(p + 1); };
    return q.size() >= 3 && (Fold(tail(app.id)).find(q) != std::wstring::npos || Fold(tail(app.target)).find(q) != std::wstring::npos);
}

} // namespace atrium
