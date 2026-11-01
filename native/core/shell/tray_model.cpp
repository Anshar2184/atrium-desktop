#include "shell/tray_model.h"

#include "log.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <map>
#include <set>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shlwapi.lib")

namespace atrium {

namespace {

constexpr int kIconPx = 64;

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), s.data(), int(s.size()), nullptr, nullptr, 0);
    return s;
}

// Lower case without spaces and the punctuation tooltips decorate names with.
std::wstring Key(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : Lower(s))
        if (!iswspace(c) && !wcschr(L"-_:：·|()（）[]【】.,，", c))
            out += c;
    return out;
}

std::wstring VersionString(const std::wstring& path, const wchar_t* field)
{
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Lang {
        WORD lang, codepage;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang))
        return {};
    wchar_t sub[96];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].codepage, field);
    wchar_t* value = nullptr;
    if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &len) && value && len)
        return value;
    return {};
}

// "{6D809377-...}\Tailscale\tailscale-ipn.exe": a known folder, then the rest.
std::wstring ResolveKnownFolder(const std::wstring& path)
{
    if (path.size() < 39 || path[0] != L'{')
        return path;
    GUID id{};
    if (FAILED(CLSIDFromString(path.substr(0, 38).c_str(), &id)))
        return path;
    PWSTR base = nullptr;
    std::wstring out = path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &base)))
        out = std::wstring(base) + path.substr(38);
    CoTaskMemFree(base);
    return out;
}

bool DecodePng(const std::vector<BYTE>& bytes, IconImage& out)
{
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> conv;
    winrt::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(bytes.data(), UINT(bytes.size())));
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512)
        return false;
    out.width = int(w);
    out.height = int(h);
    out.bgra.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data()));
}

// Windows' record of the icons that have been in the tray.
struct Record {
    std::wstring exe;     // full path, lower case
    std::wstring tip;     // tooltip when first seen; often empty
    std::vector<BYTE> png; // icon snapshot
};

std::vector<Record> ReadRecords()
{
    std::vector<Record> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[128];
        DWORD len = 128;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        Record r;
        wchar_t buf[1024];
        DWORD size = sizeof(buf);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.exe = Lower(ResolveKnownFolder(buf));
        size = sizeof(buf);
        if (RegGetValueW(root, name, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.tip = buf;
        size = 0;
        if (RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS && size && size < (1u << 20)) {
            r.png.resize(size);
            RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, r.png.data(), &size);
        }
        if (!r.exe.empty())
            out.push_back(std::move(r));
    }
    RegCloseKey(root);
    return out;
}

// Running programs: path (lower case) -> titles of their visible windows.
std::map<std::wstring, std::vector<std::wstring>> RunningPrograms()
{
    std::map<DWORD, std::wstring> paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t buf[MAX_PATH * 2];
                DWORD len = DWORD(std::size(buf));
                if (QueryFullProcessImageNameW(p, 0, buf, &len))
                    paths[pe.th32ProcessID] = Lower(buf);
                CloseHandle(p);
            }
        }
        CloseHandle(snap);
    }
    std::map<std::wstring, std::vector<std::wstring>> out;
    for (const auto& [pid, path] : paths)
        out[path];
    struct Ctx {
        std::map<DWORD, std::wstring>* paths;
        std::map<std::wstring, std::vector<std::wstring>>* out;
    } ctx{&paths, &out};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            wchar_t title[256];
            if (!IsWindowVisible(h) || !GetWindowTextW(h, title, 256))
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (auto it = c->paths->find(pid); it != c->paths->end())
                (*c->out)[it->second].push_back(title);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return out;
}

// How well a tooltip names a program: 0 none .. 100 exactly.
int Score(const std::wstring& tipKey, const std::wstring& candidate)
{
    std::wstring c = Key(candidate);
    if (c.size() < 2 || tipKey.empty())
        return 0;
    if (c == tipKey)
        return 100;
    if (tipKey.rfind(c, 0) == 0 || c.rfind(tipKey, 0) == 0)
        return 80;
    if (tipKey.find(c) != std::wstring::npos || c.find(tipKey) != std::wstring::npos)
        return 60;
    return 0;
}

struct Found {
    winrt::com_ptr<IUIAutomationElement> element;
    std::wstring tip;
    bool promoted;
    int index;
};

