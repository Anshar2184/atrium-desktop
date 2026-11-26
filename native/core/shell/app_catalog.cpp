#include "shell/app_catalog.h"

#include "log.h"
#include "shell/app_catalog_rules.h"
#include "shell/shell_icon.h"
#include "shell/start_layout.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propkey.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace atrium {

namespace {

std::wstring Fold(const std::wstring& s) { return FoldCatalogText(s); }

void SortCatalogApps(std::vector<CatalogApp>& apps)
{
    std::ranges::sort(apps, [](const CatalogApp& a, const CatalogApp& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.name.c_str(), int(a.name.size()), b.name.c_str(),
                               int(b.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
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
            app.args = PropString(item2.get(), PKEY_Link_Arguments);
        }
        if (!app.name.empty() && !app.id.empty())
            apps.push_back(std::move(app));
    }
    SortCatalogApps(apps);
    return apps;
}

bool IsPath(const std::wstring& id) { return IsCatalogDrivePath(id); }

std::wstring ShellTarget(const std::wstring& id) { return IsPath(id) ? id : (id.starts_with(L"::{") ? L"shell:" : L"shell:AppsFolder\\") + id; }

constexpr const wchar_t* kHelperPackages[] = {
    L"microsoft.windowsfeedbackhub_",
    L"microsoft.gethelp_",
    L"microsoft.getstarted_",
    L"microsoft.windows.devhome_",
    L"microsoftcorporationii.quickassist_",
    L"microsoftcorporationii.microsoftfamily_",
    L"microsoft.powerautomatedesktop_",
    L"microsoft.xboxgamingoverlay_",
    L"microsoftwindows.client.",
};

int64_t Ticks(const FILETIME& t) { return int64_t((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime); }

int64_t Created(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    return !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) ? Ticks(fa.ftCreationTime) : 0;
}

struct PackageInfo {
    int64_t installed = 0;
    bool system = false;
};

std::map<std::wstring, PackageInfo> CollectInstalledPackages()
{
    std::map<std::wstring, PackageInfo> packages;
    try {
        namespace Wam = winrt::Windows::ApplicationModel;
        winrt::Windows::Management::Deployment::PackageManager manager;
        for (const auto& package : manager.FindPackagesForUser(L"")) {
            const auto id = package.Id();
            const std::wstring publisher = Fold(std::wstring(id.Publisher()));
            const std::wstring family = Fold(std::wstring(id.FamilyName()));
            const bool system = package.SignatureKind() == Wam::PackageSignatureKind::System || CatalogByMaker(publisher) ||
                                std::ranges::any_of(kHelperPackages, [&](const wchar_t* p) { return family.starts_with(p); });
            packages[family] = {package.InstalledDate().time_since_epoch().count(), system};
        }
    } catch (const winrt::hresult_error&) {
        Log(L"catalog: packages' install dates unavailable");
    }
    return packages;
}

struct ShortcutIndex {
    std::map<std::wstring, int64_t> byTarget;
    std::map<std::wstring, int64_t> byName;
    std::set<std::wstring> systemTargets;
    std::set<std::wstring> systemNames;
    std::vector<CatalogApp> shortcuts;
};

void RecordEarliest(std::map<std::wstring, int64_t>& m, const std::wstring& key, int64_t t)
{
    if (key.empty() || t == 0)
        return;
    auto [it, added] = m.try_emplace(key, t);
    if (!added)
        it->second = std::min(it->second, t);
}

bool IsSystemShortcutRelativePath(const std::wstring& within)
{
    const size_t slash = within.find(L'\\');
    if (slash == std::wstring::npos)
        return false;
    const std::wstring top = within.substr(0, slash);
    return IsSystemStartFolder(top) || IsToolStartFolder(top);
}

void IndexProgramsFolderLinks(const KNOWNFOLDERID& id, ShortcutIndex& index)
{
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &root)) || !root) {
        CoTaskMemFree(root);
        return;
    }
    std::vector<std::pair<std::wstring, std::wstring>> links;
    const std::wstring programs = root;
    CollectStartLinks(programs, links);
    CoTaskMemFree(root);
    for (const auto& [_, link] : links) {
        const int64_t t = Created(link);
        const std::wstring file = link.substr(link.find_last_of(L'\\') + 1);
        const std::wstring displayName = file.substr(0, file.size() - 4);
        const std::wstring name = Fold(displayName);
        const std::wstring within = Fold(link.substr(std::min(link.size(), programs.size() + 1)));
        const bool inSystem = IsSystemShortcutRelativePath(within);
        RecordEarliest(index.byName, name, t);
        if (inSystem)
            index.systemNames.insert(name);
        winrt::com_ptr<IShellLinkW> shell;
        wchar_t target[MAX_PATH] = {};
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shell.put()))) &&
            SUCCEEDED(shell.as<IPersistFile>()->Load(link.c_str(), STGM_READ)) && SUCCEEDED(shell->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH))) {
            const std::wstring expandedTarget = ExpandKnownEnvFolders(target);
            const std::wstring folded = Fold(expandedTarget);
            RecordEarliest(index.byTarget, folded, t);
            if (inSystem)
                index.systemTargets.insert(folded);
            wchar_t args[1024] = {};
            if (FAILED(shell->GetArguments(args, static_cast<int>(std::size(args)))))
                args[0] = L'\0';
            std::wstring aumid;
            if (auto props = shell.try_as<IPropertyStore>()) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                    aumid = pv.pwszVal;
                PropVariantClear(&pv);
            }
            index.shortcuts.push_back(CatalogApp{displayName, link, aumid, expandedTarget, args, t, inSystem});
        }
    }
}

