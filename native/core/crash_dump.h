// Unhandled crash capture: writes a minidump, appends a stack summary to core.log, and restores Windows.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace atrium {

struct CrashDumpResult {
    bool wroteDump = false;
    bool fellBackToMinimal = false;
    DWORD error = 0;
    uint64_t bytes = 0;
    std::wstring dumpPath;
};

struct CrashDumpSummaryInfo {
    bool wroteDump = false;
    bool fellBackToMinimal = false;
    DWORD error = 0;
    uint64_t bytes = 0;
    const wchar_t* dumpPath = nullptr;
};

using EmergencyRestoreFn = void (*)();

// PruneCrashDumps retains at most kMaxRetainedCrashDumps files.
inline constexpr unsigned kMaxRetainedCrashDumps = 32;

void InstallCrashHandlers(EmergencyRestoreFn emergencyRestore);
void ShutdownCrashHandlers();
const wchar_t* ExceptionCodeName(DWORD code);
std::wstring FormatCrashDumpName(const SYSTEMTIME& localTime, DWORD pid);
size_t FormatFrameAddress(const void* address, wchar_t* buffer, size_t capacity);
size_t FormatCrashSummaryInfo(wchar_t* buffer, size_t capacity, const SYSTEMTIME& localTime, DWORD pid, DWORD threadId, const wchar_t* reason,
                              const EXCEPTION_RECORD* record, const void* const* frames, size_t frameCount, const CrashDumpSummaryInfo& dump);
size_t FormatCrashSummary(wchar_t* buffer, size_t capacity, const SYSTEMTIME& localTime, DWORD pid, DWORD threadId, const wchar_t* reason,
                          const EXCEPTION_RECORD* record, const void* const* frames, size_t frameCount, const CrashDumpResult& dump);
unsigned PruneCrashDumps(const wchar_t* logDir, unsigned maxRetained);
CrashDumpResult WriteCrashDump(const wchar_t* logDir, const wchar_t* dumpName, DWORD threadId, EXCEPTION_POINTERS* exceptionPointers, unsigned maxRetained = 5);
bool AppendEmergencyLogLine(const wchar_t* logFilePath, const wchar_t* text);

} // namespace atrium
