#include "glass_panel.h"

#include <algorithm>
#include <map>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace atrium {

namespace {

bool SystemLight()
{
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

std::mutex g_genMutex;
std::condition_variable g_genChanged;
std::map<HWND, unsigned> g_gen;

std::mutex g_inkMutex;
std::map<HWND, InkGrid> g_ink;

} // namespace

void InkFade::Target(float target, double nowMs)
{
    if (!set) {
        from = to = target;
        set = true;
        return;
    }
    if (target == to)
        return;
    from = Value(nowMs);
    to = target;
    start = nowMs;
}

float InkFade::Value(double nowMs) const
{
    double t = std::clamp((nowMs - start) / kInkFadeMs, 0.0, 1.0);
    double e = 1 - (1 - t) * (1 - t) * (1 - t); // ease out: away from the old ink at once (it may be the backdrop's colour now)
    return from + (to - from) * float(e);
}

float InkField::DarkAt(float x0, float y0, float x1, float y1, float fallback) const
{
    if (dark.empty() || w <= 0 || h <= 0)
        return fallback;
    int gx0 = std::clamp(int(x0 / cellX), 0, w - 1), gx1 = std::clamp(int((x1 - 0.01f) / cellX), 0, w - 1);
    int gy0 = std::clamp(int(y0 / cellY), 0, h - 1), gy1 = std::clamp(int((y1 - 0.01f) / cellY), 0, h - 1);
    thread_local std::vector<float> cells;
    cells.clear();
    for (int y = gy0; y <= gy1; ++y)
        for (int x = gx0; x <= gx1; ++x)
            cells.push_back(dark[size_t(y) * size_t(w) + size_t(x)]);
    if (cells.empty())
        return fallback;
    auto mid = cells.begin() + ptrdiff_t(cells.size() / 2); // the upper median: ties favor dark ink
    std::nth_element(cells.begin(), mid, cells.end());
    return *mid;
}

void GlassPresented(HWND hwnd, unsigned gen)
{
    {
        std::lock_guard g(g_genMutex);
        g_gen[hwnd] = gen;
    }
    g_genChanged.notify_all();
}

void ForgetGlass(HWND hwnd)
{
    std::scoped_lock g(g_genMutex, g_inkMutex);
    g_gen.erase(hwnd);
    g_ink.erase(hwnd);
}

bool WaitGlass(HWND hwnd, unsigned gen, double timeoutMs)
{
    std::unique_lock g(g_genMutex);
    // Generations only grow; a wrap-around is centuries away at 60 per second.
    return g_genChanged.wait_for(g, std::chrono::microseconds(int64_t(timeoutMs * 1000)), [&] {
        auto it = g_gen.find(hwnd);
        return it != g_gen.end() && it->second >= gen;
    });
}

void PublishInk(HWND hwnd, const InkGrid& grid)
{
    std::lock_guard g(g_inkMutex);
    if (grid.dark.empty())
        g_ink.erase(hwnd);
    else
        g_ink[hwnd] = grid;
}

InkGrid InkFor(HWND hwnd)
{
    std::lock_guard g(g_inkMutex);
    auto it = g_ink.find(hwnd);
    return it == g_ink.end() ? InkGrid{} : it->second;
}

PanelTone PanelToneFor(uint32_t userTint, double transparency)
{
    PanelTone t;
    float userMix = float(1.0 - transparency);
    if (userMix >= 0.45f) {
        float luma = (0.2126f * float((userTint >> 16) & 0xFF) + 0.7152f * float((userTint >> 8) & 0xFF) + 0.0722f * float(userTint & 0xFF)) / 255.f;
        t.light = luma > 0.55f;
        t.tint = userTint;
    } else {
        t.light = SystemLight();
        t.tint = t.light ? 0xF7F7FA : 0x1D1E23;
    }
    return t;
}

} // namespace atrium