std::wstring ProgramFilesVendor(const std::wstring& target)
{
    const size_t pf = target.find(L"\\program files");
    if (pf == std::wstring::npos)
        return {};
    const size_t afterRoot = target.find(L'\\', pf + 1);
    if (afterRoot == std::wstring::npos)
        return {};
    const size_t nextSlash = target.find(L'\\', afterRoot + 1);
    return target.substr(afterRoot, nextSlash - afterRoot);
}

void StampApp(CatalogApp& app, const std::map<std::wstring, PackageInfo>& packages, const ShortcutIndex& index, const std::wstring& windowsDir)
{
    const std::wstring& aumid = app.aumid.empty() ? app.id : app.aumid;
    if (const size_t bang = aumid.find(L'!'); bang != std::wstring::npos) {
        if (auto it = packages.find(Fold(aumid.substr(0, bang))); it != packages.end()) {
            app.installed = it->second.installed;
            app.system = it->second.system;
            return;
        }
    }
    const std::wstring target = Fold(app.target);
    const std::wstring id = Fold(app.id);
    const std::wstring exe = target.substr(target.find_last_of(L'\\') + 1);
    // File Explorer is Windows' own and everyday, as the Finder.
    app.system = (target.starts_with(windowsDir) && exe != L"explorer.exe") || index.systemTargets.contains(target) ||
                 index.systemNames.contains(Fold(app.name)) || (id.starts_with(L"microsoft.windows.") && id != L"microsoft.windows.explorer") ||
                 id.starts_with(L"::{") || CatalogByMaker(ProgramFilesVendor(target));
    if (IsPath(app.id) && Fold(app.id).ends_with(L".lnk")) {
        app.installed = Created(app.id);
        return;
    }
    if (auto it = index.byTarget.find(target); !app.target.empty() && it != index.byTarget.end())
        app.installed = it->second;
    else if (auto named = index.byName.find(Fold(app.name)); named != index.byName.end())
        app.installed = named->second;
    else
        app.installed = Created(app.target);
}

