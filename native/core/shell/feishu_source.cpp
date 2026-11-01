#include "shell/feishu_source.h"

#include "log.h"

#include <tlhelp32.h>
#include <uiautomation.h>

#include <winrt/base.h>

#include <map>
#include <set>
#include <vector>

namespace atrium {

namespace {

constexpr DWORD kPollMs = 400;

std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back()))
        s.pop_back();
    size_t a = s.find_first_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a);
}

// Feishu's processes: id -> program path.
std::map<DWORD, std::wstring> FeishuProcesses()
{
    std::map<DWORD, std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"Feishu.exe") != 0 && _wcsicmp(pe.szExeFile, L"Lark.exe") != 0)
            continue;
        std::wstring path;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t buf[MAX_PATH * 2];
            DWORD len = DWORD(std::size(buf));
            if (QueryFullProcessImageNameW(p, 0, buf, &len))
                path = buf;
            CloseHandle(p);
        }
        out[pe.th32ProcessID] = path;
    }
    CloseHandle(snap);
    return out;
}

// Feishu's popups on screen: visible, untitled Chromium windows of at most
// 520 x 260 DIP (its main window and other dialogs are larger or titled).
std::vector<std::pair<HWND, DWORD>> Popups(const std::map<DWORD, std::wstring>& procs)
{
    struct Ctx {
        const std::map<DWORD, std::wstring>* procs;
        std::vector<std::pair<HWND, DWORD>> out;
    } ctx{&procs, {}};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (!c->procs->count(pid) || !IsWindowVisible(h) || IsIconic(h) || GetWindowTextLengthW(h) > 0)
                return TRUE;
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 64);
            RECT r{};
            GetWindowRect(h, &r);
            float s = float(GetDpiForWindow(h)) / 96.f;
            if (s <= 0)
                s = 1;
            float w = float(r.right - r.left) / s, hh = float(r.bottom - r.top) / s;
            if (wcscmp(cls, L"Chrome_WidgetWin_1") == 0 && w >= 160 && w <= 520 && hh >= 40 && hh <= 260)
                c->out.push_back({h, pid});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

// The popup's texts in order, the empty ones left out.
std::vector<std::wstring> Texts(IUIAutomation* uia, IUIAutomationCondition* text, HWND h)
{
    std::vector<std::wstring> out;
    winrt::com_ptr<IUIAutomationElement> root;
    winrt::com_ptr<IUIAutomationElementArray> all;
    if (FAILED(uia->ElementFromHandle(h, root.put())) || !root || FAILED(root->FindAll(TreeScope_Descendants, text, all.put())) || !all)
        return out;
    int n = 0;
    all->get_Length(&n);
    for (int i = 0; i < n && i < 16; ++i) {
        winrt::com_ptr<IUIAutomationElement> e;
        all->GetElement(i, e.put());
        BSTR name = nullptr;
        if (e && SUCCEEDED(e->get_CurrentName(&name)) && name) {
            std::wstring t = Trim(name);
            if (!t.empty())
                out.push_back(t);
        }
        SysFreeString(name);
    }
    return out;
}

} // namespace

FeishuSource::FeishuSource(HWND notify, UINT msg) : notify_(notify), msg_(msg)
{
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
}

FeishuSource::~FeishuSource()
{
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

void FeishuSource::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomation> uia;
    winrt::com_ptr<IUIAutomationCondition> text;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put())))) {
        VARIANT v;
        v.vt = VT_I4;
        v.lVal = UIA_TextControlTypeId;
        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, text.put());
    }
    // Popups already handled, by window and text (a popup may be reused for the next message).
    std::map<HWND, std::wstring> seen;
    bool baseline = false, running = false;
    while (!stop_ && uia && text) {
        try {
            auto procs = FeishuProcesses();
            if (procs.empty() != !running) {
                running = !procs.empty();
                Log(L"feishu: %s", running ? L"watching Feishu's message popups" : L"Feishu is not running");
            }
            std::map<HWND, std::wstring> now;
            for (const auto& [h, pid] : Popups(procs)) {
                std::vector<std::wstring> t = Texts(uia.get(), text.get(), h);
                if (t.empty())
                    continue; // still loading: read again next time
                std::wstring key;
                for (const auto& s : t)
                    key += s + L"\n";
                now[h] = key;
                auto old = seen.find(h);
                if (!baseline || (old != seen.end() && old->second == key))
                    continue;
                auto* n = new Notice();
                n->source = L"feishu";
                n->app = L"飞书";
                n->appId = procs.at(pid);
                n->title = t[0];
                for (size_t i = 1; i < t.size(); ++i)
                    n->body += (n->body.empty() ? L"" : L" ") + t[i];
                if (!n->appId.empty())
                    n->action = L"app:" + n->appId;
                if (!PostMessageW(notify_, msg_, 0, reinterpret_cast<LPARAM>(n)))
                    delete n;
                else
                    Log(L"feishu: new message popup");
            }
            seen = std::move(now);
            baseline = true; // popups on screen when Atrium started are not new
        } catch (...) {
            Log(L"feishu: reading failed");
        }
        WaitForSingleObject(wake_, kPollMs);
    }
    text = nullptr;
    uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
