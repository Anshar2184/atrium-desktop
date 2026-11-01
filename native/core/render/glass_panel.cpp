#include "glass_panel.h"

#include <algorithm>
#include <map>
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

std::mutex g_inkMutex;
std::map<HWND, InkGrid> g_ink;

} // namespace

bool InkGrid::DarkAt(float x0, float y0, float x1, float y1, bool fallback) const
{
    if (dark.empty() || w <= 0 || h <= 0)
        return fallback;
    int gx0 = std::clamp(int(x0 / cellX), 0, w - 1), gx1 = std::clamp(int((x1 - 0.01f) / cellX), 0, w - 1);
    int gy0 = std::clamp(int(y0 / cellY), 0, h - 1), gy1 = std::clamp(int((y1 - 0.01f) / cellY), 0, h - 1);
    int n = 0, d = 0;
    for (int y = gy0; y <= gy1; ++y)
        for (int x = gx0; x <= gx1; ++x, ++n)
            d += dark[size_t(y) * size_t(w) + size_t(x)];
    return n ? 2 * d >= n : fallback;
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
