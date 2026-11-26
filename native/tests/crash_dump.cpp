#include "crash_dump.h"
#include "log.h"
#include "settings.h"

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <share.h>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
    return false;
}

struct TempDir {
    fs::path path;

    explicit TempDir(const wchar_t* prefix)
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path = fs::path(base) / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code ec;
        fs::create_directories(path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string ReadBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool WriteDummyDump(const fs::path& path, const FILETIME& writeTime)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const char payload[] = "DUMMY";
    DWORD written = 0;
    const BOOL okWrite = WriteFile(file, payload, 5, &written, nullptr);
    const BOOL okTime = SetFileTime(file, &writeTime, &writeTime, &writeTime);
    CloseHandle(file);
    return okWrite && okTime;
}

FILETIME FileTimeFromSeconds(uint64_t secondsOffset)
{
    constexpr uint64_t kBaseTime = 134100000000000000ULL;
    const uint64_t ticks = kBaseTime + secondsOffset * 10000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return ft;
}

std::wstring ToLower(std::wstring text)
{
    for (wchar_t& ch : text)
        ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

std::wstring MakeDumpName(unsigned index)
{
    wchar_t name[64]{};
    swprintf_s(name, L"crash-20261126-%06u-1.dmp", 100000U + index);
    return name;
}

void TestFormatCrashDumpName()
{
    SYSTEMTIME st{};
    st.wYear = 2026;
    st.wMonth = 11;
    st.wDay = 26;
    st.wHour = 10;
    st.wMinute = 30;
    st.wSecond = 12;
    Require(atrium::FormatCrashDumpName(st, 4242) == L"crash-20261126-103012-4242.dmp", "FormatCrashDumpName must format timestamp and pid");
}

void TestExceptionCodeName()
{
    Require(std::wstring(atrium::ExceptionCodeName(EXCEPTION_ACCESS_VIOLATION)) == L"ACCESS_VIOLATION", "access violation name");
    Require(std::wstring(atrium::ExceptionCodeName(EXCEPTION_STACK_OVERFLOW)) == L"STACK_OVERFLOW", "stack overflow name");
    Require(std::wstring(atrium::ExceptionCodeName(EXCEPTION_INT_DIVIDE_BY_ZERO)) == L"INT_DIVIDE_BY_ZERO", "int divide by zero name");
    Require(std::wstring(atrium::ExceptionCodeName(0x40000015UL)) == L"FATAL_APP_EXIT", "fatal app exit name");
    Require(std::wstring(atrium::ExceptionCodeName(0xE06D7363UL)) == L"CPP_EXCEPTION", "C++ exception name");
    Require(std::wstring(atrium::ExceptionCodeName(0x12345678UL)) == L"UNKNOWN", "unknown exception code fallback");
}

void TestFormatFrameAddress()
{
    void* frame = nullptr;
    const USHORT captured = CaptureStackBackTrace(0, 1, &frame, nullptr);
    Require(captured == 1 && frame != nullptr, "CaptureStackBackTrace must return a valid frame");

    wchar_t buf[256]{};
    const size_t len = atrium::FormatFrameAddress(frame, buf, std::size(buf));
    const std::wstring lower = ToLower(buf);
    Require(len > 0 && lower.find(L".exe+0x") != std::wstring::npos, "FormatFrameAddress must format executable module + RVA");

    wchar_t zeroBuf[64]{};
    atrium::FormatFrameAddress(nullptr, zeroBuf, std::size(zeroBuf));
    Require(std::wstring(zeroBuf) == L"0x0000000000000000", "FormatFrameAddress(nullptr) must fall back to hex address");

    wchar_t tinyBuf[8]{};
    const size_t tinyLen = atrium::FormatFrameAddress(frame, tinyBuf, std::size(tinyBuf));
    Require(tinyLen <= 7 && tinyBuf[7] == L'\0', "FormatFrameAddress must respect small buffer capacity");
}

void TestFormatCrashSummary()
{
    SYSTEMTIME st{};
    st.wYear = 2026;
    st.wMonth = 11;
    st.wDay = 26;
    st.wHour = 10;
    st.wMinute = 30;
    st.wSecond = 12;
    st.wMilliseconds = 345;

    void* frame = nullptr;
    CaptureStackBackTrace(0, 1, &frame, nullptr);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = frame;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 0;
    record.ExceptionInformation[1] = 0x18;

    const void* frames[2] = {frame, frame};
    atrium::CrashDumpResult okDump;
    okDump.wroteDump = true;
    okDump.bytes = 12345;
    okDump.dumpPath = L"C:\\Logs\\crash-20261126-103012-4242.dmp";

    wchar_t summary[2048]{};
    const size_t len = atrium::FormatCrashSummary(summary, std::size(summary), st, 4242, 88, L"unhandled-exception", &record, frames, 2, okDump);
    const std::wstring text(summary, len);
    Require(text.find(L"2026-11-26 10:30:12.345 [88] crash: unhandled-exception pid=4242 tid=88") != std::wstring::npos, "summary header");
    Require(text.find(L"code=0xC0000005 (ACCESS_VIOLATION, read 0x18)") != std::wstring::npos, "summary access violation details");
    Require(text.find(L"crash: handler-stack:") != std::wstring::npos && text.find(L" <- ") != std::wstring::npos, "summary stack frames");
    Require(text.find(L"crash: minidump C:\\Logs\\crash-20261126-103012-4242.dmp (12345 bytes)") != std::wstring::npos, "summary minidump line");

    atrium::CrashDumpResult failedDump;
    failedDump.wroteDump = false;
    failedDump.error = 5;
    const size_t failLen = atrium::FormatCrashSummary(summary, std::size(summary), st, 4242, 88, L"std-terminate", &record, frames, 1, failedDump);
    const std::wstring failText(summary, failLen);
    Require(failText.find(L"crash: minidump failed (err=5)") != std::wstring::npos, "summary failed minidump line");
}

void TestWriteAndPruneCrashDump()
{
    TempDir dir(L"AtriumCrashTest-");
    Require(fs::exists(dir.path), "TempDir must exist");

    for (unsigned i = 1; i <= 40; ++i)
        Require(WriteDummyDump(dir.path / MakeDumpName(i), FileTimeFromSeconds(static_cast<uint64_t>(i) * 10)), "seed dummy dump");

    Require(WriteDummyDump(dir.path / L"crash-unrelated.dmp", FileTimeFromSeconds(1)), "seed crash-unrelated.dmp");
    Require(WriteDummyDump(dir.path / L"core.log", FileTimeFromSeconds(1)), "seed unrelated core.log");
    Require(WriteDummyDump(dir.path / L"unrelated.dmp", FileTimeFromSeconds(1)), "seed unrelated .dmp");

    Require(atrium::PruneCrashDumps(dir.path.c_str(), 5) == 35, "PruneCrashDumps must remove 35 oldest of 40 crash dumps");
    Require(!fs::exists(dir.path / MakeDumpName(1)) && !fs::exists(dir.path / MakeDumpName(35)), "oldest 35 dumps must be gone");
    Require(fs::exists(dir.path / MakeDumpName(36)) && fs::exists(dir.path / MakeDumpName(40)), "newest 5 dumps must remain");

    const atrium::CrashDumpResult badSubdir = atrium::WriteCrashDump(dir.path.c_str(), L"sub\\bad.dmp", GetCurrentThreadId(), nullptr, 5);
    const atrium::CrashDumpResult badTraversal = atrium::WriteCrashDump(dir.path.c_str(), L"../bad.dmp", GetCurrentThreadId(), nullptr, 5);
    Require(!badSubdir.wroteDump && badSubdir.error == ERROR_INVALID_PARAMETER, "WriteCrashDump must reject path separators");
    Require(!badTraversal.wroteDump && badTraversal.error == ERROR_INVALID_PARAMETER, "WriteCrashDump must reject parent traversal");

    CONTEXT context{};
    RtlCaptureContext(&context);
    void* frame = nullptr;
    CaptureStackBackTrace(0, 1, &frame, nullptr);
    EXCEPTION_RECORD nested{};
    nested.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = frame;
    record.ExceptionRecord = &nested;
    EXCEPTION_POINTERS exceptionPointers{&record, &context};

    const std::wstring newName = MakeDumpName(41);
    const atrium::CrashDumpResult result = atrium::WriteCrashDump(dir.path.c_str(), newName.c_str(), GetCurrentThreadId(), &exceptionPointers, 5);
    Require(result.wroteDump && result.error == 0 && result.bytes >= 4, "WriteCrashDump must write a non-empty minidump");

    const atrium::CrashDumpResult clobber = atrium::WriteCrashDump(dir.path.c_str(), newName.c_str(), GetCurrentThreadId(), &exceptionPointers, 5);
    Require(!clobber.wroteDump && clobber.error == ERROR_FILE_EXISTS, "WriteCrashDump must refuse to overwrite existing dump");

    const fs::path dumpPath = dir.path / newName;
    const std::string bytes = ReadBytes(dumpPath);
    Require(bytes.starts_with("MDMP"), "minidump must start with MDMP header");
    Require(!fs::exists(dir.path / MakeDumpName(36)) && fs::exists(dir.path / MakeDumpName(37)) && fs::exists(dumpPath),
            "newest 5 crash dumps must remain after new dump");

    const std::wstring nameA = MakeDumpName(42);
    const std::wstring nameB = MakeDumpName(43);
    atrium::CrashDumpResult resA;
    atrium::CrashDumpResult resB;
    std::thread t1([&dir, &nameA, &resA] { resA = atrium::WriteCrashDump(dir.path.c_str(), nameA.c_str(), GetCurrentThreadId(), nullptr, 5); });
    std::thread t2([&dir, &nameB, &resB] { resB = atrium::WriteCrashDump(dir.path.c_str(), nameB.c_str(), GetCurrentThreadId(), nullptr, 5); });
    t1.join();
    t2.join();
    Require((resA.wroteDump || resB.wroteDump) && (resA.wroteDump || resA.error == ERROR_BUSY) && (resB.wroteDump || resB.error == ERROR_BUSY),
            "concurrent WriteCrashDump must serialize cleanly");

    const std::wstring seqName = MakeDumpName(44);
    const atrium::CrashDumpResult seqRes = atrium::WriteCrashDump(dir.path.c_str(), seqName.c_str(), GetCurrentThreadId(), nullptr, 5);
    Require(seqRes.wroteDump && seqRes.error == 0, "WriteCrashDump slot must recover cleanly after concurrent calls");

    Require(atrium::PruneCrashDumps(dir.path.c_str(), 0) >= 1, "PruneCrashDumps(0) must remove all valid crash dumps");
    Require(!fs::exists(dir.path / seqName) && fs::exists(dir.path / L"crash-unrelated.dmp") && fs::exists(dir.path / L"core.log") &&
                fs::exists(dir.path / L"unrelated.dmp"),
            "unrelated files must remain untouched");
}

void TestInstallAndShutdownCrashHandlers()
{
    atrium::InstallCrashHandlers(nullptr);
    atrium::ShutdownCrashHandlers();
    std::thread t1([] {
        for (int i = 0; i < 4; ++i)
            atrium::InstallCrashHandlers(nullptr);
    });
    std::thread t2([] {
        for (int i = 0; i < 4; ++i)
            atrium::ShutdownCrashHandlers();
    });
    t1.join();
    t2.join();
    atrium::ShutdownCrashHandlers();
}

void TestAppendEmergencyLogWhileOpen()
{
    TempDir dir(L"AtriumLogAppend-");
    const fs::path logPath = dir.path / L"core.log";

    FILE* writer = _wfsopen(logPath.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    Require(writer != nullptr, "open simulated core.log writer");
    if (!writer)
        return;
    fputws(L"2026-11-26 10:30:00.000 [1] core: starting\n", writer);
    fflush(writer);

    const std::wstring longLine = L"2026-11-26 10:30:12.000 [1] crash: \u5d29\u6e83\u6d4b\u8bd5 " + std::wstring(1200, L'x') + L" tail\n";
    const bool appended = atrium::AppendEmergencyLogLine(logPath.c_str(), longLine.c_str());
    fclose(writer);
    Require(appended, "AppendEmergencyLogLine must succeed across chunks while Writer holds _SH_DENYNO handle");

    const std::string bytes = ReadBytes(logPath);
    Require(bytes.find("core: starting") != std::string::npos, "initial log line preserved");
    Require(bytes.find("crash: \xE5\xB4\xA9\xE6\xBA\x83\xE6\xB5\x8B\xE8\xAF\x95 ") != std::string::npos && bytes.find(" tail\n") != std::string::npos,
            "multi-chunk emergency UTF-8 line appended cleanly");
}

void TestFlushLog()
{
    Require(atrium::FlushLog(100), "FlushLog before first Log() must succeed with nothing pending");
    const std::wstring marker = L"crash_dump_test_flush_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring workerMarker = L"crash_dump_test_worker_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring emergencyMarker = L"crash_dump_test_emergency_" + std::to_wstring(GetCurrentProcessId());
    atrium::Log(L"%s", marker.c_str());

    const fs::path logsDir = fs::path(atrium::DataDirectory()) / L"logs";
    std::error_code ec;
    fs::create_directories(logsDir, ec);
    const fs::path coreLogPath = logsDir / L"core.log";
    std::thread logger([&workerMarker] {
        for (int i = 0; i < 10; ++i)
            atrium::Log(L"%s_%d", workerMarker.c_str(), i);
    });
    const std::wstring emergencyLine = L"2026-11-26 10:30:12.000 [1] crash: " + emergencyMarker + L"\n";
    Require(atrium::AppendEmergencyLogLine(coreLogPath.c_str(), emergencyLine.c_str()), "concurrent emergency append must succeed");
    logger.join();
    Require(atrium::FlushLog(2000), "FlushLog must flush queued log entries within timeout");

    const std::string narrowMarker = "crash_dump_test_flush_" + std::to_string(GetCurrentProcessId());
    const std::string narrowWorkerMarker = "crash_dump_test_worker_" + std::to_string(GetCurrentProcessId()) + "_9";
    const std::string narrowEmergency = "crash_dump_test_emergency_" + std::to_string(GetCurrentProcessId());
    const std::string logBytes = ReadBytes(coreLogPath);
    Require(logBytes.find(narrowMarker) != std::string::npos && logBytes.find(narrowWorkerMarker) != std::string::npos &&
                logBytes.find(narrowEmergency) != std::string::npos,
            "flushed and emergency log markers must be visible on disk");

    atrium::ShutdownLog();
    Require(!atrium::FlushLog(100), "FlushLog after ShutdownLog must return false");
}

} // namespace

int main()
{
    TestFormatCrashDumpName();
    TestExceptionCodeName();
    TestFormatFrameAddress();
    TestFormatCrashSummary();
    TestWriteAndPruneCrashDump();
    TestInstallAndShutdownCrashHandlers();
    TestAppendEmergencyLogWhileOpen();
    TestFlushLog();
    if (g_failures > 0)
        return 1;
    std::printf("crash dump: all checks passed\n");
    return 0;
}
