// One millisecond clock for animations shared by the UI thread and the render
// thread (QueryPerformanceCounter, monotonic).
#pragma once

#include <windows.h>

namespace atrium {

inline double ClockMs()
{
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) * 1000.0 / double(freq.QuadPart);
}

} // namespace atrium