// When each app came, for Launchpad's order: a packaged app's package install date; a desktop app's start
// menu shortcut's creation (the installer made it), else its program's; 0 when unknown (Windows' own).
void StampInstalled(std::vector<CatalogApp>& apps)
{
    const std::map<std::wstring, PackageInfo> packages = CollectInstalledPackages();
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring windowsDir = Fold(std::wstring(windows) + L"\\");
    ShortcutIndex index;
    IndexProgramsFolderLinks(FOLDERID_CommonPrograms, index);
    IndexProgramsFolderLinks(FOLDERID_Programs, index);
    if (MergeStartMenuShortcuts(apps, index.shortcuts, &AppCatalog::IsLaunchable))
        SortCatalogApps(apps);
    for (CatalogApp& app : apps)
        StampApp(app, packages, index, windowsDir);
}

} // namespace

bool AppCatalog::IsLaunchable(const CatalogApp& app)
{
    const std::wstring id = Fold(app.id), name = Fold(app.name);
    if (id.starts_with(L"http:") || id.starts_with(L"https:") || id.starts_with(L"::{") || id.starts_with(L"shell:"))
        return false;
    for (const wchar_t* ext : {L".url", L".txt", L".pdf", L".chm", L".htm", L".html", L".md", L".rtf", L".doc", L".docx", L".website"})
        if (id.ends_with(ext))
            return false;
    if (CatalogNameExcluded(name))
        return false;
    return !NotAnAppShortcut(app);
}

AppCatalog::AppCatalog(HWND notify, UINT readyMsg) : notify_(notify), msg_(readyMsg), stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), refresh_(true)
{
    if (!stopEvent_)
        winrt::throw_last_error();
    thread_ = std::thread([this] { Worker(); });
}

AppCatalog::~AppCatalog()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    if (stopEvent_)
        SetEvent(stopEvent_);
    cv_.notify_all();
    thread_.join();
    if (stopEvent_)
        CloseHandle(stopEvent_);
}

