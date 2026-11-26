#include "crash_dump.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace atrium {

namespace {

constexpr size_t kMaxPathChars = MAX_PATH * 2;
constexpr DWORD kFatalAppExitCode = 0x40000015UL;
constexpr DWORD kInvalidParameterCode = 0xC000000DUL;
constexpr DWORD kCxxExceptionCode = 0xE06D7363UL;

struct DumpEntry {
    FILETIME writeTime{};
    wchar_t name[MAX_PATH]{};
};

void AppendFormatted(wchar_t* buffer, size_t capacity, size_t& pos, const wchar_t* format, ...)
{
    if (!buffer || capacity == 0 || pos >= capacity - 1)
        return;
    va_list args;
    va_start(args, format);
    const int written = _vsnwprintf_s(buffer + pos, capacity - pos, _TRUNCATE, format, args);
    va_end(args);
    if (written >= 0) {
        pos += static_cast<size_t>(written);
        return;
    }
    pos = capacity - 1;
    buffer[pos] = L'\0';
}

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

bool IsAsciiDigits(const wchar_t* text, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (text[i] < L'0' || text[i] > L'9')
            return false;
    }
    return true;
}

bool IsAtriumCrashDumpFileName(const wchar_t* name)
{
    if (!name)
        return false;
    const size_t len = wcslen(name);
    if (len < 27 || _wcsnicmp(name, L"crash-", 6) != 0 || _wcsicmp(name + len - 4, L".dmp") != 0)
        return false;
    if (!IsAsciiDigits(name + 6, 8) || name[14] != L'-' || !IsAsciiDigits(name + 15, 6) || name[21] != L'-')
        return false;
    return IsAsciiDigits(name + 22, len - 26);
}

bool DumpEntryNewer(const DumpEntry& a, const DumpEntry& b)
{
    const LONG cmp = CompareFileTime(&a.writeTime, &b.writeTime);
    if (cmp != 0)
        return cmp > 0;
    return wcscmp(a.name, b.name) > 0;
}

void InsertSortedEntry(DumpEntry* kept, size_t count, const DumpEntry& item)
{
    size_t pos = count;
    while (pos > 0 && DumpEntryNewer(item, kept[pos - 1])) {
        kept[pos] = kept[pos - 1];
        --pos;
    }
    kept[pos] = item;
}

void DeleteDumpIfValidPath(const wchar_t* logDir, const wchar_t* fileName, unsigned& removed)
{
    wchar_t fullPath[kMaxPathChars]{};
    if (JoinPath(fullPath, std::size(fullPath), logDir, fileName) && DeleteFileW(fullPath))
        ++removed;
}

void AppendTimestampPrefix(wchar_t* buffer, size_t capacity, size_t& pos, const SYSTEMTIME& localTime, DWORD threadId)
{
    AppendFormatted(buffer, capacity, pos, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ", localTime.wYear, localTime.wMonth, localTime.wDay, localTime.wHour,
                    localTime.wMinute, localTime.wSecond, localTime.wMilliseconds, threadId);
}

void AppendExceptionDetails(wchar_t* buffer, size_t capacity, size_t& pos, const EXCEPTION_RECORD* record)
{
    if (!record) {
        AppendFormatted(buffer, capacity, pos, L" code=none");
        return;
    }
    AppendFormatted(buffer, capacity, pos, L" code=0x%08lX (%s", record->ExceptionCode, ExceptionCodeName(record->ExceptionCode));
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2) {
        const ULONG_PTR op = record->ExceptionInformation[0];
        const wchar_t* access = op == 0 ? L"read" : op == 1 ? L"write" : op == 8 ? L"dep" : L"access";
        AppendFormatted(buffer, capacity, pos, L", %s 0x%llX", access, static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    wchar_t faultFrame[160]{};
    FormatFrameAddress(record->ExceptionAddress, faultFrame, std::size(faultFrame));
    AppendFormatted(buffer, capacity, pos, L") at %s", faultFrame);
}

} // namespace

