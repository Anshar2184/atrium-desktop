#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

namespace {

std::wstring LogPath()
{
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
        dir = std::wstring(base) + L"\\Atrium";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\core.log";
}

// Lines are written by a thread of their own into a file kept open: opening
// and closing it for every line (a virus scan each time) took milliseconds,
// under a lock every thread shared - animations stalled on their own log
// lines. A file over 8 MB is moved to core.1.log at start.
class Writer {
public:
    static Writer& Get()
    {
        static Writer w;
        return w;
    }
    void Add(std::wstring line)
    {
        {
            std::lock_guard g(mutex_);
            queue_.push_back(std::move(line));
        }
        cv_.notify_one();
    }

private:
    Writer() : thread_([this] { Run(); }) {}
    ~Writer()
    {
        {
            std::lock_guard g(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }
    void Run()
    {
        SetThreadDescription(GetCurrentThread(), L"Atrium log");
        const std::wstring path = LogPath();
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && (a.nFileSizeHigh || a.nFileSizeLow > 8u << 20))
            MoveFileExW(path.c_str(), (path.substr(0, path.size() - 4) + L".1.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        FILE* f = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO); // others may read it meanwhile
        std::vector<std::wstring> lines;
        for (;;) {
            {
                std::unique_lock g(mutex_);
                cv_.wait(g, [&] { return stop_ || !queue_.empty(); });
                lines.swap(queue_);
                if (lines.empty() && stop_)
                    break;
            }
            if (f) {
                for (const std::wstring& l : lines)
                    fputws(l.c_str(), f);
                fflush(f);
            }
            lines.clear();
        }
        if (f)
            fclose(f);
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::wstring> queue_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace

void Log(const wchar_t* format, ...)
{
    wchar_t text[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds, GetCurrentThreadId(), text);
    OutputDebugStringW(line);
    Writer::Get().Add(line);
}

} // namespace atrium
