// The batteries of the Bluetooth devices connected now, for the Bluetooth widget: the level
// Windows keeps for a device (what Settings > Bluetooth & devices shows), read with SetupAPI
// on a worker thread every 30 seconds while such a widget is on the desktop.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

enum class BluetoothKind { Other, Headset, Gamepad, Keyboard, Mouse, Pen, Phone };

struct BluetoothBattery {
    std::wstring name;
    BluetoothKind kind = BluetoothKind::Other;
    int percent = 0;
};

class BluetoothBatteries {
public:
    // `message` is posted to `notify` when Latest() changes.
    BluetoothBatteries(HWND notify, UINT message);
    ~BluetoothBatteries();
    BluetoothBatteries(const BluetoothBatteries&) = delete;
    BluetoothBatteries& operator=(const BluetoothBatteries&) = delete;

    void SetActive(bool on);
    std::vector<BluetoothBattery> Latest();
    bool Read(); // false until the first reading

private:
    void Run();

    HWND notify_;
    UINT message_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool active_ = false, quit_ = false, read_ = false;
    std::vector<BluetoothBattery> latest_;
    std::thread thread_;
};

// The connected devices that report a battery, by name.
std::vector<BluetoothBattery> ReadBluetoothBatteries();

} // namespace atrium