const wchar_t* ExceptionCodeName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return L"ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR: return L"IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW: return L"STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return L"ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return L"PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return L"INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return L"FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return L"FLT_INVALID_OPERATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return L"ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return L"DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT: return L"BREAKPOINT";
    case EXCEPTION_GUARD_PAGE: return L"GUARD_PAGE";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return L"NONCONTINUABLE_EXCEPTION";
    case kFatalAppExitCode: return L"FATAL_APP_EXIT";
    case kInvalidParameterCode: return L"INVALID_PARAMETER";
    case kCxxExceptionCode: return L"CPP_EXCEPTION";
    default: return L"UNKNOWN";
    }
}

std::wstring FormatCrashDumpName(const SYSTEMTIME& localTime, DWORD pid)
{
    wchar_t name[64]{};
    size_t pos = 0;
    AppendFormatted(name, std::size(name), pos, L"crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", localTime.wYear, localTime.wMonth, localTime.wDay, localTime.wHour,
                    localTime.wMinute, localTime.wSecond, pid);
    return name;
}

size_t FormatFrameAddress(const void* address, wchar_t* buffer, size_t capacity)
{
    if (!buffer || capacity == 0)
        return 0;
    buffer[0] = L'\0';
    const auto rawAddress = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address));
    HMODULE module = nullptr;
    if (address &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module)) {
        wchar_t modulePath[kMaxPathChars]{};
        const DWORD pathLen = GetModuleFileNameW(module, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (pathLen > 0 && pathLen < std::size(modulePath)) {
            const wchar_t* base = modulePath;
            for (const wchar_t* p = modulePath; *p != L'\0'; ++p) {
                if (*p == L'\\' || *p == L'/')
                    base = p + 1;
            }
            const auto baseAddress = reinterpret_cast<uintptr_t>(module);
            const auto offset = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - baseAddress);
            size_t pos = 0;
            AppendFormatted(buffer, capacity, pos, L"%s+0x%llX", base, offset);
            return pos;
        }
    }
    size_t pos = 0;
    AppendFormatted(buffer, capacity, pos, L"0x%016llX", rawAddress);
    return pos;
}

size_t FormatCrashSummaryInfo(wchar_t* buffer, size_t capacity, const SYSTEMTIME& localTime, DWORD pid, DWORD threadId, const wchar_t* reason,
                              const EXCEPTION_RECORD* record, const void* const* frames, size_t frameCount, const CrashDumpSummaryInfo& dump)
{
    if (!buffer || capacity == 0)
        return 0;
    buffer[0] = L'\0';
    size_t pos = 0;
    const wchar_t* safeReason = (reason && *reason) ? reason : L"unknown";
    AppendTimestampPrefix(buffer, capacity, pos, localTime, threadId);
    AppendFormatted(buffer, capacity, pos, L"crash: %s pid=%lu tid=%lu", safeReason, pid, threadId);
    AppendExceptionDetails(buffer, capacity, pos, record);
    AppendFormatted(buffer, capacity, pos, L"\n");

    if (frames && frameCount > 0) {
        AppendTimestampPrefix(buffer, capacity, pos, localTime, threadId);
        AppendFormatted(buffer, capacity, pos, L"crash: handler-stack:");
        for (size_t i = 0; i < frameCount; ++i) {
            wchar_t frame[160]{};
            FormatFrameAddress(frames[i], frame, std::size(frame));
            AppendFormatted(buffer, capacity, pos, i == 0 ? L" %s" : L" <- %s", frame);
        }
        AppendFormatted(buffer, capacity, pos, L"\n");
    }

    AppendTimestampPrefix(buffer, capacity, pos, localTime, threadId);
    if (dump.wroteDump) {
        AppendFormatted(buffer, capacity, pos, L"crash: minidump %s (%llu bytes%s)\n", dump.dumpPath ? dump.dumpPath : L"",
                        static_cast<unsigned long long>(dump.bytes), dump.fellBackToMinimal ? L"; minimal" : L"");
    } else {
        AppendFormatted(buffer, capacity, pos, L"crash: minidump failed (err=%lu)\n", dump.error);
    }
    return pos;
}

