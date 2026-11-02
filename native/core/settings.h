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
    static constexpr int kVersion = 2; // 2: Windows notifications on by default (v1 files are migrated)

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
    double magnify = 1.7;            // dock magnification under the pointer: icon scale, 1 = off
    bool autoWidth = true;           // shrink to the content
    bool showClock = true;
    bool showRunning = true;
    std::wstring textColor = L"auto"; // auto | light | dark
    // Notifications (Atrium's own cards above the bell).
    double notifyTimeout = 6;        // seconds a popup stays (3..30)
    bool notifyHideBody = false;     // privacy: no message text; cards excluded from screen capture
    bool notifyListener = true;      // Windows notifications as Atrium cards (UserNotificationListener)
    bool notifyFeishu = true;        // Feishu messages as Atrium cards (its own popup, read through UI Automation)
    bool notifyWeChat = true;        // WeChat messages as Atrium cards (its chat list, read through UI Automation)
    bool widgets = true;             // desktop widgets (their layout is widgets.json)
    // Pinned apps; nullopt = never saved yet (first run imports the Windows pins).
    std::optional<std::vector<PinnedApp>> pins;

    static Settings Liquid();
    static Settings Frosted();

    // Clamps every value into its valid range (input from disk or the pipe).
    void Clamp();

    std::wstring ToJson() const;
    // Missing keys keep the values of `base`; malformed JSON returns false.
    static bool FromJson(const std::wstring& json, const Settings& base, Settings& out);
};

// %LOCALAPPDATA%\Atrium\settings.json
std::wstring SettingsPath();
// UTF-8 text files (a BOM is skipped); the write replaces the file at once.
std::wstring ReadTextFile(const std::wstring& path);
bool WriteTextFile(const std::wstring& path, const std::wstring& text);
Settings LoadSettings();
// Atomic replace; returns false if the file could not be written.
bool SaveSettings(const Settings& s);

} // namespace atrium