// The tray buttons in the taskbar and in the overflow window, in order.
std::vector<Found> FindButtons(IUIAutomation* uia, bool& available)
{
    std::vector<Found> out;
    available = false;
    auto collect = [&](HWND host, bool promoted) {
        HWND bridge = host ? FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
        winrt::com_ptr<IUIAutomationElement> root;
        if (!bridge || FAILED(uia->ElementFromHandle(bridge, root.put())) || !root)
            return;
        available = true;
        winrt::com_ptr<IUIAutomationCondition> cond;
        VARIANT v;
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"NotifyItemIcon");
        HRESULT hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, cond.put());
        VariantClear(&v);
        winrt::com_ptr<IUIAutomationElementArray> all;
        if (FAILED(hr) || FAILED(root->FindAll(TreeScope_Descendants, cond.get(), all.put())) || !all)
            return;
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            Found f;
            all->GetElement(i, f.element.put());
            BSTR name = nullptr;
            if (f.element)
                f.element->get_CurrentName(&name);
            f.tip = name ? name : L"";
            SysFreeString(name);
            f.promoted = promoted;
            f.index = int(std::count_if(out.begin(), out.end(), [&](const Found& o) { return o.promoted == promoted; }));
            out.push_back(std::move(f));
        }
    };
    collect(FindWindowW(L"Shell_TrayWnd", nullptr), true);
    collect(FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr), false);
    return out;
}

} // namespace

std::wstring TrayModel::Title(const std::wstring& tip)
{
    size_t a = tip.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos)
        return {};
    size_t b = tip.find_first_of(L"\r\n", a);
    std::wstring line = tip.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
    while (!line.empty() && iswspace(line.back()))
        line.pop_back();
    return line;
}

TrayModel::TrayModel(HWND notify, UINT msg) : notify_(notify), msg_(msg)
{
    thread_ = std::thread([this] { Worker(); });
}

TrayModel::~TrayModel()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void TrayModel::Refresh()
{
    std::lock_guard g(mutex_);
    if (std::none_of(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.refresh; }))
        jobs_.push_back({true, {}, false});
    cv_.notify_all();
}

void TrayModel::Click(const TrayIcon& icon, bool right)
{
    std::lock_guard g(mutex_);
    jobs_.push_back({false, icon, right});
    cv_.notify_all();
}

std::vector<TrayIcon> TrayModel::Icons() const
{
    std::lock_guard g(mutex_);
    return icons_;
}

bool TrayModel::Loaded() const
{
    std::lock_guard g(mutex_);
    return loaded_;
}

bool TrayModel::Available() const
{
    std::lock_guard g(mutex_);
    return available_;
}

