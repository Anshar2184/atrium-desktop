#include "crash_dump.h"

#include "log.h"
#include "settings.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <iterator>
#include <mutex>

namespace atrium {

namespace {

constexpr size_t kMaxPathChars = MAX_PATH * 2;
constexpr unsigned kMaxCrashDumps = 5;
constexpr size_t kMaxStackFrames = 20;
constexpr SIZE_T kDumpThreadStackBytes = 256U * 1024U;
constexpr DWORD kCrashWorkerTimeoutMs = 5000;
constexpr DWORD kWorkerJoinTimeoutMs = 2000;
constexpr unsigned long kLogFlushTimeoutMs = 500;
constexpr DWORD kFatalAppExitCode = 0x40000015UL;
constexpr DWORD kInvalidParameterCode = 0xC000000DUL;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

enum class DumpSlotState : int { Idle = 0, Running = 1, Finished = 2, Abandoned = 3 };
enum class HandlerLifecycle : int { Stopped = 0, Running = 1, Stopping = 2, Poisoned = 3 };

struct CrashDumpOutcome {
    bool wroteDump = false;
    bool fellBackToMinimal = false;
    DWORD error = 0;
    uint64_t bytes = 0;
    wchar_t dumpPath[kMaxPathChars]{};
};

struct ExceptionSnapshot {
    bool hasRecord = false;
    bool hasContext = false;
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS ptrs{};
};

struct CrashRequest {
    DWORD pid = 0;
    DWORD threadId = 0;
    SYSTEMTIME localTime{};
    const wchar_t* reason = L"unknown";
    bool stackOverflow = false;
    ExceptionSnapshot exception{};
    void* frames[kMaxStackFrames]{};
    USHORT frameCount = 0;
};

struct StandaloneDumpState {
    std::atomic<DumpSlotState> state{DumpSlotState::Idle};
    DWORD threadId = 0;
    unsigned maxRetained = kMaxCrashDumps;
    ExceptionSnapshot exception{};
    wchar_t logDir[kMaxPathChars]{};
    wchar_t dumpPath[kMaxPathChars]{};
    CrashDumpOutcome outcome{};
};

// Static buffers avoid heap allocation inside the crash path and keep worker-thread state valid across timeouts.
std::mutex g_lifecycleMutex;
std::atomic<HandlerLifecycle> g_handlerLifecycle{HandlerLifecycle::Stopped};
std::atomic<uint32_t> g_activeHandlers{0};
std::atomic<bool> g_dbghelpInUse{false};
std::atomic<EmergencyRestoreFn> g_emergencyRestore{nullptr};
wchar_t g_logDir[kMaxPathChars]{};
wchar_t g_coreLogPath[kMaxPathChars]{};
HMODULE g_dbghelp = nullptr;
MiniDumpWriteDumpFn g_writeDump = nullptr;
HANDLE g_crashRequestEvent = nullptr;
HANDLE g_crashDoneEvent = nullptr;
HANDLE g_crashWorkerThread = nullptr;
std::atomic<DWORD> g_crashWorkerThreadId{0};
std::atomic<bool> g_crashWorkerStop{false};
std::atomic<bool> g_inCrashHandler{false};
LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandledFilter = nullptr;
std::terminate_handler g_prevTerminateHandler = nullptr;
_invalid_parameter_handler g_prevInvalidParamHandler = nullptr;
_purecall_handler g_prevPurecallHandler = nullptr;
CrashRequest g_crashRequest{};
wchar_t g_crashSummary[4096]{};
StandaloneDumpState g_standaloneDump{};

struct ScopedAtomicFlag {
    std::atomic<bool>& flag;

    explicit ScopedAtomicFlag(std::atomic<bool>& target) : flag(target) {}
    ~ScopedAtomicFlag() { flag.store(false, std::memory_order_release); }

