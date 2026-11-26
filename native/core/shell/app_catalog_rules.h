// Word-boundary exclusion/maker matching and Start Menu shortcut merging for AppCatalog.
#pragma once

#include "shell/app_catalog.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium {

inline std::wstring FoldCatalogText(const std::wstring& s)
{
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), out.data(), int(out.size()), nullptr, nullptr, 0);
    return out;
}

inline bool HasCatalogWordToken(std::wstring_view foldedText, std::wstring_view foldedToken)
{
    if (foldedToken.empty() || foldedText.size() < foldedToken.size())
        return false;
    const bool nonAscii = std::ranges::any_of(foldedToken, [](wchar_t c) { return c >= 128; });
    if (nonAscii)
        return foldedText.find(foldedToken) != std::wstring_view::npos;
    size_t pos = foldedText.find(foldedToken);
    while (pos != std::wstring_view::npos) {
        const bool leftOk = pos == 0 || !iswalnum(foldedText[pos - 1]);
        const size_t end = pos + foldedToken.size();
        const bool rightOk = end == foldedText.size() || !iswalnum(foldedText[end]);
        if (leftOk && rightOk)
            return true;
        pos = foldedText.find(foldedToken, pos + 1);
    }
    return false;
}

inline constexpr const wchar_t* kCatalogMakers[] = {
    L"nvidia",  L"intel", L"realtek", L"dolby",      L"lenovo",    L"dell",        L"hp inc",
    L"hewlett", L"asus",  L"acer",    L"micro-star", L"synaptics", L"waves audio", L"elan",
};

inline bool CatalogByMaker(const std::wstring& folded)
{
    return std::ranges::any_of(kCatalogMakers, [&](const wchar_t* maker) { return HasCatalogWordToken(folded, maker); });
}

inline bool CatalogNameExcluded(const std::wstring& foldedName)
{
    constexpr const wchar_t* kWords[] = {
        L"readme", L"read me", L"help", L"帮助", L"manual", L"手册", L"license", L"许可", L"website", L"官网", L"官方网站", L"release notes",
    };
    return std::ranges::any_of(kWords, [&](const wchar_t* word) { return HasCatalogWordToken(foldedName, word); });
}

inline std::wstring NormalizeCatalogPath(const std::wstring& path)
{
    std::wstring folded = FoldCatalogText(path);
    std::ranges::replace(folded, L'/', L'\\');
    return folded;
}

inline std::wstring NormalizeCatalogArgs(const std::wstring& args)
{
    const size_t first = args.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    const size_t last = args.find_last_not_of(L" \t\r\n");
    return FoldCatalogText(args.substr(first, last - first + 1));
}

inline bool IsCatalogDrivePath(const std::wstring& id) { return id.size() > 2 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/'); }

inline bool MergeStartMenuShortcuts(std::vector<CatalogApp>& apps, const std::vector<CatalogApp>& shortcuts,
                                    const std::function<bool(const CatalogApp&)>& isLaunchable)
{
    std::set<std::wstring> seenAumids;
    std::set<std::pair<std::wstring, std::wstring>> seenTargetArgs;
    std::set<std::wstring> seenIds;
    std::set<std::wstring> seenNames;
    for (const CatalogApp& app : apps) {
        if (!app.id.empty())
            seenIds.insert(NormalizeCatalogPath(app.id));
        if (!app.aumid.empty())
            seenAumids.insert(FoldCatalogText(app.aumid));
        else if (!app.id.empty() && !IsCatalogDrivePath(app.id))
            seenAumids.insert(FoldCatalogText(app.id));
        if (!app.target.empty())
            seenTargetArgs.emplace(NormalizeCatalogPath(app.target), NormalizeCatalogArgs(app.args));
        if (!app.name.empty())
            seenNames.insert(FoldCatalogText(app.name));
    }

    bool added = false;
    for (const CatalogApp& s : shortcuts) {
        if (s.name.empty() || s.id.empty() || !isLaunchable(s))
            continue;
        const std::wstring normId = NormalizeCatalogPath(s.id);
        if (seenIds.contains(normId))
            continue;
        const std::wstring foldedAumid = FoldCatalogText(s.aumid);
        if (!foldedAumid.empty() && seenAumids.contains(foldedAumid))
            continue;
        if (!s.target.empty()) {
            const std::pair key{NormalizeCatalogPath(s.target), NormalizeCatalogArgs(s.args)};
            if (seenTargetArgs.contains(key))
                continue;
            seenTargetArgs.insert(key);
        } else if (foldedAumid.empty() && seenNames.contains(FoldCatalogText(s.name))) {
            continue;
        }
        seenIds.insert(normId);
        if (!foldedAumid.empty())
            seenAumids.insert(foldedAumid);
        seenNames.insert(FoldCatalogText(s.name));
        apps.push_back(s);
        added = true;
    }
    return added;
}

} // namespace atrium
