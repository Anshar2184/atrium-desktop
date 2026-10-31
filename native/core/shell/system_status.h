// Battery, volume and network state for the start menu's footer, through
// public APIs only (GetSystemPowerStatus, Core Audio, Network List Manager),
// and the power actions.
#pragma once

#include <string>

namespace atrium {

struct SystemStatus {
    bool battery = false;   // a system battery is present
    int batteryPercent = -1;
    bool charging = false;
    bool audio = false;     // a default playback device exists
    int volume = 0;         // 0..100
    bool muted = false;
    bool online = false;    // internet connectivity
    bool connected = false; // any network
    std::wstring network;   // name of the connected network
};

SystemStatus QuerySystemStatus();
// Changes the default playback volume by `delta` percent points; returns the new level.
int AdjustVolume(int delta);
bool ToggleMute();

enum class PowerAction { Sleep, Restart, Shutdown };
// ATRIUM_POWER_DRYRUN=1 only logs the action (tests).
bool RunPowerAction(PowerAction action);

} // namespace atrium
