// Battery, volume and network state for the bar's status icon, the control
// centre and the start menu, through public APIs only (GetSystemPowerStatus,
// Core Audio, Network List Manager, WLAN), and the power actions.
#pragma once

#include <windows.h>

#include <string>

namespace atrium {

struct SystemStatus {
    bool battery = false;   // a system battery is present
    int batteryPercent = -1;
    bool charging = false;  // on mains power
    int batteryMinutes = -1; // estimated time left on battery; -1 unknown
    bool audio = false;     // a default playback device exists
    int volume = 0;         // 0..100
    bool muted = false;
    std::wstring audioDevice; // its name
    bool online = false;    // internet connectivity
    bool connected = false; // any network
    std::wstring network;   // name of the connected network
    bool wifi = false;      // the connection is Wi-Fi (else wired or other)
    int signal = 0;         // Wi-Fi signal 0..100
    int brightness = -1;    // the built-in screen 0..100 (the power notifications); -1 unknown
    bool operator==(const SystemStatus&) const = default;
};

SystemStatus QuerySystemStatus();
// Changes the default playback volume by `delta` percent points; returns the new level.
int AdjustVolume(int delta);
void SetVolume(int percent); // 0..100; unmutes above 0
bool ToggleMute();
// Posts `msg` to `hwnd` whenever the default playback device's volume or mute
// changes (volume keys, other programs). Call again after the default device
// changed: it follows the current one. Call on an STA/MTA thread with COM.
void WatchVolume(HWND hwnd, UINT msg);

enum class PowerAction { Sleep, Restart, Shutdown };
bool RunPowerAction(PowerAction action);

} // namespace atrium
