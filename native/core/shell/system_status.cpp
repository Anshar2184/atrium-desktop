#include "shell/system_status.h"

#include "log.h"
#include "shell/wifi.h"

#include <windows.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <netlistmgr.h>
#include <powrprof.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "powrprof.lib")

namespace atrium {

namespace {

winrt::com_ptr<IAudioEndpointVolume> DefaultVolume(std::wstring* name = nullptr)
{
    winrt::com_ptr<IMMDeviceEnumerator> devices;
    winrt::com_ptr<IMMDevice> device;
    winrt::com_ptr<IAudioEndpointVolume> volume;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(devices.put()))) ||
        FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put())) ||
        FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, volume.put_void())))
        return nullptr;
    winrt::com_ptr<IPropertyStore> props;
    if (name && SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put()))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            *name = v.pwszVal;
        PropVariantClear(&v);
    }
    return volume;
}

} // namespace

SystemStatus QuerySystemStatus()
{
    SystemStatus s;
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        // 128: no system battery; 255: unknown.
        s.battery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100;
        s.batteryPercent = s.battery ? ps.BatteryLifePercent : -1;
        s.charging = ps.ACLineStatus == 1;
        s.batteryMinutes = s.battery && !s.charging && ps.BatteryLifeTime != DWORD(-1) ? int(ps.BatteryLifeTime / 60) : -1;
    }
    try {
        if (auto v = DefaultVolume(&s.audioDevice)) {
            float level = 0;
            BOOL mute = FALSE;
            v->GetMasterVolumeLevelScalar(&level);
            v->GetMute(&mute);
            s.audio = true;
            s.volume = int(std::lround(level * 100));
            s.muted = mute != FALSE;
        }
        winrt::com_ptr<INetworkListManager> nlm;
        if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(nlm.put())))) {
            NLM_CONNECTIVITY c{};
            if (SUCCEEDED(nlm->GetConnectivity(&c))) {
                s.online = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
                s.connected = c != NLM_CONNECTIVITY_DISCONNECTED;
            }
            // The network that carries the internet connection (a VPN or a
            // virtual adapter is often connected too, without internet).
            winrt::com_ptr<IEnumNetworks> nets;
            if (SUCCEEDED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, nets.put()))) {
                std::wstring first;
                for (;;) {
                    winrt::com_ptr<INetwork> net;
                    if (nets->Next(1, net.put(), nullptr) != S_OK)
                        break;
                    BSTR name = nullptr;
                    std::wstring n;
                    if (SUCCEEDED(net->GetName(&name)) && name)
                        n = name;
                    SysFreeString(name);
                    NLM_CONNECTIVITY nc{};
                    net->GetConnectivity(&nc);
                    if (first.empty())
                        first = n;
                    if (nc & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) {
                        s.network = n;
                        break;
                    }
                }
                if (s.network.empty())
                    s.network = first;
            }
        }
        // Wi-Fi: the signal, and its SSID where the WLAN API gives it out
        // (Windows 11 may withhold it from apps without location access).
        WifiState w = QueryWifi();
        if (w.connected && s.connected) {
            s.wifi = true;
            s.signal = w.signal;
            if (!w.ssid.empty() && (s.network.empty() || !s.online))
                s.network = w.ssid;
        }
    } catch (...) {
        Log(L"status: query failed");
    }
    return s;
}

int AdjustVolume(int delta)
{
    auto v = DefaultVolume();
    if (!v)
        return -1;
    float level = 0;
    v->GetMasterVolumeLevelScalar(&level);
    level = std::clamp(level + float(delta) / 100.f, 0.f, 1.f);
    v->SetMasterVolumeLevelScalar(level, nullptr);
    if (delta > 0)
        v->SetMute(FALSE, nullptr);
    return int(std::lround(level * 100));
}

void SetVolume(int percent)
{
    if (auto v = DefaultVolume()) {
        v->SetMasterVolumeLevelScalar(float(std::clamp(percent, 0, 100)) / 100.f, nullptr);
        if (percent > 0)
            v->SetMute(FALSE, nullptr);
    }
}

bool ToggleMute()
{
    auto v = DefaultVolume();
    BOOL mute = FALSE;
    if (!v || FAILED(v->GetMute(&mute)))
        return false;
    v->SetMute(!mute, nullptr);
    return true;
}

bool RunPowerAction(PowerAction action)
{
    const wchar_t* names[] = {L"sleep", L"restart", L"shutdown"};
    wchar_t dry[8] = {};
    if (GetEnvironmentVariableW(L"ATRIUM_POWER_DRYRUN", dry, 8) && dry[0] == L'1') {
        Log(L"power: %s (dry run)", names[int(action)]);
        return true;
    }
    Log(L"power: %s", names[int(action)]);
    if (action == PowerAction::Sleep)
        return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
    // Shutting down needs SeShutdownPrivilege enabled in the token (every
    // interactive user has it, disabled by default).
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    UINT flags = action == PowerAction::Restart ? EWX_REBOOT : EWX_SHUTDOWN | EWX_POWEROFF;
    bool ok = ExitWindowsEx(flags, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED) != FALSE;
    if (!ok)
        Log(L"power: ExitWindowsEx failed (%lu)", GetLastError());
    return ok;
}

} // namespace atrium
