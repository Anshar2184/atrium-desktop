// Atrium.Guard: restores the Windows taskbar when the Atrium core exits, crashes
// or stops making progress. Native Win32 only: no GPU, no .NET, static CRT.
//
//   Atrium.Guard.exe --watch <corePid> <stateFile>   watch a core, restore on exit/hang
//   Atrium.Guard.exe --restore <stateFile>           idempotent restore from the state file
//
// Takeover protocol (v2, docs/WINDOWS-INTEGRATION.md, recovery):
//  - The core creates the control block "Local\AtriumTx-<pid>" (heartbeat, txId,
//    guardPid, revoked) and writes the state file with the same txId.
//  - The guard opens the core process, checks its start time and txId, maps the
//    control block read/write and only then publishes guardPid. The core may
//    not change anything before it sees guardPid; any guard init failure exits
//    without publishing it, so the takeover never starts.
//  - Every takeover step runs under the takeover lock and re-checks `revoked`.
//    A restore sets `revoked` first, so a stalled core that resumes cannot hide
//    the taskbar again. On a hang the guard also terminates the core before
//    restoring, so its AppBar is gone before the result is checked.
//  - `restored` is written only after the desktop has been checked (taskbar
//    visible, auto-hide, work area, core no longer owning an AppBar). Otherwise
//    the state stays `restore-pending` and the next restore tries again.
//
// Exit codes: 0 restored or nothing to do, 2 state write failed, 3 restore
// pending (retry later), 4 takeover lock unavailable (retry later),
// 11-13 guard initialization failed (no takeover allowed).
//
// ATRIUM_GUARD_FAULT injects failures for the recovery tests only:
// no-map, no-terminate, no-shell, shell-late, autohide-noop.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace {

constexpr DWORD kPollMs = 250;
constexpr DWORD kHangMs = 3000;
constexpr DWORD kLockMs = 10000;
constexpr DWORD kVerifyMs = 8000;
constexpr DWORD kCoreExitMs = 10000;
constexpr wchar_t kTakeoverLock[] = L"Local\\AtriumTaskbarRestore";

// Shared with the core (poc/Probe/RecoveryProbe.cs). 32 bytes.
struct Control {
    volatile uint64_t heartbeat;
    volatile uint64_t txId;
    volatile LONG guardPid;
    volatile LONG revoked;
    uint64_t reserved;
};

using State = std::map<std::string, std::string>;

std::string g_fault;
ULONGLONG g_restoreStart; // shell-late is timed from the start of a restore

bool Fault(const char* name) { return g_fault == name; }

State ReadState(const std::wstring& path)
{
    State s;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos)
            s[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return s;
}

// Atomic replace so a crash never leaves a half-written state file. The temp
// name is per process, so writers never share it (they are serialized anyway).
bool WriteState(const std::wstring& path, const State& s)
{
    std::wstring tmp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto& [k, v] : s)
            out << k << '=' << v << '\n';
        out.flush();
        if (!out)
            return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string Get(const State& s, const char* key)
{
    auto it = s.find(key);
    return it == s.end() ? std::string() : it->second;
}

uint64_t ToU64(const std::string& v) { return v.empty() ? 0 : std::strtoull(v.c_str(), nullptr, 10); }

uint64_t StartTimeOf(HANDLE process)
{
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(process, &c, &e, &k, &u);
    return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}

// The taskbar window, looked up fresh each time (Explorer may have restarted).
HWND Tray()
{
    if (Fault("no-shell") || (Fault("shell-late") && GetTickCount64() - g_restoreStart < 2000))
        return nullptr;
    return FindWindowW(L"Shell_TrayWnd", nullptr);
}

bool TaskbarsVisible()
{
    HWND main = Tray();
    if (!main || !IsWindowVisible(main))
        return false;
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        if (!IsWindowVisible(h))
            return false;
    return true;
}

void ShowTaskbars()
{
    if (HWND main = Tray(); main && !IsWindowVisible(main))
        ShowWindow(main, SW_SHOWNA);
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        if (!IsWindowVisible(h))
            ShowWindow(h, SW_SHOWNA);
}

bool GetAutoHide()
{
    APPBARDATA abd{sizeof(abd)};
    return (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;
}

void SetAutoHide(bool on)
{
    if (Fault("autohide-noop"))
        return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = Tray();
    abd.lParam = on ? ABS_AUTOHIDE : 0;
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

std::string WorkArea()
{
    RECT r{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
    return buf;
}

// True while the exact core recorded in the state file is still running.
bool CoreAlive(const State& s)
{
    DWORD pid = DWORD(ToU64(Get(s, "corePid")));
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!h)
        return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT && StartTimeOf(h) == ToU64(Get(s, "coreStart"));
    CloseHandle(h);
    return alive;
}

// Withdraws the core's takeover right; it re-checks this before every step.
void Revoke(const State& s)
{
    std::string pid = Get(s, "corePid");
    if (pid.empty())
        return;
    std::wstring name = L"Local\\AtriumTx-" + std::wstring(pid.begin(), pid.end());
    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    if (!map)
        return;
    if (auto* c = static_cast<Control*>(MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Control)))) {
        if (c->txId == ToU64(Get(s, "txId")))
            InterlockedExchange(&c->revoked, 1);
        UnmapViewOfFile(c);
    }
    CloseHandle(map);
}