void AppCatalog::Refresh(bool forceLayout)
{
    {
        std::lock_guard g(mutex_);
        refresh_ = true;
        forceLayout_ |= forceLayout;
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

std::vector<CatalogApp> AppCatalog::StartPins() const
{
    std::lock_guard g(mutex_);
    return startPins_;
}

std::vector<CatalogFolder> AppCatalog::StartPinGroups() const
{
    std::lock_guard g(mutex_);
    return startPinGroups_;
}

std::vector<CatalogFolder> AppCatalog::StartFolders() const
{
    std::lock_guard g(mutex_);
    return startFolders_;
}

void AppCatalog::PrefetchStartIcons(int px)
{
    std::lock_guard g(mutex_);
    cache_.prefetchPx = std::clamp(px, 8, 512);
    QueuePrefetch();
}

void AppCatalog::SetStartIds(std::vector<std::wstring> ids)
{
    std::lock_guard g(mutex_);
    cache_.startIds = std::set<std::wstring>(ids.begin(), ids.end());
    QueuePrefetch();
}

void AppCatalog::QueuePrefetch()
{
    if (cache_.prefetchPx <= 0)
        return;
    const ULONGLONG now = GetTickCount64();
    auto queue = [&](const std::wstring& id) {
        IconKey key{id, cache_.prefetchPx};
        if (!cache_.icons.contains(key))
            cache_.QueueIcon(key, IconJobTier::Prefetch, now);
    };
    for (const CatalogApp& app : startPins_)
        queue(app.id);
    for (const std::wstring& id : cache_.startIds)
        queue(id);
    cv_.notify_all();
}

IconImage AppCatalog::Icon(const std::wstring& id, int px)
{
    px = std::clamp(px, 8, 512);
    std::lock_guard g(mutex_);
    IconKey key{id, px};
    if (auto it = cache_.icons.find(key); it != cache_.icons.end()) {
        it->second.used = ++cache_.iconUse;
        return it->second.image;
    }
    if (cache_.QueueIcon(key, IconJobTier::Visible, GetTickCount64()))
        cv_.notify_one();
    return {};
}

void AppCatalog::SetAheadIcons(const std::vector<std::wstring>& ids, int px)
{
    px = std::clamp(px, 8, 512);
    std::lock_guard g(mutex_);
    if (cache_.SetAhead(ids, px, GetTickCount64()))
        cv_.notify_all();
}

void AppCatalog::PrefetchIcons(const std::vector<std::wstring>& ids, int px)
{
    px = std::clamp(px, 8, 512);
    std::lock_guard g(mutex_);
    const ULONGLONG now = GetTickCount64();
    bool queued = false;
    for (const std::wstring& id : ids) {
        IconKey key{id, px};
        if (!cache_.icons.contains(key))
            queued |= cache_.QueueIcon(key, IconJobTier::Prefetch, now);
    }
    if (queued)
        cv_.notify_all();
}

bool AppCatalog::IconsReady(const std::vector<std::wstring>& ids, int px)
{
    px = std::clamp(px, 8, 512);
    std::lock_guard g(mutex_);
    const ULONGLONG now = GetTickCount64();
    bool ready = true, asked = false;
    for (const std::wstring& id : ids) {
        IconKey key{id, px};
        if (cache_.icons.contains(key))
            continue;
        if (auto it = cache_.failed.find(key); it != cache_.failed.end() && !cache_.IsRequested(key) && it->second.attempts >= 1)
            continue; // no icon to wait for: its quiet square
        ready = false;
        asked |= cache_.QueueIcon(key, IconJobTier::Visible, now);
    }
    if (asked)
        cv_.notify_all();
    return ready;
}

void AppCatalog::SetVisibleIcons(VisibleIconsOwner owner, const std::vector<std::pair<std::wstring, int>>& icons)
{
    std::lock_guard g(mutex_);
    if (!cache_.SetVisible(size_t(owner), icons))
        return;
    PruneIcons();
}

void AppCatalog::PruneIcons(bool sweepStale)
{
    cache_.Prune(loaded_, sweepStale, [&](const std::wstring& id) { return std::ranges::any_of(startPins_, [&](const auto& app) { return app.id == id; }); });
}

void AppCatalog::RunRefresh(bool forceLayout)
{
    const ULONGLONG t0 = GetTickCount64();
    std::vector<CatalogApp> apps = Enumerate();
    StampInstalled(apps);
    Log(L"catalog: %zu apps in %llu ms", apps.size(), GetTickCount64() - t0);

    RefreshStartLayout(forceLayout);
    std::vector<CatalogFolder> pinGroups;
    std::vector<CatalogApp> pins = ResolveStartLayoutPins(apps, pinGroups);
    std::vector<CatalogFolder> folders = ReadStartMenuFolders(apps);
    {
        std::lock_guard g(mutex_);
        apps_ = std::move(apps);
        startPins_ = std::move(pins);
        startPinGroups_ = std::move(pinGroups);
        startFolders_ = std::move(folders);
        cache_.liveIds.clear();
        for (const auto& app : apps_)
            cache_.liveIds.insert(app.id);
        for (const auto& app : startPins_)
            cache_.liveIds.insert(app.id);
        for (const auto& folder : startFolders_) {
            for (const auto& app : folder.apps)
                cache_.liveIds.insert(app.id);
        }
        cache_.liveIds.insert(cache_.startIds.begin(), cache_.startIds.end());
        cache_.failed.clear(); // catalogue changes allow another bounded attempt
        loaded_ = true;
        PruneIcons(true);
        QueuePrefetch();
    }
    PostMessageW(notify_, msg_, 0, 0);
}

void AppCatalog::RunIconJob(const IconKey& job)
{
    IconImage img;
    LoadShellImage(ShellTarget(job.first), job.second, img);
    {
        std::lock_guard g(mutex_);
        cache_.activeJob.reset();
        if (!img.Empty()) {
            cache_.InsertIcon(job, std::move(img));
        } else {
            auto& failure = cache_.failed[job];
            ++failure.attempts;
            failure.time = GetTickCount64();
        }
        PruneIcons();
    }
    PostMessageW(notify_, msg_, 1, 0);
}

void AppCatalog::Worker()
{
    // STA: shell item image factories of some handlers need it.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    for (;;) {
        bool refresh = false, forceLayout = false;
        IconKey job;
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || refresh_ || cache_.HasWork(); });
            if (stop_)
                break;
            if (cache_.PopNextJob(job)) {
                // Icons before a refresh (it takes about a second).
            } else if (refresh_) {
                refresh = true;
                refresh_ = false;
                forceLayout = forceLayout_;
                forceLayout_ = false;
            } else {
                continue;
            }
        }
        if (refresh)
            RunRefresh(forceLayout);
        else
            RunIconJob(job);
    }
    winrt::uninit_apartment();
}

