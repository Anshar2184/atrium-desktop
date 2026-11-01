#include "shell/radios.h"

#include "log.h"

#include <wbemidl.h>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <set>

#pragma comment(lib, "wbemuuid.lib")

namespace atrium {

using namespace winrt;
using namespace winrt::Windows::Devices;

RadioState QueryRadios()
{
    RadioState s;
    try {
        for (const Radios::Radio& r : Radios::Radio::GetRadiosAsync().get()) {
            bool on = r.State() == Radios::RadioState::On;
            if (r.Kind() == Radios::RadioKind::WiFi) {
                s.wifiPresent = true;
                s.wifiOn = s.wifiOn || on;
            } else if (r.Kind() == Radios::RadioKind::Bluetooth) {
                s.btPresent = true;
                s.btOn = s.btOn || on;
            }
        }
    } catch (...) {
        Log(L"radios: reading the radios failed");
    }
    return s;
}

bool SetRadio(bool bluetooth, bool on)
{
    try {
        if (Radios::Radio::RequestAccessAsync().get() != Radios::RadioAccessStatus::Allowed) {
            Log(L"radios: access to the radios was refused");
            return false;
        }
        bool ok = false;
        for (const Radios::Radio& r : Radios::Radio::GetRadiosAsync().get())
            if (r.Kind() == (bluetooth ? Radios::RadioKind::Bluetooth : Radios::RadioKind::WiFi))
                ok = r.SetStateAsync(on ? Radios::RadioState::On : Radios::RadioState::Off).get() == Radios::RadioAccessStatus::Allowed || ok;
        Log(L"radios: %s %s %s", bluetooth ? L"Bluetooth" : L"Wi-Fi", on ? L"on" : L"off", ok ? L"done" : L"refused");
        return ok;
    } catch (...) {
        Log(L"radios: switching the radio failed");
        return false;
    }
}

std::vector<BluetoothDevice> ListBluetooth()
{
    std::vector<BluetoothDevice> out;
    std::set<std::wstring> names;
    try {
        // Classic devices (headsets, keyboards, phones), then Bluetooth LE.
        for (const auto& d : Enumeration::DeviceInformation::FindAllAsync(Bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true)).get()) {
            BluetoothDevice b;
            b.name = std::wstring(d.Name());
            try {
                if (auto dev = Bluetooth::BluetoothDevice::FromIdAsync(d.Id()).get())
                    b.connected = dev.ConnectionStatus() == Bluetooth::BluetoothConnectionStatus::Connected;
            } catch (...) {
            }
            if (!b.name.empty() && names.insert(b.name).second)
                out.push_back(b);
        }
        for (const auto& d : Enumeration::DeviceInformation::FindAllAsync(Bluetooth::BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)).get()) {
            BluetoothDevice b;
            b.name = std::wstring(d.Name());
            b.lowEnergy = true;
            try {
                if (auto dev = Bluetooth::BluetoothLEDevice::FromIdAsync(d.Id()).get())
                    b.connected = dev.ConnectionStatus() == Bluetooth::BluetoothConnectionStatus::Connected;
            } catch (...) {
            }
            if (!b.name.empty() && names.insert(b.name).second)
                out.push_back(b);
        }
    } catch (...) {
        Log(L"radios: listing Bluetooth devices failed");
    }
    std::stable_sort(out.begin(), out.end(), [](const BluetoothDevice& a, const BluetoothDevice& b) { return a.connected > b.connected; });
    return out;
}

namespace {

// ROOT\WMI, where laptops publish the built-in screen's brightness.
com_ptr<IWbemServices> Wmi()
{
    com_ptr<IWbemLocator> locator;
    com_ptr<IWbemServices> services;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(locator.put()))))
        return nullptr;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.put());
    SysFreeString(ns);
    if (FAILED(hr))
        return nullptr;
    CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                      EOAC_NONE);
    return services;
}

// The first instance of a WMI class.
com_ptr<IWbemClassObject> First(IWbemServices* wmi, const wchar_t* query)
{
    com_ptr<IEnumWbemClassObject> e;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    HRESULT hr = wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, e.put());
    SysFreeString(lang);
    SysFreeString(q);
    com_ptr<IWbemClassObject> o;
    ULONG n = 0;
    if (FAILED(hr) || !e || FAILED(e->Next(2000, 1, o.put(), &n)) || n == 0)
        return nullptr;
    return o;
}

} // namespace

int GetBrightness()
{
    com_ptr<IWbemServices> wmi = Wmi();
    com_ptr<IWbemClassObject> o = wmi ? First(wmi.get(), L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE") : nullptr;
    if (!o)
        return -1;
    VARIANT v;
    VariantInit(&v);
    int out = -1;
    if (SUCCEEDED(o->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)) && (v.vt == VT_UI1 || v.vt == VT_I4))
        out = v.vt == VT_UI1 ? int(v.bVal) : int(v.lVal);
    VariantClear(&v);
    return out;
}

bool SetBrightness(int percent)
{
    com_ptr<IWbemServices> wmi = Wmi();
    com_ptr<IWbemClassObject> inst = wmi ? First(wmi.get(), L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE") : nullptr;
    if (!inst)
        return false;
    VARIANT path;
    VariantInit(&path);
    com_ptr<IWbemClassObject> cls, inDef, in;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods"), method = SysAllocString(L"WmiSetBrightness");
    bool ok = false;
    if (SUCCEEDED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR &&
        SUCCEEDED(wmi->GetObject(clsName, 0, nullptr, cls.put(), nullptr)) && SUCCEEDED(cls->GetMethod(method, 0, inDef.put(), nullptr)) && inDef &&
        SUCCEEDED(inDef->SpawnInstance(0, in.put()))) {
        VARIANT t, b;
        t.vt = VT_I4;
        t.lVal = 0;
        b.vt = VT_UI1;
        b.bVal = BYTE(std::clamp(percent, 0, 100));
        in->Put(L"Timeout", 0, &t, 0);
        in->Put(L"Brightness", 0, &b, 0);
        ok = SUCCEEDED(wmi->ExecMethod(path.bstrVal, method, 0, nullptr, in.get(), nullptr, nullptr));
    }
    VariantClear(&path);
    SysFreeString(clsName);
    SysFreeString(method);
    return ok;
}

} // namespace atrium