    ScopedAtomicFlag(const ScopedAtomicFlag&) = delete;
    ScopedAtomicFlag& operator=(const ScopedAtomicFlag&) = delete;
};

struct ScopedActiveHandler {
    ScopedActiveHandler() { g_activeHandlers.fetch_add(1, std::memory_order_seq_cst); }
    ~ScopedActiveHandler() { g_activeHandlers.fetch_sub(1, std::memory_order_seq_cst); }

    ScopedActiveHandler(const ScopedActiveHandler&) = delete;
    ScopedActiveHandler& operator=(const ScopedActiveHandler&) = delete;
};

bool CopyBounded(wchar_t* dst, size_t capacity, const wchar_t* src)
{
    if (!dst || capacity == 0 || !src)
        return false;
    const size_t len = wcslen(src);
    if (len >= capacity)
        return false;
    wmemcpy(dst, src, len + 1);
    return true;
}

bool JoinPath(wchar_t* out, size_t capacity, const wchar_t* dir, const wchar_t* name)
{
    if (!out || capacity == 0)
        return false;
    out[0] = L'\0';
    if (!dir || !*dir || !name || !*name)
        return false;
    const size_t dirLen = wcslen(dir);
    const size_t nameLen = wcslen(name);
    const bool hasSep = dir[dirLen - 1] == L'\\' || dir[dirLen - 1] == L'/';
    const size_t sepLen = hasSep ? 0 : 1;
    if (dirLen + sepLen + nameLen >= capacity)
        return false;
    wmemcpy(out, dir, dirLen);
    if (sepLen)
        out[dirLen] = L'\\';
    wmemcpy(out + dirLen + sepLen, name, nameLen + 1);
    return true;
}

bool IsSafeFileName(const wchar_t* name)
{
    if (!name || !*name)
        return false;
    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
        return false;
    for (const wchar_t* p = name; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':')
            return false;
    }
    return true;
}

void CloseAndClear(HANDLE& handle)
{
    if (handle) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void CopyExceptionSnapshot(EXCEPTION_POINTERS* src, ExceptionSnapshot& dst)
{
    dst.hasRecord = src && src->ExceptionRecord;
    if (dst.hasRecord) {
        dst.record = *src->ExceptionRecord;
        // Clear nested chain pointer so the snapshot does not retain pointers into caller stack memory.
        dst.record.ExceptionRecord = nullptr;
    }
    dst.hasContext = src && src->ContextRecord;
    if (dst.hasContext)
        dst.context = *src->ContextRecord;
    dst.ptrs.ExceptionRecord = dst.hasRecord ? &dst.record : nullptr;
    dst.ptrs.ContextRecord = dst.hasContext ? &dst.context : nullptr;
}

EXCEPTION_POINTERS* SnapshotPointers(ExceptionSnapshot& snapshot) { return (snapshot.hasRecord || snapshot.hasContext) ? &snapshot.ptrs : nullptr; }

void FormatCrashDumpNameBuffer(wchar_t* buffer, size_t capacity, const SYSTEMTIME& localTime, DWORD pid)
{
    if (!buffer || capacity == 0)
        return;
    _snwprintf_s(buffer, capacity, _TRUNCATE, L"crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", localTime.wYear, localTime.wMonth, localTime.wDay, localTime.wHour,
                 localTime.wMinute, localTime.wSecond, pid);
}

void ExecuteMiniDump(MiniDumpWriteDumpFn writeDump, HANDLE file, DWORD threadId, EXCEPTION_POINTERS* exceptionPointers, bool& wrote, bool& minimal,
                     DWORD& error)
{
    wrote = false;
    minimal = false;
    error = 0;
    if (!writeDump || file == INVALID_HANDLE_VALUE) {
        error = ERROR_INVALID_HANDLE;
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{threadId, exceptionPointers, FALSE};
    PMINIDUMP_EXCEPTION_INFORMATION meiPtr = exceptionPointers ? &mei : nullptr;
    const auto richType =
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    if (writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, richType, meiPtr, nullptr, nullptr)) {
        wrote = true;
        return;
    }
    const DWORD richError = GetLastError();
    LARGE_INTEGER zero{};
    SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
    SetEndOfFile(file);
    if (writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, meiPtr, nullptr, nullptr)) {
        wrote = true;
        minimal = true;
        return;
    }
    error = GetLastError();
    if (error == 0)
        error = richError;
}

CrashDumpOutcome WriteCrashDumpWithFn(MiniDumpWriteDumpFn writeDump, const wchar_t* logDir, const wchar_t* dumpPath, DWORD threadId,
                                      EXCEPTION_POINTERS* exceptionPointers, unsigned maxRetained, bool pruneOnSuccess)
{
    CrashDumpOutcome outcome;
    CopyBounded(outcome.dumpPath, std::size(outcome.dumpPath), dumpPath);
    if (!writeDump) {
        outcome.error = ERROR_MOD_NOT_FOUND;
        return outcome;
    }
    if (g_dbghelpInUse.exchange(true, std::memory_order_acq_rel)) {
        outcome.error = ERROR_BUSY;
        return outcome;
    }
    ScopedAtomicFlag dbghelpLock(g_dbghelpInUse);
    HANDLE file = CreateFileW(dumpPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        outcome.error = GetLastError();
        return outcome;
    }
    ExecuteMiniDump(writeDump, file, threadId, exceptionPointers, outcome.wroteDump, outcome.fellBackToMinimal, outcome.error);
    if (outcome.wroteDump) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0)
            outcome.bytes = static_cast<uint64_t>(size.QuadPart);
    }
    CloseHandle(file);
    if (!outcome.wroteDump)
        DeleteFileW(dumpPath);
    else if (pruneOnSuccess)
        PruneCrashDumps(logDir, maxRetained);
    return outcome;
}