bool AppCatalog::Launch(const CatalogApp& app)
{
    std::wstring path = ShellTarget(app.id);
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

std::wstring AppCatalog::FileLocation(const CatalogApp& app)
{
    for (const std::wstring& file : {app.target, IsPath(app.id) ? app.id : std::wstring()})
        if (!file.empty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
            return file;
    return {};
}

bool AppCatalog::LaunchAsAdmin(const CatalogApp& app)
{
    // The shortcut keeps its arguments and working folder; without one, the exe with the link's arguments.
    const bool link = IsPath(app.id) && GetFileAttributesW(app.id.c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::wstring file = link ? app.id : app.target;
    if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    const std::wstring dir = link ? std::wstring() : file.substr(0, file.find_last_of(L"\\/"));
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = link || app.args.empty() ? nullptr : app.args.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CANCELLED) // the user said no at the prompt
            Log(L"catalog: elevated launch of %s failed (%lu)", file.c_str(), error);
        return false;
    }
    Log(L"catalog: launched %s as administrator", file.c_str());
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
    if (!app.target.empty() && lower.size() > 4 && lower.substr(lower.size() - 4) == L".exe") {
        p.path = app.target;
        p.args = app.args;
    } else if (IsPath(app.id))
        p.path = app.id; // a shortcut outside the catalog
    else
        p.aumid = !app.aumid.empty() ? app.aumid : app.id;
    return p;
}

AppCatalog::PreparedQuery AppCatalog::PrepareQuery(const std::wstring& query)
{
    PreparedQuery out;
    if (query.empty())
        return out;
    out.folded = Fold(query);
    out.asciiAlnum = !out.folded.empty() && std::ranges::all_of(out.folded, [](wchar_t c) { return c < 128 && iswalnum(c); });
    out.pathMatch = out.folded.size() >= 3;
    return out;
}

bool AppCatalog::MatchesPrepared(const CatalogApp& app, const PreparedQuery& query)
{
    if (query.folded.empty())
        return true;
    if (Fold(app.name).find(query.folded) != std::wstring::npos)
        return true;
    // Localised names hide the English one ("计算器"); the id and the exe
    // usually carry it (Microsoft.WindowsCalculator_..., notepad.exe).
    if (query.asciiAlnum && NameInitials(app.name).starts_with(query.folded))
        return true;
    auto tail = [](const std::wstring& s) {
        size_t p = s.find_last_of(L"\\/");
        return p == std::wstring::npos ? s : s.substr(p + 1);
    };
    return query.pathMatch && (Fold(tail(app.id)).find(query.folded) != std::wstring::npos || Fold(tail(app.target)).find(query.folded) != std::wstring::npos);
}

bool AppCatalog::Matches(const CatalogApp& app, const std::wstring& query) { return MatchesPrepared(app, PrepareQuery(query)); }

} // namespace atrium
