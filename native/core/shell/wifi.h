// Wi-Fi through the native WLAN API (wlanapi): the current connection and its
// signal, the networks in range, connecting to a network that has a saved
// profile and disconnecting. Networks without a profile (a password to enter)
// are left to the system's network list. Calls block (scanning takes seconds):
// call them from a worker thread.
#pragma once

#include <string>
#include <vector>

namespace atrium {

struct WifiState {
    bool adapter = false;   // a Wi-Fi interface exists
    bool connected = false;
    std::wstring ssid;
    int signal = 0;         // 0..100
};

struct WifiNetwork {
    std::wstring ssid;
    std::wstring profile;   // saved profile name; empty: none
    int signal = 0;         // 0..100
    bool secured = false;
    bool connected = false;
};

WifiState QueryWifi();
// Networks in range, strongest first, one entry per SSID. rescan: ask the
// adapter for a fresh scan first (waits up to ~4 s).
std::vector<WifiNetwork> ListWifi(bool rescan);
bool ConnectWifi(const WifiNetwork& network);
bool DisconnectWifi();


} // namespace atrium
