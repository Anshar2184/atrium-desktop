#include "shell/bluetooth_batteries.h"

#include "log.h"

#include <cfgmgr32.h>
#include <initguid.h> // the DEVPKEY_ constants defined here
#include <devpkey.h>
#include <setupapi.h>

#include <algorithm>
#include <chrono>
#include <map>

namespace atrium {

namespace {

constexpr auto kEvery = std::chrono::seconds(30);

// The battery level Windows keeps for a Bluetooth device (a byte, 0..100): on the device of a
// Bluetooth LE device, on the hands-free service of a classic headset.
constexpr DEVPROPKEY kBatteryLevel = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
// Whether a Bluetooth device is connected now (a paired one keeps its last level while it is not).
constexpr DEVPROPKEY kConnected = {{0x83DA6326, 0x97A6, 0x4088, {0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29}}, 15};

struct Container {
    std::wstring name;
    bool device = false, connected = false;
    int percent = -1;
    std::wstring category;
    bool keyboard = false, mouse = false;
};

bool StartsWith(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

template <typename T>
bool Property(HDEVINFO set, SP_DEVINFO_DATA& d, const DEVPROPKEY& key, T& out)
{
    DEVPROPTYPE type = 0;
    return SetupDiGetDevicePropertyW(set, &d, &key, &type, reinterpret_cast<BYTE*>(&out), sizeof(T), nullptr, 0) != FALSE;
}

std::wstring Text(HDEVINFO set, SP_DEVINFO_DATA& d, const DEVPROPKEY& key)
{
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    SetupDiGetDevicePropertyW(set, &d, &key, &type, nullptr, 0, &size, 0);
    if (!size || (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_LIST))
        return {};
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (!SetupDiGetDevicePropertyW(set, &d, &key, &type, reinterpret_cast<BYTE*>(s.data()), size, nullptr, 0))
        return {};
    s.resize(wcslen(s.c_str())); // the first string of a list
    return s;
}

BluetoothKind KindOf(const Container& c)
{
    const std::wstring& k = c.category;
    if (StartsWith(k, L"Communication.Headset") || StartsWith(k, L"Multimedia.Headphone") || StartsWith(k, L"Multimedia.Speaker") ||
        k.find(L"Audio") != std::wstring::npos)
        return BluetoothKind::Headset;
    if (StartsWith(k, L"Input.Gaming"))
        return BluetoothKind::Gamepad;
    if (StartsWith(k, L"Input.Pen"))
        return BluetoothKind::Pen;
    if (StartsWith(k, L"Communication.Phone"))
        return BluetoothKind::Phone;
    if (StartsWith(k, L"Input.Keyboard") || c.keyboard)
        return BluetoothKind::Keyboard;
    if (StartsWith(k, L"Input.Mouse") || StartsWith(k, L"Input.Touchpad") || c.mouse)
        return BluetoothKind::Mouse;
    return BluetoothKind::Other;
}

} // namespace

std::vector<BluetoothBattery> ReadBluetoothBatteries()
{
    // Every device present, grouped by the physical thing it belongs to (its container): the
    // Bluetooth device itself gives the name and whether it is connected, any of its parts the
    // battery, its keyboard or mouse (HID) parts what it is.
    std::map<GUID, Container, decltype([](const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) < 0; })> containers;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return {};
    SP_DEVINFO_DATA d{sizeof(d)};
    wchar_t id[MAX_DEVICE_ID_LEN];
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        GUID container{};
        if (!Property(set, d, DEVPKEY_Device_ContainerId, container) || !SetupDiGetDeviceInstanceIdW(set, &d, id, MAX_DEVICE_ID_LEN, nullptr))
            continue;
        const std::wstring instance = id;
        const bool bluetooth = StartsWith(instance, L"BTHENUM\\") || StartsWith(instance, L"BTHLE\\") || StartsWith(instance, L"BTHLEDEVICE\\");
        if (!bluetooth && !StartsWith(instance, L"HID\\"))
            continue;
        Container& c = containers[container];
        if (bluetooth) {
            BYTE level = 0;
            if (Property(set, d, kBatteryLevel, level) && level <= 100)
                c.percent = std::max(c.percent, int(level));
        }
        if (StartsWith(instance, L"BTHENUM\\DEV_") || StartsWith(instance, L"BTHLE\\DEV_")) {
            c.device = true;
            DEVPROP_BOOLEAN on = DEVPROP_FALSE;
            c.connected = Property(set, d, kConnected, on) && on == DEVPROP_TRUE;
            c.name = Text(set, d, DEVPKEY_Device_FriendlyName);
            c.category = Text(set, d, DEVPKEY_DeviceContainer_Category);
        }
        const std::wstring cls = Text(set, d, DEVPKEY_Device_Class);
        c.keyboard |= cls == L"Keyboard";
        c.mouse |= cls == L"Mouse";
    }
    SetupDiDestroyDeviceInfoList(set);
    std::vector<BluetoothBattery> out;
    for (auto& [guid, c] : containers)
        if (c.device && c.connected && c.percent >= 0 && !c.name.empty())
            out.push_back({c.name, KindOf(c), c.percent});
    std::sort(out.begin(), out.end(), [](const BluetoothBattery& a, const BluetoothBattery& b) { return a.name < b.name; });
    return out;
}

BluetoothBatteries::BluetoothBatteries(HWND notify, UINT message) : notify_(notify), message_(message)
{
    thread_ = std::thread([this] { Run(); });
}

BluetoothBatteries::~BluetoothBatteries()
{
    {
        std::lock_guard g(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void BluetoothBatteries::SetActive(bool on)
{
    {
        std::lock_guard g(lock_);
        if (on == active_)
            return;
        active_ = on;
    }
    wake_.notify_all();
}

std::vector<BluetoothBattery> BluetoothBatteries::Latest()
{
    std::lock_guard g(lock_);
    return latest_;
}

bool BluetoothBatteries::Read()
{
    std::lock_guard g(lock_);
    return read_;
}

void BluetoothBatteries::Run()
{
    for (;;) {
        {
            std::unique_lock g(lock_);
            wake_.wait(g, [&] { return quit_ || active_; }); // nothing to read while no such widget shows
            if (quit_)
                return;
        }
        std::vector<BluetoothBattery> now;
        try {
            now = ReadBluetoothBatteries();
        } catch (...) {
            Log(L"bluetooth: reading the batteries failed");
        }
        bool changed;
        {
            std::lock_guard g(lock_);
            changed = !read_ || now.size() != latest_.size() ||
                      !std::equal(now.begin(), now.end(), latest_.begin(), [](const BluetoothBattery& a, const BluetoothBattery& b) {
                          return a.name == b.name && a.kind == b.kind && a.percent == b.percent;
                      });
            latest_ = std::move(now);
            read_ = true;
        }
        if (changed)
            PostMessageW(notify_, message_, 0, 0);
        std::unique_lock g(lock_);
        wake_.wait_for(g, kEvery, [&] { return quit_ || !active_; });
        if (quit_)
            return;
    }
}

} // namespace atrium