// Holds the takeover lock for its lifetime. Abandoned counts as acquired: the
// previous owner died, and the state file is always complete (atomic replace).
class TakeoverLock {
public:
    TakeoverLock()
    {
        handle_ = CreateMutexW(nullptr, FALSE, kTakeoverLock);
        if (!handle_)
            return;
        DWORD w = WaitForSingleObject(handle_, kLockMs);
        held_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
        abandoned_ = w == WAIT_ABANDONED;
    }
    ~TakeoverLock()
    {
        if (held_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }
    TakeoverLock(const TakeoverLock&) = delete;
    TakeoverLock& operator=(const TakeoverLock&) = delete;
    bool held() const { return held_; }
    bool abandoned() const { return abandoned_; }

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
    bool abandoned_ = false;
};

double MsSince(ULONGLONG t0) { return double(GetTickCount64() - t0); }

// One pass of the restore: apply what is still off, then report what is still
// wrong (empty when the desktop is fully back).
std::string RestoreStep(const State& s, bool& appbarRemoved)
{
    bool alive = CoreAlive(s);
    if (!alive && Get(s, "appbar") == "1" && !appbarRemoved && !Get(s, "appbarHwnd").empty()) {
        // The core is gone; make sure Explorer drops its AppBar reservation.
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(ToU64(Get(s, "appbarHwnd"))));
        SHAppBarMessage(ABM_REMOVE, &abd);
        appbarRemoved = true;
    }
    if (!Tray())
        return "shell-missing";
    bool wantVisible = Get(s, "taskbarVisible") == "1";
    bool fixAutoHide = Get(s, "autoHideChanged") == "1";
    bool userAutoHide = Get(s, "userAutoHide") == "1";
    if (wantVisible)
        ShowTaskbars();
    // Auto-hide is put back only if the takeover had changed it.
    if (fixAutoHide && GetAutoHide() != userAutoHide)
        SetAutoHide(userAutoHide);
    if (alive && Get(s, "appbar") == "1")
        return "core-owns-appbar";
    if (wantVisible && !TaskbarsVisible())
        return "taskbar-hidden";
    if (fixAutoHide && GetAutoHide() != userAutoHide)
        return "autohide-mismatch";
    if (!Get(s, "workArea").empty() && WorkArea() != Get(s, "workArea"))
        return "workarea-mismatch";
    return {};
}

// Undo only what the state file says Atrium changed. Safe to run any number of
// times and from several processes at once (serialized by the takeover lock).
int Restore(const std::wstring& path, const char* reason)
{
    ULONGLONG t0 = GetTickCount64();
    // Revoke before taking the lock, then let a live core that still owns an
    // AppBar give it back. That core needs the lock to see the revocation at
    // its next step, so it must not be waited for while we hold the lock.
    {
        State early = ReadState(path);
        Revoke(early);
        while (CoreAlive(early) && Get(early, "appbar") == "1" && MsSince(t0) < kCoreExitMs)
            Sleep(50);
    }
    TakeoverLock lock;
    g_restoreStart = GetTickCount64();
    if (!lock.held()) {
        std::printf("RESTORE lock-unavailable reason=%s ms=%.0f\n", reason, MsSince(t0));
        std::fflush(stdout);
        return 4;
    }
    State s = ReadState(path);
    std::string from = Get(s, "phase");
    if (s.empty() || from == "restored" || from == "none") {
        std::printf("RESTORE nothing-to-do phase=%s reason=%s\n", s.empty() ? "missing" : from.c_str(), reason);
        std::fflush(stdout);
        return 0;
    }
    Revoke(s);
    s["phase"] = "restoring";
    s["attempts"] = std::to_string(ToU64(Get(s, "attempts")) + 1);
    if (!WriteState(path, s))
        return 2;

    // Apply and check until the desktop really is back, within a bounded time.
    std::string failed;
    int rounds = 0;
    ULONGLONG verifyStart = GetTickCount64();
    bool appbarRemoved = false;
    for (;;) {
        ++rounds;
        failed = RestoreStep(s, appbarRemoved);
        if (failed.empty() || MsSince(verifyStart) > kVerifyMs)
            break;
        Sleep(kPollMs);
    }

    int rc;
    if (failed.empty()) {
        s["phase"] = "restored";
        s["restoredBy"] = reason;
        s.erase("lastError");
        rc = WriteState(path, s) ? 0 : 2;
    } else {
        s["phase"] = "restore-pending";
        s["lastError"] = failed;
        rc = WriteState(path, s) ? 3 : 2;
    }
    std::printf("RESTORE %s from=%s reason=%s rounds=%d error=%s abandoned=%d workArea=%s ms=%.0f\n",
                failed.empty() ? "done" : "pending", from.c_str(), reason, rounds, failed.empty() ? "-" : failed.c_str(),
                lock.abandoned() ? 1 : 0, WorkArea().c_str(), MsSince(t0));
    std::fflush(stdout);
    return rc;
}

