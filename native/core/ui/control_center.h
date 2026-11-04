// The control centre, opened from the bar's status ring, after macOS's: small
// pieces of the bar's glass. Top left the connections (Wi-Fi and Bluetooth,
// each a round switch and its state); top right the battery and screen
// casting; below, sliders for the screen's brightness and the sound. The
// connections open their own page in place: the Wi-Fi networks in range
// (connecting to saved ones) or the paired Bluetooth devices; so do the sound
// (the output device and each app's volume, as on the Windows taskbar) and
// casting (wireless displays and the projection modes of Win+P). Drawn by
// Atrium; only the system's dialogs and settings pages are left to Windows.
#pragma once

#include "motion.h"
#include "shell/audio_devices.h"
#include "shell/projection.h"
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
    // Windows reported the built-in screen's brightness (keys, quick settings).
    void BrightnessChanged(int percent);

    std::function<void()> onChanged; // volume, mute or the network changed: the bar's icon updates

private:
    enum class Page { Main, Wifi, Bluetooth, Sound, Cast };
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    void Paint(Canvas& c);
    void PaintMain(Canvas& c, const Theme& t);
    void PaintWifi(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintBluetooth(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintSound(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintCast(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void Link(Canvas& c, const Theme& t, const std::wstring& id, const std::wstring& label, float left, float right, float y, int align);
    void PaintSlider(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& id, const std::wstring& title, const std::wstring& glyph,
                     int value, bool dim, const std::wstring& right, D2D1_RECT_F& track);
    float PaintHeader(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& title, bool on, const std::wstring& toggleId);
    void Switch(Canvas& c, const Theme& t, D2D1_RECT_F r, bool on, float hv);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::wstring HitAt(POINT client) const;
    void Activate(const std::wstring& id);
    void Scan(bool rescan);
    void ReadRadios(bool devices);
    void ReadAudio(bool icons);
    void ReadMode();
    void SetRadioAsync(bool bluetooth, bool on);
    void Place();
    void ShowPage(Page page);
    void SetSliderAt(const std::wstring& id, float x);
    float Hover(Canvas& c, const std::wstring& id, bool on);
    float MainHeight() const;
    float PageHeight(Page page) const;
    float HeightOf(Page page) const { return page == Page::Main ? MainHeight() : PageHeight(page); }
    float MaxHeight() const;
    std::vector<ShapeDip> MainShapes() const; // the main page's pieces of glass, from its top
    void PaintPage(Canvas& c, const Theme& t, Page page);
    D2D1_SIZE_F Size() const; // the window's: as tall as the tallest page, the glass standing on its bottom

    Surface surface_;
    SystemStatus status_;
    RadioState radios_;
    bool radiosRead_ = false;
    bool brightRead_ = false; // the brightness, once per opening (WMI is slow)
    std::vector<BluetoothDevice> devices_;
    bool devicesRead_ = false;
    int brightness_ = -2; // -1: not adjustable (no built-in screen), -2: not read yet
    // The display's slider: read, or (before the first reading) a laptop's built-in screen, so the
    // first opening is laid out at once instead of the sound's slider moving down when WMI answers.
    bool HasBrightness() const { return brightness_ >= 0 || (brightness_ == -2 && status_.battery); }
    std::vector<WifiNetwork> networks_;
    bool scanning_ = false, scanned_ = false;
    unsigned scanGen_ = 0, acceptedGen_ = 0;
    Page page_ = Page::Main;
    double pageMs_ = 0;
    int pageDir_ = 0;
    Page fromPage_ = Page::Main; // the page leaving while the new one comes in
    // The glass's height glides between pages (and when a page's rows change);
    // the window keeps its size, so nothing jumps while it does.
    float shownH_ = 0, fromH_ = 0, toH_ = 0;
    double heightMs_ = 0;
    float cardTop_ = 0;     // DIP from the window's top: where the glass begins; the content hangs from it
    int regionTop_ = -1;    // the window region's top (px): clicks above the glass go to what is under it
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_, dragging_;
    bool tracking_ = false;
    int downX_ = 0; // where a press on the volume's symbol began: moving off it drags the slider
    D2D1_RECT_F volumeTrack_{}, brightTrack_{}; // the sliders' tracks, DIP
    RECT anchor_{}, bar_{};
    float radius_ = 30, barH_ = 60;
    ULONGLONG lastClosed_ = 0;
    bool private_ = false;
    std::map<std::wstring, motion::Follow> hoverAnim_;
    int brightPending_ = -1; // a brightness the worker has yet to set
    bool brightBusy_ = false;
    // Sound output page.
    std::vector<AudioOutput> outputs_;
    std::vector<AudioApp> apps_;
    bool audioRead_ = false;
    std::vector<std::wstring> iconTried_;  // apps whose icon was asked for
    std::vector<D2D1_RECT_F> appTracks_;   // the apps' sliders, DIP
    // Cast page.
    DisplayFinder finder_;
    std::vector<WirelessDisplay> displays_;
    std::wstring projecting_, connecting_; // display ids
    ProjectionMode mode_ = ProjectionMode::Unknown;
    int monitors_ = 1;
};

} // namespace atrium
