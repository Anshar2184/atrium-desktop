#include "settings.h"

#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

Settings Settings::Liquid() { return Settings{}; }

Settings Settings::Frosted()
{
    Settings s;
    s.preset = L"frosted";
    s.transparency = 0.78;
    s.blur = 1.0;
    s.refraction = 0;
    s.edge = 1;
    s.dispersion = 0;
    s.highlight = 0.30;
    s.tint = 0xF5F5FA;
    return s;
}

void Settings::Clamp()
{
    auto c = [](double& v, double lo, double hi, double fallback) {
        v = v != v ? fallback : std::clamp(v, lo, hi); // NaN -> fallback
    };
    Settings d;
    c(transparency, 0, 1, d.transparency);
    c(cornerRadius, 0, 200, d.cornerRadius);
    c(blur, 0, 1, d.blur);
    c(refraction, 0, 60, d.refraction);
    c(edge, 1, 80, d.edge);
    c(dispersion, 0, 0.5, d.dispersion);
    c(highlight, 0, 1, d.highlight);
    c(shadow, 0, 1, d.shadow);
    c(width, 200, 4000, d.width);
    c(height, 32, 160, d.height);
    c(margin, 0, 400, d.margin);
    tint &= 0xFFFFFF;
    if (preset != L"liquid" && preset != L"frosted")
        preset = L"custom";
}

std::wstring Settings::ToJson() const
{
    json::JsonObject o;
    auto num = [&](const wchar_t* k, double v) { o.SetNamedValue(k, json::JsonValue::CreateNumberValue(v)); };
    wchar_t tintText[8];
    swprintf_s(tintText, L"#%06X", tint);
    num(L"version", kVersion);
    o.SetNamedValue(L"preset", json::JsonValue::CreateStringValue(preset));
    num(L"transparency", transparency);
    num(L"cornerRadius", cornerRadius);
    num(L"blur", blur);
    num(L"refraction", refraction);
    num(L"edge", edge);
    num(L"dispersion", dispersion);
    num(L"highlight", highlight);
    num(L"shadow", shadow);
    o.SetNamedValue(L"tint", json::JsonValue::CreateStringValue(tintText));
    num(L"width", width);
    num(L"height", height);
    num(L"margin", margin);
    o.SetNamedValue(L"showClock", json::JsonValue::CreateBooleanValue(showClock));
    return std::wstring(o.Stringify());
}

bool Settings::FromJson(const std::wstring& text, const Settings& base, Settings& out)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(text, o))
        return false;
    Settings s = base;
    auto num = [&](const wchar_t* k, double& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Number)
            v = o.GetNamedNumber(k);
    };
    if (o.HasKey(L"preset") && o.GetNamedValue(L"preset").ValueType() == json::JsonValueType::String)
        s.preset = std::wstring(o.GetNamedString(L"preset"));
    num(L"transparency", s.transparency);
    num(L"cornerRadius", s.cornerRadius);
    num(L"blur", s.blur);
    num(L"refraction", s.refraction);
    num(L"edge", s.edge);
    num(L"dispersion", s.dispersion);
    num(L"highlight", s.highlight);
    num(L"shadow", s.shadow);
    num(L"width", s.width);
    num(L"height", s.height);
    num(L"margin", s.margin);
    if (o.HasKey(L"tint") && o.GetNamedValue(L"tint").ValueType() == json::JsonValueType::String) {
        std::wstring t(o.GetNamedString(L"tint"));
        if (t.size() == 7 && t[0] == L'#' && std::all_of(t.begin() + 1, t.end(), [](wchar_t c) { return iswxdigit(c) != 0; }))
            s.tint = static_cast<uint32_t>(wcstoul(t.c_str() + 1, nullptr, 16));
    }
    if (o.HasKey(L"showClock") && o.GetNamedValue(L"showClock").ValueType() == json::JsonValueType::Boolean)
        s.showClock = o.GetNamedBoolean(L"showClock");
    s.Clamp();
    out = s;
    return true;
}

std::wstring SettingsPath()
{
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
        dir = std::wstring(base) + L"\\Atrium";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.json";
}

namespace {

std::wstring ReadUtf8(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0)
        bytes.erase(0, 3); // UTF-8 BOM, e.g. saved by Notepad or Windows PowerShell
    if (bytes.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), w.data(), n);
    return w;
}

} // namespace

Settings LoadSettings()
{
    Settings s;
    std::wstring text = ReadUtf8(SettingsPath());
    if (text.empty())
        return s;
    if (!Settings::FromJson(text, Settings{}, s))
        Log(L"settings: %s is not valid JSON, using defaults", SettingsPath().c_str());
    return s;
}

bool SaveSettings(const Settings& s)
{
    std::wstring path = SettingsPath();
    std::wstring tmp = path + L".tmp";
    std::wstring text = s.ToJson();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << bytes;
        out.flush();
        if (!out)
            return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace atrium
