// DWM thumbnails, driven from a thread of their own. DWM answers slowly while it animates windows
// (minimizing, restoring: 100 to 350 ms a call), and the UI thread must not wait for it: a strip of
// pictures would stop gliding exactly when windows come and go. Calls keep their order, so the
// pictures stack in the order they were registered; of the updates to one picture only the latest
// waiting is made.
#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <cstdint>
#include <memory>

namespace atrium {

class ThumbnailThread {
public:
    // `message` is posted to `notify` (wParam: the picture's id) once its source's size is known.
    ThumbnailThread(HWND notify, UINT message);
    ~ThumbnailThread();
    ThumbnailThread(const ThumbnailThread&) = delete;
    ThumbnailThread& operator=(const ThumbnailThread&) = delete;

    // A picture of `source` in `destination`; the id is there at once, the picture a little later.
    uint64_t Register(HWND destination, HWND source);
    void Update(uint64_t id, const DWM_THUMBNAIL_PROPERTIES& p);
    void Unregister(uint64_t id);
    // The source's size in its own px, {0, 0} until DWM has said (or if it could not).
    SIZE SourceSize(uint64_t id) const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace atrium
