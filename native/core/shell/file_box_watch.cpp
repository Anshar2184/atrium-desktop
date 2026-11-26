#include "shell/file_box_watch.h"

#include "log.h"

#include <vector>

namespace atrium {

namespace {

// The changes a box cares about: what is in the folder, and how big or how new its files are.
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_LAST_WRITE;

constexpr DWORD kBufferBytes = 16 * 1024;

} // namespace

FileBoxWatch::FileBoxWatch(HWND host, UINT message, void* owner) : host_(host), message_(message), owner_(owner) {}

FileBoxWatch::~FileBoxWatch() { Stop(); }

void FileBoxWatch::Stop()
{
    if (stop_)
        SetEvent(stop_);
    if (thread_.joinable())
        thread_.join();
    if (stop_) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
    folder_.clear();
}

void FileBoxWatch::SetFolder(const std::wstring& folder)
{
    if (folder == folder_ && thread_.joinable())
        return; // already following it
    Stop();
    if (folder.empty())
        return;
    folder_ = folder;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_)
        return;
    // The thread owns nothing but its own handles: Stop sets the event and joins it.
    thread_ = std::thread([this, folder] { Run(folder); });
}

void FileBoxWatch::Run(std::wstring folder)
{
    HANDLE dir = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        Log(L"files: cannot watch %s (%lu)", folder.c_str(), GetLastError());
        return;
    }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        CloseHandle(dir);
        return;
    }
    std::vector<BYTE> buffer(kBufferBytes);
    for (;;) {
        if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0)
            break;
        ResetEvent(ov.hEvent);
        // The first call reports what changed while the last was being handled: nothing is lost
        // between a read and the next wait.
        if (!ReadDirectoryChangesW(dir, buffer.data(), DWORD(buffer.size()), FALSE, kFilter, nullptr, &ov, nullptr))
            break;
        HANDLE waits[2] = {stop_, ov.hEvent};
        const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (which != WAIT_OBJECT_0 + 1)
            break; // stopped, or the wait itself failed
        DWORD got = 0;
        if (!GetOverlappedResult(dir, &ov, &got, FALSE))
            break;
        if (got == 0)
            continue;
        if (!PostMessageW(host_, message_, 0, reinterpret_cast<LPARAM>(owner_)))
            break; // the host is gone
        // A burst of changes (a copy of many files) is one refresh: let it settle first.
        Sleep(250);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
}

} // namespace atrium
