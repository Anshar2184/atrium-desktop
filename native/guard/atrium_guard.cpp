// Atrium.Guard: restores the Windows taskbar when the Atrium core exits, crashes
// or stops making progress. Native Win32 only: no GPU, no .NET, static CRT.
//
//   Atrium.Guard.exe --watch <corePid> <stateFile>   watch a core, restore on exit/hang
//   Atrium.Guard.exe --restore <stateFile>           idempotent restore from the state file
//
// The state file is written by the core before every takeover step, so the guard
// knows exactly what to undo (see docs/WINDOWS-INTEGRATION.md, recovery).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {

constexpr DWORD kPollMs = 250;
constexpr DWORD kHangMs = 3000;
constexpr wchar_t kRestoreMutex[] = L"Local\\AtriumTaskbarRestore";

using State = std::map<std::string, std::string>;

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

// Atomic replace so a crash never leaves a half-written state file.
bool WriteState(const std::wstring& path, const State& s)
{
    std::wstring tmp = path + L".tmp";
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

void ShowTaskbars()
{
    if (HWND main = FindWindowW(L"Shell_TrayWnd", nullptr); main && !IsWindowVisible(main))
        ShowWindow(main, SW_SHOWNA);
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        if (!IsWindowVisible(h))
            ShowWindow(h, SW_SHOWNA);
}

void SetAutoHide(bool on)
{
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    abd.lParam = on ? ABS_AUTOHIDE : 0;
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

// Undo only what the state file says Atrium changed. Safe to run any number of
// times and from several processes at once (serialized by a named mutex).
int Restore(const std::wstring& path, const char* reason)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kRestoreMutex);
    if (mutex)
        WaitForSingleObject(mutex, 10000);
    State s = ReadState(path);
    int rc = 0;
    if (s.empty() || s["phase"] == "restored" || s["phase"] == "none") {
        std::printf("RESTORE nothing-to-do phase=%s reason=%s\n", s.empty() ? "missing" : s["phase"].c_str(), reason);
    } else {
        std::string from = s["phase"];
        s["phase"] = "restoring";
        WriteState(path, s);
        if (s["taskbarVisible"] == "1")
            ShowTaskbars();
        // Auto-hide is put back only if the takeover had changed it.
        if (s.count("userAutoHide") && s["autoHideChanged"] == "1")
            SetAutoHide(s["userAutoHide"] == "1");
        s["phase"] = "restored";
        s["restoredBy"] = reason;
        rc = WriteState(path, s) ? 0 : 2;
        QueryPerformanceCounter(&t1);
        std::printf("RESTORE done from=%s reason=%s ms=%.1f\n", from.c_str(), reason,
                    (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    }
    std::fflush(stdout);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return rc;
}

uint64_t FileTimeOf(HANDLE process)
{
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(process, &c, &e, &k, &u);
    return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}

int Watch(DWORD pid, const std::wstring& path)
{
    HANDLE core = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!core) {
        std::printf("WATCH cannot-open pid=%lu\n", pid);
        return Restore(path, "core-missing");
    }
    // Guard against PID reuse: the state file records the core's start time.
    State s = ReadState(path);
    if (s.count("coreStart") && std::stoull(s["coreStart"]) != FileTimeOf(core)) {
        std::printf("WATCH pid-reused pid=%lu\n", pid);
        CloseHandle(core);
        return Restore(path, "pid-reused");
    }
    std::wstring name = L"Local\\AtriumHeartbeat-" + std::to_wstring(pid);
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    auto* beat = map ? static_cast<volatile uint64_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(uint64_t))) : nullptr;
    std::printf("GUARDING pid=%lu heartbeat=%s\n", pid, beat ? "yes" : "no");
    std::fflush(stdout);

    uint64_t last = beat ? *beat : 0;
    ULONGLONG lastChange = GetTickCount64();
    const char* reason = "core-exit";
    for (;;) {
        if (WaitForSingleObject(core, kPollMs) == WAIT_OBJECT_0)
            break;
        if (!beat)
            continue;
        uint64_t now = *beat;
        if (now != last) {
            last = now;
            lastChange = GetTickCount64();
        } else if (GetTickCount64() - lastChange > kHangMs) {
            reason = "core-hang";
            break;
        }
    }
    // Restore first, then terminate a hung core so its AppBar is released.
    int rc = Restore(path, reason);
    if (std::string(reason) == "core-hang") {
        TerminateProcess(core, 0xDEAD);
        WaitForSingleObject(core, 5000);
        std::printf("TERMINATED hung core\n");
        std::fflush(stdout);
    }
    if (beat)
        UnmapViewOfFile(const_cast<uint64_t*>(beat));
    if (map)
        CloseHandle(map);
    CloseHandle(core);
    return rc;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 4 && std::wstring(argv[1]) == L"--watch")
        return Watch(std::wcstoul(argv[2], nullptr, 10), argv[3]);
    if (argc == 3 && std::wstring(argv[1]) == L"--restore")
        return Restore(argv[2], "manual");
    std::fprintf(stderr, "usage: Atrium.Guard --watch <corePid> <stateFile> | --restore <stateFile>\n");
    return 1;
}
