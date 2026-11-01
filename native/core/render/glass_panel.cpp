#include "glass_panel.h"

#include <algorithm>

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

} // namespace

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