DWORD WINAPI StandaloneDumpThreadProc(LPVOID)
{
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto writeDump =
        dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : static_cast<MiniDumpWriteDumpFn>(nullptr);
    CrashDumpOutcome outcome;
    if (!dbghelp) {
        outcome.error = GetLastError();
        CopyBounded(outcome.dumpPath, std::size(outcome.dumpPath), g_standaloneDump.dumpPath);
    } else {
        outcome = WriteCrashDumpWithFn(writeDump, g_standaloneDump.logDir, g_standaloneDump.dumpPath, g_standaloneDump.threadId,
                                       SnapshotPointers(g_standaloneDump.exception), g_standaloneDump.maxRetained, false);
        FreeLibrary(dbghelp);
    }
    g_standaloneDump.outcome = outcome;
    DumpSlotState expected = DumpSlotState::Running;
    if (g_standaloneDump.state.compare_exchange_strong(expected, DumpSlotState::Finished, std::memory_order_acq_rel, std::memory_order_acquire))
        return 0;
    // Caller timed out and transitioned Running -> Abandoned, transferring cleanup responsibility here.
    if (outcome.wroteDump)
        DeleteFileW(g_standaloneDump.dumpPath);
    g_standaloneDump.state.store(DumpSlotState::Idle, std::memory_order_release);
    return 0;
}

void ProcessCrashRequest()
{
    if (const EmergencyRestoreFn restore = g_emergencyRestore.load(std::memory_order_acquire))
        restore();
    FlushLog(kLogFlushTimeoutMs);

    wchar_t dumpName[64]{};
    FormatCrashDumpNameBuffer(dumpName, std::size(dumpName), g_crashRequest.localTime, g_crashRequest.pid);
    wchar_t dumpPath[kMaxPathChars]{};
    CrashDumpOutcome dump;
    if (g_logDir[0] == L'\0') {
        dump.error = ERROR_PATH_NOT_FOUND;
    } else if (!JoinPath(dumpPath, std::size(dumpPath), g_logDir, dumpName)) {
        dump.error = ERROR_FILENAME_EXCED_RANGE;
    } else {
        CreateDirectoryW(g_logDir, nullptr);
        dump = WriteCrashDumpWithFn(g_writeDump, g_logDir, dumpPath, g_crashRequest.threadId, SnapshotPointers(g_crashRequest.exception), kMaxCrashDumps, true);
    }

    const EXCEPTION_RECORD* record = g_crashRequest.exception.hasRecord ? &g_crashRequest.exception.record : nullptr;
    const CrashDumpSummaryInfo info{dump.wroteDump, dump.fellBackToMinimal, dump.error, dump.bytes, dump.dumpPath};
    FormatCrashSummaryInfo(g_crashSummary, std::size(g_crashSummary), g_crashRequest.localTime, g_crashRequest.pid, g_crashRequest.threadId,
                           g_crashRequest.reason, record, g_crashRequest.frames, g_crashRequest.frameCount, info);
    if (g_coreLogPath[0] != L'\0' && g_crashSummary[0] != L'\0')
        AppendEmergencyLogLine(g_coreLogPath, g_crashSummary);
}

