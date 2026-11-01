// The control centre, opened from the bar's status ring, after macOS's: small
// pieces of the bar's glass. Top left the connections (Wi-Fi and Bluetooth,
// each a round switch and its state); top right the battery and screen
// casting; below, sliders for the screen's brightness and the sound. The
// connections open their own page in place: the Wi-Fi networks in range
// (connecting to saved ones) or the paired Bluetooth devices. Drawn by
// Atrium; only the system's dialogs and settings pages are left to Windows.
#pragma once

#include "shell/radios.h"
#include "shell/system_status.h"
#include "shell/wifi.h"
#include "ui/surface.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace atrium {

class ControlCenter {
public:
    explicit ControlCenter(HINSTANCE instance);

    // anchor: the status ring on screen; bar: the bar's glass shape.
    void Open(RECT anchor, RECT bar);
    void Close();
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void SetRadius(float dip) { radius_ = dip; surface_.SetRadius(dip); }
    void SetBarHeight(float dip) { barH_ = dip; }
    HWND Hwnd() const { return surface_.Hwnd(); }
    // Test hooks: log what is shown (screen px); act as a click on `id`.
    void LogState() const;
    void TestClick(const std::wstring& id);

    std::function<void()> onChanged; // volume, mute or the network changed: the bar's icon updates

private:
    enum class Page { Main, Wifi, Bluetooth };
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    void Paint(Canvas& c);
    void PaintMain(Canvas& c, const Theme& t, std::vector<ShapeDip>& shapes);
    void PaintWifi(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintBluetooth(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintSlider(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& id, const std::wstring& title, const std::wstring& glyph,
                     int value, bool dim, const std::wstring& right, D2D1_RECT_F& track);
    float PaintHeader(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& title, bool on, const std::wstring& toggleId);
    void Switch(Canvas& c, const Theme& t, D2D1_RECT_F r, bool on, float hv);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::wstring HitAt(POINT client) const;
    void Activate(const std::wstring& id);
    void Scan(bool rescan);
    void ReadRadios(bool devices);
    void SetRadioAsync(bool bluetooth, bool on);
    void Place();
    void ShowPage(Page page);
    void SetSliderAt(const std::wstring& id, float x);
    float Hover(Canvas& c, const std::wstring& id, bool on);
    float MainHeight() const;
    float PageHeight() const;
    D2D1_SIZE_F Size() const;

    Surface surface_;
    SystemStatus status_;
    RadioState radios_;
    bool radiosRead_ = false;
    bool brightRead_ = false; // the brightness, once per opening (WMI is slow)
    std::vector<BluetoothDevice> devices_;
    bool devicesRead_ = false;
    int brightness_ = -1; // -1: not adjustable (no built-in screen), -2: not read yet
    std::vector<WifiNetwork> networks_;
    bool scanning_ = false, scanned_ = false;
    unsigned scanGen_ = 0, acceptedGen_ = 0;
    Page page_ = Page::Main;
    double pageMs_ = 0;
    int pageDir_ = 0;
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_, dragging_;
    bool tracking_ = false;
    D2D1_RECT_F volumeTrack_{}, brightTrack_{}; // the sliders' tracks, DIP
    RECT anchor_{}, bar_{};
    float radius_ = 30, barH_ = 60;
    ULONGLONG lastClosed_ = 0;
    bool private_ = false;
    std::map<std::wstring, std::pair<float, double>> hoverAnim_;
    int brightPending_ = -1; // a brightness the worker has yet to set
    bool brightBusy_ = false;
};

} // namespace atrium
