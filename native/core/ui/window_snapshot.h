// Still pictures of windows for Stage Manager's strip: taken just before a window is put away
// (a minimized window has nothing to show), on worker threads, since PrintWindow waits for the
// window's app. A picture is small (about the size it is shown at), its corners rounded, with a soft
// shadow of its own; both premultiplied BGRA, ready for Canvas::ImagePerspective.
#pragma once

#include "shell/task_model.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <vector>

namespace atrium {

struct WindowPicture {
    IconImage image;  // the window, with kPicturePad transparent px around it
    IconImage shadow; // its shadow at half the size, kShadowPad of the picture's px around it
    int width = 0, height = 0; // the window's part of `image`, px
    bool ok = false;
};

constexpr int kPicturePad = 2;
constexpr int kShadowPad = 24;

// The soft shadow of a w x h rounded rectangle, at half its size, kShadowPad of its px around it.
IconImage SoftShadow(int w, int h);

// The picture of a placeholder window: `icon` in the middle of a plain rounded panel (a window
// put away before Atrium started has no picture).
WindowPicture PlaceholderPicture(const IconImage& icon, int width, int height, bool dark);

class WindowSnapshots {
public:
    // Results are announced with `message` posted to `notify`, then Take()n.
    WindowSnapshots(HWND notify, UINT message);
    ~WindowSnapshots();
    WindowSnapshots(const WindowSnapshots&) = delete;
    WindowSnapshots& operator=(const WindowSnapshots&) = delete;

    // Pictures `window` at most maxWidth x maxHeight px, corners of `radius` px.
    void Request(HWND window, int maxWidth, int maxHeight, float radius);
    struct Result {
        HWND window;
        WindowPicture picture;
    };
    std::vector<Result> Take();

private:
    struct Shared {
        std::mutex lock;
        std::vector<Result> done;
        HWND notify = nullptr;
        UINT message = 0;
    };
    std::shared_ptr<Shared> shared_;
};

} // namespace atrium