int InitFailed(int code, const char* why, DWORD pid)
{
    std::printf("WATCH init-failed pid=%lu why=%s\n", pid, why);
    std::fflush(stdout);
    return code;
}

int Watch(DWORD pid, const std::wstring& path)
{
    HANDLE core = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!core) {
        // Nothing to guard; undo whatever a dead core may have left behind.
        std::printf("WATCH cannot-open pid=%lu\n", pid);
        return Restore(path, "core-missing");
    }
    State s = ReadState(path);
    if (Get(s, "coreStart").empty() || ToU64(Get(s, "coreStart")) != StartTimeOf(core)) {
        CloseHandle(core);
        return InitFailed(11, "state-or-pid-mismatch", pid);
    }
    std::wstring name = L"Local\\AtriumTx-" + std::to_wstring(pid);
    HANDLE map = Fault("no-map") ? nullptr : OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    auto* ctl = map ? static_cast<Control*>(MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Control))) : nullptr;
    if (!ctl || ctl->txId != ToU64(Get(s, "txId"))) {
        bool mismatch = ctl != nullptr;
        if (ctl)
            UnmapViewOfFile(ctl);
        if (map)
            CloseHandle(map);
        CloseHandle(core);
        return mismatch ? InitFailed(13, "txid-mismatch", pid) : InitFailed(12, "control-block", pid);
    }
    // Keep the lock object alive while watching, so a core that dies holding it
    // leaves it abandoned (observable) instead of destroying it with its handle.
    HANDLE lockObject = CreateMutexW(nullptr, FALSE, kTakeoverLock);
    // Ready: from here on the core is allowed to take over.
    InterlockedExchange(&ctl->guardPid, LONG(GetCurrentProcessId()));
    std::printf("GUARDING pid=%lu tx=%llu\n", pid, static_cast<unsigned long long>(ctl->txId));
    std::fflush(stdout);

    uint64_t last = ctl->heartbeat;
    ULONGLONG lastChange = GetTickCount64();
    const char* reason = "core-exit";
    for (;;) {
        if (WaitForSingleObject(core, kPollMs) == WAIT_OBJECT_0)
            break;
        uint64_t now = ctl->heartbeat;
        if (now != last) {
            last = now;
            lastChange = GetTickCount64();
        } else if (GetTickCount64() - lastChange > kHangMs) {
            reason = "core-hang";
            break;
        }
    }
    if (std::string(reason) == "core-hang") {
        // Withdraw the takeover right first, then end the core so it cannot
        // resume and its AppBar is released before the restore is checked.
        InterlockedExchange(&ctl->revoked, 1);
        bool terminate = !Fault("no-terminate");
        if (terminate)
            TerminateProcess(core, 0xDEAD);
        bool gone = WaitForSingleObject(core, terminate ? kCoreExitMs : 0) == WAIT_OBJECT_0;
        std::printf("REVOKED hung core terminated=%d\n", gone ? 1 : 0);
        std::fflush(stdout);
    }
    int rc = Restore(path, reason);
    if (lockObject)
        CloseHandle(lockObject);
    UnmapViewOfFile(ctl);
    CloseHandle(map);
    CloseHandle(core);
    return rc;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Work areas are compared in physical pixels, like the core records them;
    // a DPI-unaware guard would see scaled coordinates on a high-DPI monitor.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    char fault[32] = {};
    DWORD n = GetEnvironmentVariableA("ATRIUM_GUARD_FAULT", fault, sizeof fault);
    if (n > 0 && n < sizeof fault)
        g_fault = fault;
    if (argc == 4 && std::wstring(argv[1]) == L"--watch")
        return Watch(std::wcstoul(argv[2], nullptr, 10), argv[3]);
    if (argc == 3 && std::wstring(argv[1]) == L"--restore")
        return Restore(argv[2], "manual");
    std::fprintf(stderr, "usage: Atrium.Guard --watch <corePid> <stateFile> | --restore <stateFile>\n");
    return 1;
}
