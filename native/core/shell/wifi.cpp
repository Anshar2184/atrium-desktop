#include "shell/wifi.h"

#include "log.h"

#include <windows.h>
#include <wlanapi.h>

#include <algorithm>
#include <map>

#pragma comment(lib, "wlanapi.lib")

namespace atrium {

namespace {

// One client handle per call; the API is cheap to open and this keeps every
// function usable from any thread.
class Wlan {
public:
    Wlan()
    {
        DWORD version = 0;
        if (WlanOpenHandle(2, nullptr, &version, &handle_) != ERROR_SUCCESS)
            handle_ = nullptr;
    }
    ~Wlan()
    {
        if (list_)
            WlanFreeMemory(list_);
        if (handle_)
            WlanCloseHandle(handle_, nullptr);
    }
    Wlan(const Wlan&) = delete;
    Wlan& operator=(const Wlan&) = delete;
    HANDLE Handle() const { return handle_; }
    // The first Wi-Fi interface (laptops have one).
    const GUID* Interface()
    {
        if (!handle_)
            return nullptr;
        if (!list_ && WlanEnumInterfaces(handle_, nullptr, &list_) != ERROR_SUCCESS)
            return nullptr;
        return list_ && list_->dwNumberOfItems ? &list_->InterfaceInfo[0].InterfaceGuid : nullptr;
    }

private:
    HANDLE handle_ = nullptr;
    WLAN_INTERFACE_INFO_LIST* list_ = nullptr;
};

std::wstring Ssid(const DOT11_SSID& s)
{
    std::string bytes(reinterpret_cast<const char*>(s.ucSSID), std::min<ULONG>(s.uSSIDLength, DOT11_SSID_MAX_LENGTH));
    // SSIDs are bytes; nearly always UTF-8.
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
    UINT cp = n > 0 ? CP_UTF8 : CP_ACP;
    n = MultiByteToWideChar(cp, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring out(size_t(std::max(n, 0)), L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), int(bytes.size()), out.data(), n);
    return out;
}

} // namespace

int WifiBars(int signal) { return signal >= 75 ? 4 : signal >= 50 ? 3 : signal >= 25 ? 2 : 1; }

WifiState QueryWifi()
{
    WifiState s;
    Wlan w;
    const GUID* itf = w.Interface();
    if (!itf)
        return s;
    s.adapter = true;
    DWORD size = 0;
    WLAN_CONNECTION_ATTRIBUTES* conn = nullptr;
    if (WlanQueryInterface(w.Handle(), itf, wlan_intf_opcode_current_connection, nullptr, &size, reinterpret_cast<void**>(&conn), nullptr) ==
            ERROR_SUCCESS &&
        conn) {
        if (conn->isState == wlan_interface_state_connected) {
            s.connected = true;
            s.ssid = Ssid(conn->wlanAssociationAttributes.dot11Ssid);
            s.signal = int(conn->wlanAssociationAttributes.wlanSignalQuality);
        }
        WlanFreeMemory(conn);
    }
    return s;
}

std::vector<WifiNetwork> ListWifi(bool rescan)
{
    std::vector<WifiNetwork> out;
    Wlan w;
    const GUID* itf = w.Interface();
    if (!itf)
        return out;
    if (rescan && WlanScan(w.Handle(), itf, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        Sleep(3500); // results arrive within about 4 s (no notification needed for a list)
    WLAN_AVAILABLE_NETWORK_LIST* list = nullptr;
    if (WlanGetAvailableNetworkList(w.Handle(), itf, 0, nullptr, &list) != ERROR_SUCCESS || !list)
        return out;
    std::map<std::wstring, WifiNetwork> bySsid;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_AVAILABLE_NETWORK& a = list->Network[i];
        WifiNetwork n;
        n.ssid = Ssid(a.dot11Ssid);
        if (n.ssid.empty())
            continue; // hidden network
        n.signal = int(a.wlanSignalQuality);
        n.secured = a.bSecurityEnabled != FALSE;
        n.connected = (a.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
        if (a.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE)
            n.profile = a.strProfileName;
        // The list has one entry per profile and one without: merge by SSID.
        WifiNetwork& m = bySsid[n.ssid];
        if (m.ssid.empty()) {
            m = n;
        } else {
            m.signal = std::max(m.signal, n.signal);
            m.connected |= n.connected;
            if (m.profile.empty())
                m.profile = n.profile;
        }
    }
    WlanFreeMemory(list);
    for (auto& [ssid, n] : bySsid)
        out.push_back(n);
    std::sort(out.begin(), out.end(), [](const WifiNetwork& a, const WifiNetwork& b) {
        return a.connected != b.connected ? a.connected : a.signal > b.signal;
    });
    return out;
}

bool ConnectWifi(const WifiNetwork& network)
{
    if (network.profile.empty())
        return false;
    Wlan w;
    const GUID* itf = w.Interface();
    if (!itf)
        return false;
    WLAN_CONNECTION_PARAMETERS p{};
    p.wlanConnectionMode = wlan_connection_mode_profile;
    p.strProfile = network.profile.c_str();
    p.dot11BssType = dot11_BSS_type_any;
    DWORD r = WlanConnect(w.Handle(), itf, &p, nullptr);
    Log(L"wifi: connect to a saved network: %lu", r);
    return r == ERROR_SUCCESS;
}

bool DisconnectWifi()
{
    Wlan w;
    const GUID* itf = w.Interface();
    DWORD r = itf ? WlanDisconnect(w.Handle(), itf, nullptr) : ERROR_NOT_FOUND;
    Log(L"wifi: disconnect: %lu", r);
    return r == ERROR_SUCCESS;
}

} // namespace atrium
