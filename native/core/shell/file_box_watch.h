// One folder, watched on a thread of its own: ReadDirectoryChangesW blocks until something
// changes, so a file box that follows a folder needs a thread that can wait. When the folder
// changes, `message` is posted to `host` with the box's address. The box checks that address
// against its own list before using it (WM_WIDGET_WATCH), so a box removed while the change was
// in flight is simply skipped.
#pragma once

#include <windows.h>

#include <string>
#include <thread>

namespace atrium {

class FileBoxWatch {
public:
    FileBoxWatch(HWND host, UINT message, void* owner);
    ~FileBoxWatch();
    FileBoxWatch(const FileBoxWatch&) = delete;
    FileBoxWatch& operator=(const FileBoxWatch&) = delete;

    // Follows `folder` instead of what it followed before (empty: stop watching).
    void SetFolder(const std::wstring& folder);
    void Stop();
    const std::wstring& Folder() const { return folder_; }

private:
    void Run(std::wstring folder);

    HWND host_ = nullptr;
    UINT message_ = 0;
    void* owner_ = nullptr;
    std::wstring folder_;
    HANDLE stop_ = nullptr;
    std::thread thread_;
};

} // namespace atrium