size_t FormatCrashSummary(wchar_t* buffer, size_t capacity, const SYSTEMTIME& localTime, DWORD pid, DWORD threadId, const wchar_t* reason,
                          const EXCEPTION_RECORD* record, const void* const* frames, size_t frameCount, const CrashDumpResult& dump)
{
    const CrashDumpSummaryInfo info{dump.wroteDump, dump.fellBackToMinimal, dump.error, dump.bytes, dump.dumpPath.c_str()};
    return FormatCrashSummaryInfo(buffer, capacity, localTime, pid, threadId, reason, record, frames, frameCount, info);
}

unsigned PruneCrashDumps(const wchar_t* logDir, unsigned maxRetained)
{
    if (!logDir || !*logDir)
        return 0;
    wchar_t pattern[kMaxPathChars]{};
    if (!JoinPath(pattern, std::size(pattern), logDir, L"crash-*.dmp"))
        return 0;
    WIN32_FIND_DATAW findData{};
    HANDLE search = FindFirstFileW(pattern, &findData);
    if (search == INVALID_HANDLE_VALUE)
        return 0;

    constexpr DWORD kSkipAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    const size_t keepLimit = maxRetained < kMaxRetainedCrashDumps ? maxRetained : kMaxRetainedCrashDumps;
    DumpEntry kept[kMaxRetainedCrashDumps]{};
    size_t keptCount = 0;
    unsigned removed = 0;
    do {
        if ((findData.dwFileAttributes & kSkipAttributes) != 0 || !IsAtriumCrashDumpFileName(findData.cFileName))
            continue;
        if (keepLimit == 0) {
            DeleteDumpIfValidPath(logDir, findData.cFileName, removed);
            continue;
        }
        DumpEntry candidate{};
        candidate.writeTime = findData.ftLastWriteTime;
        if (!CopyBounded(candidate.name, std::size(candidate.name), findData.cFileName))
            continue;
        if (keptCount < keepLimit) {
            InsertSortedEntry(kept, keptCount++, candidate);
        } else if (DumpEntryNewer(candidate, kept[keepLimit - 1])) {
            DeleteDumpIfValidPath(logDir, kept[keepLimit - 1].name, removed);
            InsertSortedEntry(kept, keepLimit - 1, candidate);
        } else {
            DeleteDumpIfValidPath(logDir, candidate.name, removed);
        }
    } while (FindNextFileW(search, &findData));
    FindClose(search);
    return removed;
}

bool AppendEmergencyLogLine(const wchar_t* logFilePath, const wchar_t* text)
{
    if (!logFilePath || !*logFilePath || !text || !*text)
        return false;
    HANDLE file = CreateFileW(logFilePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const size_t totalChars = wcslen(text);
    size_t offset = 0;
    bool ok = true;
    while (offset < totalChars) {
        size_t chunk = totalChars - offset;
        if (chunk > 1024) {
            chunk = 1024;
            const wchar_t last = text[offset + chunk - 1];
            if (last >= 0xD800 && last <= 0xDBFF)
                --chunk;
        }
        char utf8[4096];
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text + offset, static_cast<int>(chunk), utf8, static_cast<int>(std::size(utf8)), nullptr, nullptr);
        if (bytes <= 0) {
            ok = false;
            break;
        }
        int writtenTotal = 0;
        while (writtenTotal < bytes) {
            DWORD written = 0;
            if (!WriteFile(file, utf8 + writtenTotal, static_cast<DWORD>(bytes - writtenTotal), &written, nullptr) || written == 0) {
                ok = false;
                break;
            }
            writtenTotal += static_cast<int>(written);
        }
        if (!ok)
            break;
        offset += chunk;
    }
    if (ok && !FlushFileBuffers(file))
        ok = false;
    CloseHandle(file);
    return ok;
}

} // namespace atrium
