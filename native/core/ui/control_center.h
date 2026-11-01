// The control centre, opened from the bar's status icon: separate pieces of the
// bar's glass for the network (the connection, the Wi-Fi networks in range,
// connecting to saved ones), the volume (a slider and mute, shaped like the
// bar) and the battery. Drawn by Atrium; only the system's own dialogs (a
// password for a new network, the settings pages) are left to Windows.
#pragma once

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

    // anchor: the status icon on screen; bar: the bar's glass shape.
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
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    void Paint(Canvas& c);
    void PaintNetwork(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintVolume(Canvas& c, const Theme& t, D2D1_RECT_F cap);
    void PaintBattery(Canvas& c, const Theme& t, D2D1_RECT_F cap);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::wstring HitAt(POINT client) const;
    void Activate(const std::wstring& id);
    void Scan(bool rescan);
    void Place();
    void SetVolumeAt(float x);
    float Hover(Canvas& c, const std::wstring& id, bool on);
    int Rows() const;
    float NetworkHeight() const;
    D2D1_SIZE_F Size() const;

    Surface surface_;
    SystemStatus status_;
    std::vector<WifiNetwork> networks_;
    bool scanning_ = false, scanned_ = false;
    unsigned scanGen_ = 0, acceptedGen_ = 0;
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_;
    bool tracking_ = false, dragging_ = false;
    D2D1_RECT_F track_{}; // the volume slider's track, DIP
    RECT anchor_{}, bar_{};
    float radius_ = 30, barH_ = 60;
    ULONGLONG lastClosed_ = 0;
    bool private_ = false;
    std::map<std::wstring, std::pair<float, double>> hoverAnim_;
    double openedMs_ = 0;
};

} // namespace atrium