DWORD WINAPI CrashWorkerProc(LPVOID)
{
    for (;;) {
        const DWORD wait = WaitForSingleObject(g_crashRequestEvent, INFINITE);
        if (wait != WAIT_OBJECT_0 || g_crashWorkerStop.load(std::memory_order_acquire))
            break;
        ProcessCrashRequest();
        if (g_crashDoneEvent)
            SetEvent(g_crashDoneEvent);
    }
    return 0;
}

void HandleFatalCrash(const wchar_t* reason, EXCEPTION_POINTERS* exceptionPointers)
{
    ScopedActiveHandler activeHandler;
    if (g_handlerLifecycle.load(std::memory_order_seq_cst) != HandlerLifecycle::Running)
        return;
    if (g_inCrashHandler.exchange(true, std::memory_order_acq_rel)) {
        if (g_crashDoneEvent) {
            if (GetCurrentThreadId() == g_crashWorkerThreadId.load(std::memory_order_acquire))
                SetEvent(g_crashDoneEvent);
            else
                WaitForSingleObject(g_crashDoneEvent, kCrashWorkerTimeoutMs);
        }
        return;
    }
    if (!g_crashWorkerThread || !g_crashRequestEvent || !g_crashDoneEvent) {
        if (const EmergencyRestoreFn restore = g_emergencyRestore.load(std::memory_order_acquire))
            restore();
        return;
    }

    GetLocalTime(&g_crashRequest.localTime);
    g_crashRequest.pid = GetCurrentProcessId();
    g_crashRequest.threadId = GetCurrentThreadId();
    g_crashRequest.reason = (reason && *reason) ? reason : L"unknown";
    CopyExceptionSnapshot(exceptionPointers, g_crashRequest.exception);
    g_crashRequest.stackOverflow = g_crashRequest.exception.hasRecord && g_crashRequest.exception.record.ExceptionCode == EXCEPTION_STACK_OVERFLOW;
    g_crashRequest.frameCount = 0;
    if (!g_crashRequest.stackOverflow)
        g_crashRequest.frameCount = CaptureStackBackTrace(0, static_cast<DWORD>(kMaxStackFrames), g_crashRequest.frames, nullptr);

    ResetEvent(g_crashDoneEvent);
    SetEvent(g_crashRequestEvent);
    WaitForSingleObject(g_crashDoneEvent, kCrashWorkerTimeoutMs);
}

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionPointers)
{
    HandleFatalCrash(L"unhandled-exception", exceptionPointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

void HandleNonSehFatal(const wchar_t* reason, DWORD code)
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_AMD64)
    record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(context.Rip));
#elif defined(_M_ARM64)
    record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(context.Pc));
#endif
    EXCEPTION_POINTERS exceptionPointers{&record, &context};
    HandleFatalCrash(reason, &exceptionPointers);
    TerminateProcess(GetCurrentProcess(), code);
}

void TerminateHandler() { HandleNonSehFatal(L"std-terminate", kFatalAppExitCode); }

void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    HandleNonSehFatal(L"crt-invalid-parameter", kInvalidParameterCode);
}

void PurecallHandler() { HandleNonSehFatal(L"crt-purecall", EXCEPTION_NONCONTINUABLE_EXCEPTION); }

