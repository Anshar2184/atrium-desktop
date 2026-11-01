// Wi-Fi and Bluetooth radios, paired Bluetooth devices and the screen's
// brightness for the control centre, through public APIs
// (Windows.Devices.Radios, Windows.Devices.Enumeration / Bluetooth, WMI).
// Every call blocks: run them on a worker thread (MTA), never on the UI thread.
#pragma once

#include <string>
#include <vector>

namespace atrium {

struct RadioState {
    bool wifiPresent = false, wifiOn = false;
    bool btPresent = false, btOn = false;
    bool operator==(const RadioState&) const = default;
};

struct BluetoothDevice {
    std::wstring name;
    bool connected = false;
    bool lowEnergy = false;
};

RadioState QueryRadios();
// Turns the Wi-Fi or Bluetooth radio on or off; false if Windows refused.
bool SetRadio(bool bluetooth, bool on);
// Paired devices, connected ones first.
std::vector<BluetoothDevice> ListBluetooth();

// The built-in screen's brightness 0..100, or -1 where it cannot be set (WMI).
int GetBrightness();
bool SetBrightness(int percent);

} // namespace atrium
