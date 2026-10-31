// User-adjustable appearance and layout. One flat JSON object, shared with
// Atrium.Settings (settings/Atrium.Settings/Model/BarSettings.cs) and the pipe
// protocol (docs/SETTINGS-PROTOCOL.md). Lengths are in DIP.
#pragma once

#include "shell/task_model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

struct Settings {
    static constexpr int kVersion = 1;

    // Material (presets change only these). Liquid follows macOS Liquid Glass:
    // nearly clear centre, strong lensing over a wide rim, visible dispersion.
    std::wstring preset = L"liquid"; // liquid | frosted | custom (informational)
    double transparency = 0.95;      // 0 = fully tinted, 1 = clear glass
    double blur = 0.50;              // 0 = sharp backdrop, 1 = fully frosted
    double refraction = 30;          // max sample displacement at the rim
    double edge = 24;                // width of the refracting rim
    double dispersion = 0.30;        // relative channel spread of the refraction
    double highlight = 0.80;
    double shadow = 0.30;
    uint32_t tint = 0xFFFFFF;        // 0xRRGGBB

    // Layout and content.
    double cornerRadius = 30;        // clamped to half the bar height when drawn
    double width = 1200;             // maximum width when autoWidth
    double height = 60;
    double margin = 10;              // gap above the bottom of the work area
    double iconSize = 30;
    bool autoWidth = true;           // shrink to the content
    bool showClock = true;
    bool showRunning = true;
    std::wstring textColor = L"auto"; // auto | light | dark
    // Pinned apps; nullopt = never saved yet (first run imports the Windows pins).
    std::optional<std::vector<PinnedApp>> pins;

    static Settings Liquid();
    static Settings Frosted();

    // Clamps every value into its valid range (input from disk or the pipe).
    void Clamp();
    // Copies the material of `preset`, keeping layout and content.
    void ApplyMaterial(const Settings& preset);

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