void ReleaseCrashWorkerResources()
{
    CloseAndClear(g_crashWorkerThread);
    g_crashWorkerThreadId.store(0, std::memory_order_release);
    CloseAndClear(g_crashRequestEvent);
    CloseAndClear(g_crashDoneEvent);
    g_writeDump = nullptr;
    if (g_dbghelp) {
        FreeLibrary(g_dbghelp);
        g_dbghelp = nullptr;
    }
}

bool DrainActiveCrashHandlers()
{
    if (g_activeHandlers.load(std::memory_order_seq_cst) == 0)
        return true;
    HANDLE drainEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const ULONGLONG deadline = GetTickCount64() + kCrashWorkerTimeoutMs + 500;
    while (g_activeHandlers.load(std::memory_order_seq_cst) > 0 && GetTickCount64() < deadline && drainEvent)
        WaitForSingleObject(drainEvent, 25);
    CloseAndClear(drainEvent);
    return g_activeHandlers.load(std::memory_order_seq_cst) == 0;
}

void ShutdownCrashHandlersLocked()
{
    if (g_handlerLifecycle.load(std::memory_order_acquire) != HandlerLifecycle::Running)
        return;
    SetUnhandledExceptionFilter(g_prevUnhandledFilter);
    std::set_terminate(g_prevTerminateHandler);
    _set_invalid_parameter_handler(g_prevInvalidParamHandler);
    _set_purecall_handler(g_prevPurecallHandler);
    g_prevUnhandledFilter = nullptr;
    g_prevTerminateHandler = nullptr;
    g_prevInvalidParamHandler = nullptr;
    g_prevPurecallHandler = nullptr;

    g_handlerLifecycle.store(HandlerLifecycle::Stopping, std::memory_order_seq_cst);
    if (!DrainActiveCrashHandlers()) {
        g_handlerLifecycle.store(HandlerLifecycle::Poisoned, std::memory_order_release);
        return;
    }
    if (g_crashWorkerThread) {
        g_crashWorkerStop.store(true, std::memory_order_release);
        if (g_crashRequestEvent)
            SetEvent(g_crashRequestEvent);
        if (WaitForSingleObject(g_crashWorkerThread, kWorkerJoinTimeoutMs) != WAIT_OBJECT_0) {
            g_handlerLifecycle.store(HandlerLifecycle::Poisoned, std::memory_order_release);
            return;
        }
    }
    ReleaseCrashWorkerResources();
    g_emergencyRestore.store(nullptr, std::memory_order_release);
    g_handlerLifecycle.store(HandlerLifecycle::Stopped, std::memory_order_seq_cst);
}

} // namespace

void InstallCrashHandlers(EmergencyRestoreFn emergencyRestore)
{
    std::lock_guard lifecycleLock(g_lifecycleMutex);
    if (g_handlerLifecycle.load(std::memory_order_acquire) == HandlerLifecycle::Running)
        ShutdownCrashHandlersLocked();
    if (g_handlerLifecycle.load(std::memory_order_acquire) == HandlerLifecycle::Poisoned) {
        Log(L"crash: previous crash worker did not exit; keeping handlers disabled");
        return;
    }

    g_inCrashHandler.store(false, std::memory_order_release);
    g_emergencyRestore.store(emergencyRestore, std::memory_order_release);

    const std::wstring dir = DataDirectory() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!CopyBounded(g_logDir, std::size(g_logDir), dir.c_str()))
        g_logDir[0] = L'\0';
    const std::wstring logPath = dir + L"\\core.log";
    if (!CopyBounded(g_coreLogPath, std::size(g_coreLogPath), logPath.c_str()))
        g_coreLogPath[0] = L'\0';

    g_dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    g_writeDump = g_dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(g_dbghelp, "MiniDumpWriteDump")) : nullptr;
    g_crashWorkerStop.store(false, std::memory_order_release);
    g_crashRequestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_crashDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD workerThreadId = 0;
    if (g_crashRequestEvent && g_crashDoneEvent)
        g_crashWorkerThread = CreateThread(nullptr, kDumpThreadStackBytes, &CrashWorkerProc, nullptr, 0, &workerThreadId);
    g_crashWorkerThreadId.store(g_crashWorkerThread ? workerThreadId : 0, std::memory_order_release);
    if (!g_crashWorkerThread) {
        Log(L"crash: worker unavailable (%lu); minidumps disabled", GetLastError());
        ReleaseCrashWorkerResources();
    }

    ULONG stackGuarantee = 32768;
    SetThreadStackGuarantee(&stackGuarantee);
    g_prevUnhandledFilter = SetUnhandledExceptionFilter(&UnhandledExceptionHandler);
    g_prevTerminateHandler = std::set_terminate(&TerminateHandler);
    g_prevInvalidParamHandler = _set_invalid_parameter_handler(&InvalidParameterHandler);
    g_prevPurecallHandler = _set_purecall_handler(&PurecallHandler);
    g_handlerLifecycle.store(HandlerLifecycle::Running, std::memory_order_seq_cst);
}