void TrayModel::Worker()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()))))
        Log(L"tray: UI Automation unavailable");
    std::map<std::wstring, std::shared_ptr<const IconImage>> pictures; // exe or snapshot -> picture
    for (;;) {
        Job job;
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || !jobs_.empty(); });
            if (stop_)
                break;
            job = jobs_.front();
            jobs_.pop_front();
        }
        if (!uia)
            continue;
        try {
            bool available = false;
            std::vector<Found> buttons = FindButtons(uia.get(), available);
            if (!job.refresh) {
                // The same place and position with the same tooltip; else the
                // same tooltip anywhere (the list may have changed meanwhile).
                auto it = std::find_if(buttons.begin(), buttons.end(), [&](const Found& f) {
                    return f.promoted == job.icon.promoted && f.index == job.icon.index && f.tip == job.icon.tip;
                });
                if (it == buttons.end())
                    it = std::find_if(buttons.begin(), buttons.end(), [&](const Found& f) { return f.tip == job.icon.tip; });
                HRESULT hr = E_FAIL;
                if (it != buttons.end() && it->element) {
                    if (job.right) {
                        winrt::com_ptr<IUIAutomationElement3> e3 = it->element.try_as<IUIAutomationElement3>();
                        hr = e3 ? e3->ShowContextMenu() : E_NOINTERFACE;
                    } else {
                        winrt::com_ptr<IUIAutomationInvokePattern> invoke;
                        hr = it->element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()));
                        if (SUCCEEDED(hr) && invoke)
                            hr = invoke->Invoke();
                    }
                }
                Log(L"tray: %s on '%s' -> 0x%08X", job.right ? L"right click" : L"click", Title(job.icon.tip).c_str(), unsigned(hr));
                PostMessageW(notify_, msg_, 1, 0);
                continue;
            }

            // Pictures: match each button to a running program Windows has a
            // tray record of, best names first, each used once.
            std::vector<Record> records = ReadRecords();
            auto running = RunningPrograms();
            struct Candidate {
                size_t record;
                std::vector<std::wstring> names;
            };
            std::vector<Candidate> candidates;
            std::map<std::wstring, std::pair<std::wstring, std::wstring>> version; // exe -> description, product
            for (size_t r = 0; r < records.size(); ++r) {
                auto run = running.find(records[r].exe);
                if (run == running.end())
                    continue;
                auto& v = version[records[r].exe];
                if (v.first.empty() && v.second.empty())
                    v = {VersionString(records[r].exe, L"FileDescription"), VersionString(records[r].exe, L"ProductName")};
                std::wstring stem = PathFindFileNameW(records[r].exe.c_str());
                PathRemoveExtensionW(stem.data());
                stem.resize(wcslen(stem.c_str()));
                Candidate c{r, {records[r].tip, v.first, v.second, stem}};
                c.names.insert(c.names.end(), run->second.begin(), run->second.end());
                candidates.push_back(std::move(c));
            }
            struct Pair {
                int score;
                size_t button, candidate;
            };
            std::vector<Pair> pairs;
            for (size_t b = 0; b < buttons.size(); ++b) {
                std::wstring key = Key(Title(buttons[b].tip));
                for (size_t c = 0; c < candidates.size(); ++c) {
                    int best = 0;
                    for (size_t k = 0; k < candidates[c].names.size(); ++k) // the recorded tooltip counts most
                        best = std::max(best, Score(key, candidates[c].names[k]) + (k == 0 ? 5 : 0));
                    if (best > 0)
                        pairs.push_back({best + (records[candidates[c].record].png.empty() ? 0 : 1), b, c});
                }
            }
            std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.score > b.score; });
            std::vector<int> match(buttons.size(), -1);
            std::set<std::wstring> usedExe; // one button per program record set
            std::set<size_t> usedCandidate;
            for (const Pair& p : pairs)
                if (match[p.button] < 0 && !usedCandidate.count(p.candidate) && !usedExe.count(records[candidates[p.candidate].record].exe)) {
                    match[p.button] = int(candidates[p.candidate].record);
                    usedCandidate.insert(p.candidate);
                    usedExe.insert(records[candidates[p.candidate].record].exe);
                }

            std::vector<TrayIcon> icons;
            int recognised = 0;
            for (size_t b = 0; b < buttons.size(); ++b) {
                TrayIcon t;
                t.tip = buttons[b].tip;
                t.promoted = buttons[b].promoted;
                t.index = buttons[b].index;
                if (match[b] >= 0) {
                    const Record& r = records[size_t(match[b])];
                    t.exe = r.exe;
                    ++recognised;
                    std::wstring key = r.png.empty() ? r.exe : r.exe + L"|" + std::to_wstring(r.png.size());
                    auto& pic = pictures[key];
                    if (!pic) {
                        IconImage img;
                        if ((r.png.empty() || !DecodePng(r.png, img)) && !LoadShellImage(r.exe, kIconPx, img))
                            img = {};
                        if (!img.empty())
                            pic = std::make_shared<const IconImage>(std::move(img));
                    }
                    t.image = pic;
                }
                icons.push_back(std::move(t));
            }
            bool changed;
            {
                std::lock_guard g(mutex_);
                changed = !loaded_ || icons.size() != icons_.size() ||
                          !std::equal(icons.begin(), icons.end(), icons_.begin(), [](const TrayIcon& a, const TrayIcon& b) {
                              return a.tip == b.tip && a.exe == b.exe && a.image == b.image && a.promoted == b.promoted;
                          });
                icons_ = std::move(icons);
                loaded_ = true;
                available_ = available;
            }
            if (changed) {
                Log(L"tray: %zu icons (%d recognised)%s", buttons.size(), recognised, available ? L"" : L", the shell's tray was not found");
                PostMessageW(notify_, msg_, 0, 0);
            }
        } catch (...) {
            Log(L"tray: reading the tray failed");
        }
    }
    uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
