// User-adjustable appearance and layout. One flat JSON object, shared with
// Atrium.Settings (settings/Atrium.Settings/Model/BarSettings.cs) and the pipe
// protocol (docs/SETTINGS-PROTOCOL.md). Lengths are in DIP.
#pragma once

#include <cstdint>
#include <string>

namespace atrium {

struct Settings {
    static constexpr int kVersion = 1;

    std::wstring preset = L"liquid"; // liquid | frosted | custom (informational)
    double transparency = 0.90;      // 0 = fully tinted, 1 = clear glass
    double cornerRadius = 28;        // clamped to half the bar height when drawn
    double blur = 0.35;              // 0 = sharp backdrop, 1 = fully frosted
    double refraction = 13;          // max sample displacement at the rim
    double edge = 17;                // width of the refracting rim
    double dispersion = 0.08;        // relative channel spread of the refraction
    double highlight = 0.55;
    double shadow = 0.28;
    uint32_t tint = 0xFFFFFF;        // 0xRRGGBB
    double width = 720;
    double height = 56;
    double margin = 10;              // gap above the bottom of the work area
    bool showClock = true;

    static Settings Liquid();
    static Settings Frosted();

    // Clamps every value into its valid range (input from disk or the pipe).
    void Clamp();
    bool SameLayout(const Settings& o) const { return width == o.width && height == o.height && margin == o.margin; }

    std::wstring ToJson() const;
    // Missing keys keep the values of `base`; malformed JSON returns false.
    static bool FromJson(const std::wstring& json, const Settings& base, Settings& out);
};

// %LOCALAPPDATA%\Atrium\settings.json
std::wstring SettingsPath();
Settings LoadSettings();
// Atomic replace; returns false if the file could not be written.
bool SaveSettings(const Settings& s);

} // namespace atrium