void ShutdownCrashHandlers()
{
    std::lock_guard lifecycleLock(g_lifecycleMutex);
    ShutdownCrashHandlersLocked();
}

CrashDumpResult WriteCrashDump(const wchar_t* logDir, const wchar_t* dumpName, DWORD threadId, EXCEPTION_POINTERS* exceptionPointers, unsigned maxRetained)
{
    CrashDumpResult result;
    if (!logDir || !*logDir || !IsSafeFileName(dumpName)) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    wchar_t dumpPath[kMaxPathChars]{};
    if (!JoinPath(dumpPath, std::size(dumpPath), logDir, dumpName)) {
        result.error = ERROR_FILENAME_EXCED_RANGE;
        return result;
    }
    result.dumpPath = dumpPath;
    DumpSlotState expected = DumpSlotState::Idle;
    if (!g_standaloneDump.state.compare_exchange_strong(expected, DumpSlotState::Running, std::memory_order_acq_rel, std::memory_order_acquire)) {
        result.error = ERROR_BUSY;
        return result;
    }

    CreateDirectoryW(logDir, nullptr);
    g_standaloneDump.threadId = threadId;
    g_standaloneDump.maxRetained = maxRetained;
    CopyBounded(g_standaloneDump.logDir, std::size(g_standaloneDump.logDir), logDir);
    CopyBounded(g_standaloneDump.dumpPath, std::size(g_standaloneDump.dumpPath), dumpPath);
    CopyExceptionSnapshot(exceptionPointers, g_standaloneDump.exception);
    g_standaloneDump.outcome = CrashDumpOutcome{};

    HANDLE thread = CreateThread(nullptr, kDumpThreadStackBytes, &StandaloneDumpThreadProc, nullptr, 0, nullptr);
    if (!thread) {
        result.error = GetLastError();
        g_standaloneDump.state.store(DumpSlotState::Idle, std::memory_order_release);
        return result;
    }
    const DWORD wait = WaitForSingleObject(thread, kCrashWorkerTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        DumpSlotState running = DumpSlotState::Running;
        if (g_standaloneDump.state.compare_exchange_strong(running, DumpSlotState::Abandoned, std::memory_order_acq_rel, std::memory_order_acquire)) {
            CloseHandle(thread);
            result.error = ERROR_TIMEOUT;
            return result;
        }
        WaitForSingleObject(thread, kWorkerJoinTimeoutMs);
    }
    CloseHandle(thread);
    const CrashDumpOutcome outcome = g_standaloneDump.outcome;
    result.wroteDump = outcome.wroteDump;
    result.fellBackToMinimal = outcome.fellBackToMinimal;
    result.error = outcome.error;
    result.bytes = outcome.bytes;
    if (result.wroteDump)
        PruneCrashDumps(g_standaloneDump.logDir, g_standaloneDump.maxRetained);
    g_standaloneDump.state.store(DumpSlotState::Idle, std::memory_order_release);
    return result;
}

} // namespace atrium
