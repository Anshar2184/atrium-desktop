#include "ui/control_center.h"

#include "clock.h"
#include "log.h"

#include <shellapi.h>
#include <windowsx.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

namespace atrium {

namespace {

constexpr float kW = 340, kGap = 10, kTopH = 150, kSliderH = 68, kHeader = 58, kRowH = 44, kFooter = 46, kPad = 14;
constexpr float kSection = 30, kAppRowH = 50, kModeH = 78;
constexpr int kMaxRows = 6, kMaxOutputs = 4, kMaxApps = 5, kMaxDisplays = 4;
constexpr UINT_PTR kStatusTimer = 1;
constexpr double kPageMs = 240;
constexpr UINT WM_WIFI_LIST = WM_APP + 0x50;  // wParam generation, lParam std::vector<WifiNetwork>*
constexpr UINT WM_WIFI_AGAIN = WM_APP + 0x51; // after connecting: read the state and the list again
constexpr UINT WM_CC_RADIOS = WM_APP + 0x52;  // lParam RadioRead*
constexpr UINT WM_CC_BRIGHT = WM_APP + 0x53;  // the brightness worker is done (wParam: the value it set last)
constexpr UINT WM_CC_AUDIO = WM_APP + 0x54;   // lParam AudioRead*
constexpr UINT WM_CC_DISPLAYS = WM_APP + 0x55; // the wireless displays in range changed
constexpr UINT WM_CC_PROJECTED = WM_APP + 0x56; // wParam 1 projecting, 0 refused; lParam std::wstring* the display (null: stopped)

struct RadioRead {
    RadioState radios;
    bool devicesRead = false;
    std::vector<BluetoothDevice> devices;
    int brightness = -3; // -3: not read this time
};

struct AudioRead {
    std::vector<AudioOutput> outputs;
    std::vector<AudioApp> apps;
    bool icons = false;
};

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
float Ease(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

void OpenUri(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

// Win+K: the system's casting panel (a public shortcut, D11).
void SendWinK()
{
    INPUT in[4] = {};
    for (auto& i : in)
        i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].ki.wVk = 'K';
    in[2].ki.wVk = 'K';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// Signal as four rising bars.
void Bars(Canvas& c, float x, float cy, int signal, D2D1_COLOR_F ink)
{
    int lit = WifiBars(signal);
    for (int i = 0; i < 4; ++i) {
        float h = 4.f + 3.f * float(i);
        D2D1_RECT_F b = D2D1::RectF(x + float(i) * 4.5f, cy + 6 - h, x + float(i) * 4.5f + 3, cy + 6);
        c.Fill(b, 1.2f, i < lit ? ink : Alpha(ink, 0.28f));
    }
}

// Runs `work` on a thread of its own with a multithreaded COM apartment (WinRT, WMI).
template <class F> void OnWorker(F work)
{
    std::thread([work] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        work();
        winrt::uninit_apartment();
    }).detach();
}

} // namespace

ControlCenter::ControlCenter(HINSTANCE instance) : surface_(instance, L"Atrium Control Center", true)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onMargin = [this] { Close(); };
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
}

float ControlCenter::MainHeight() const { return kTopH + kGap + kSliderH + (brightness_ >= 0 ? kGap + kSliderH : 0); }

float ControlCenter::PageHeight() const
{
    if (page_ == Page::Sound) {
        int outs = std::clamp(int(outputs_.size()), 1, kMaxOutputs), apps = std::clamp(int(apps_.size()), 1, kMaxApps);
        return kHeader + float(outs) * kRowH + kSection + float(apps) * kAppRowH + kFooter + 6;
    }
    if (page_ == Page::Cast)
        return kHeader + float(std::clamp(int(displays_.size()), 1, kMaxDisplays)) * kRowH + kSection + kModeH + kFooter + 6;
    size_t n = page_ == Page::Wifi ? networks_.size() : devices_.size();
    bool off = page_ == Page::Wifi ? radiosRead_ && !radios_.wifiOn : !radios_.btOn;
    int rows = off ? 1 : std::clamp(int(n), 1, kMaxRows);
    return kHeader + float(rows) * kRowH + kFooter + 6;
}

D2D1_SIZE_F ControlCenter::Size() const { return D2D1::SizeF(kW, page_ == Page::Main ? MainHeight() : PageHeight()); }

void ControlCenter::Place()
{
    RECT r = PopupRect(bar_, Size(), PopupAlign::Right);
    if (surface_.Visible())
        surface_.Move(r);
    else
        surface_.Show(r, true, POINT{(anchor_.left + anchor_.right) / 2, r.bottom});
}

void ControlCenter::Open(RECT anchor, RECT bar)
{
    anchor_ = anchor;
    bar_ = bar;
    hover_.clear();
    pressed_.clear();
    dragging_.clear();
    hoverAnim_.clear();
    page_ = Page::Main;
    pageMs_ = ClockMs();
    pageDir_ = 0;
    brightRead_ = false; // changed meanwhile with the keyboard or in Settings
    status_ = QuerySystemStatus();
    Place();
    ReadRadios(true);
    Scan(false); // what the adapter already knows, ready for the Wi-Fi page
    SetTimer(surface_.Hwnd(), kStatusTimer, 1500, nullptr);
    Log(L"control: opened");
}

void ControlCenter::Close()
{
    if (!surface_.Visible())
        return;
    KillTimer(surface_.Hwnd(), kStatusTimer);
    finder_.Stop();
    if (!dragging_.empty())
        ReleaseCapture();
    dragging_.clear();
    surface_.Hide();
    lastClosed_ = GetTickCount64();
}

void ControlCenter::ShowPage(Page page)
{
    if (page == page_)
        return;
    if (page_ == Page::Cast)
        finder_.Stop();
    pageDir_ = page == Page::Main ? -1 : 1;
    page_ = page;
    pageMs_ = ClockMs();
    hover_.clear();
    Place();
    surface_.Invalidate();
}

void ControlCenter::Scan(bool rescan)
{
    scanning_ = true;
    HWND h = surface_.Hwnd();
    unsigned gen = ++scanGen_;
    std::thread([h, rescan, gen] {
        auto* list = new std::vector<WifiNetwork>(ListWifi(rescan));
        if (!PostMessageW(h, WM_WIFI_LIST, gen, reinterpret_cast<LPARAM>(list)))
            delete list;
    }).detach();
}

void ControlCenter::ReadRadios(bool devices)
{
    HWND h = surface_.Hwnd();
    bool bright = !brightRead_;
    brightRead_ = true;
    OnWorker([h, devices, bright] {
        auto* r = new RadioRead();
        r->radios = QueryRadios();
        if (devices && r->radios.btOn) {
            r->devices = ListBluetooth();
            r->devicesRead = true;
        }
        if (bright)
            r->brightness = GetBrightness();
        if (!PostMessageW(h, WM_CC_RADIOS, 0, reinterpret_cast<LPARAM>(r)))
            delete r;
    });
}

void ControlCenter::SetRadioAsync(bool bluetooth, bool on)
{
    // Shown at once; the next reading confirms it (or puts it back).
    (bluetooth ? radios_.btOn : radios_.wifiOn) = on;
    surface_.Invalidate();
    HWND h = surface_.Hwnd();
    OnWorker([h, bluetooth, on] {
        SetRadio(bluetooth, on);
        Sleep(bluetooth ? 900 : 1500);
        auto* r = new RadioRead();
        r->radios = QueryRadios();
        if (r->radios.btOn) {
            r->devices = ListBluetooth();
            r->devicesRead = true;
        }
        if (!PostMessageW(h, WM_CC_RADIOS, 0, reinterpret_cast<LPARAM>(r)))
            delete r;
        if (!bluetooth)
            PostMessageW(h, WM_WIFI_AGAIN, 0, 0);
    });
}

void ControlCenter::ReadAudio(bool icons)
{
    HWND h = surface_.Hwnd();
    int px = int(std::lround(26 * surface_.Scale()));
    OnWorker([h, px, icons] {
        auto* r = new AudioRead();
        r->outputs = ListAudioOutputs();
        r->apps = ListAudioApps(icons ? px : 0);
        r->icons = icons;
        if (!PostMessageW(h, WM_CC_AUDIO, 0, reinterpret_cast<LPARAM>(r)))
            delete r;
    });
}

void ControlCenter::ReadMode()
{
    ProjectionMode m = CurrentProjectionMode(&monitors_);
    if (m != mode_) {
        mode_ = m;
        surface_.Invalidate();
    }
}

float ControlCenter::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& [v, last] = hoverAnim_[id];
    double dt = last > 0 ? std::clamp(c.now - last, 0.0, 100.0) : 0;
    last = c.now;
    float target = on ? 1.f : 0.f;
    v += (target - v) * (1 - std::exp(-float(dt) / 1000.f * (on ? 20.f : 11.f)));
    if (std::fabs(target - v) < 0.01f)
        v = target;
    else
        c.Continue();
    return v;
}

void ControlCenter::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    hits_.clear();
    std::vector<ShapeDip> shapes;
    float e = pageDir_ ? Ease(float((c.now - pageMs_) / kPageMs)) : 1.f;
    if (e < 1)
        c.Continue();
    const float h = Size().height;
    // A new page slides in from the side it comes from.
    c.BeginGroup(e, 1, kW / 2, h / 2, float(pageDir_) * 22 * (1 - e), 0);
    if (page_ == Page::Main) {
        PaintMain(c, t, shapes);
    } else {
        D2D1_RECT_F card = D2D1::RectF(0, 0, kW, h);
        shapes.push_back({card, radius_});
        if (c.panel)
            c.Fill(card, radius_, t.panel);
        if (page_ == Page::Wifi)
            PaintWifi(c, t, card);
        else if (page_ == Page::Bluetooth)
            PaintBluetooth(c, t, card);
        else if (page_ == Page::Sound)
            PaintSound(c, t, card);
        else
            PaintCast(c, t, card);
    }
    c.EndGroup();
    surface_.SetShapes(shapes);
}

void ControlCenter::Switch(Canvas& c, const Theme& t, D2D1_RECT_F r, bool on, float hv)
{
    float hh = (r.bottom - r.top) / 2;
    c.Fill(r, hh, on ? t.accent : Alpha(t.text, 0.22f + 0.08f * hv));
    float x = on ? r.right - hh : r.left + hh, cy = (r.top + r.bottom) / 2;
    c.Circle(x, cy + 0.5f, hh - 2, D2D1::ColorF(0, 0, 0, 0.12f));
    c.Circle(x, cy, hh - 2.5f, D2D1::ColorF(1, 1, 1, 0.98f));
}

void ControlCenter::PaintMain(Canvas& c, const Theme& t, std::vector<ShapeDip>& shapes)
{
    const float modR = std::min(radius_, 22.f), colW = (kW - kGap) / 2;
    const D2D1_RECT_F conn = D2D1::RectF(0, 0, colW, kTopH);
    const D2D1_RECT_F batt = D2D1::RectF(colW + kGap, 0, kW, (kTopH - kGap) / 2);
    const D2D1_RECT_F cast = D2D1::RectF(colW + kGap, batt.bottom + kGap, kW, kTopH);
    float y = kTopH + kGap;
    D2D1_RECT_F bright{};
    if (brightness_ >= 0) {
        bright = D2D1::RectF(0, y, kW, y + kSliderH);
        y = bright.bottom + kGap;
    }
    const D2D1_RECT_F sound = D2D1::RectF(0, y, kW, y + kSliderH);
    for (D2D1_RECT_F r : {conn, batt, cast})
        shapes.push_back({r, modR});
    if (brightness_ >= 0)
        shapes.push_back({bright, modR});
    shapes.push_back({sound, modR});
    if (c.panel)
        for (const ShapeDip& s : shapes)
            c.Fill(s.rect, s.radius, t.panel);

    // Connections: a round switch and the state; the row opens its page.
    auto row = [&](int i, const std::wstring& id, const wchar_t* glyph, const std::wstring& title, const std::wstring& sub, bool on, bool avail) {
        const float rh = (kTopH - 16) / 2, y0 = conn.top + 8 + float(i) * rh, cy = y0 + rh / 2;
        const std::wstring page = id + L"-page";
        D2D1_RECT_F text = D2D1::RectF(conn.left + 6, y0 + 2, conn.right - 6, y0 + rh - 2);
        float th = Hover(c, page, hover_ == page);
        if (th > 0.001f || pressed_ == page)
            c.Fill(text, 16, pressed_ == page ? t.press : Alpha(t.hover, th));
        hits_.push_back({text, page});
        D2D1_RECT_F disc = D2D1::RectF(conn.left + 14, cy - 18, conn.left + 50, cy + 18);
        float dh = Hover(c, id, hover_ == id);
        c.Circle((disc.left + disc.right) / 2, cy, 18, on ? t.accent : Alpha(t.text, 0.16f + 0.08f * dh));
        bool halo = c.halo;
        c.halo = false;
        c.Glyph(glyph, disc, 15, on ? D2D1::ColorF(1, 1, 1, 0.98f) : avail ? t.text : t.faint);
        c.halo = halo;
        if (avail)
            hits_.push_back({disc, id});
        c.Text(title, D2D1::RectF(disc.right + 10, cy - 18, conn.right - 10, cy), 13.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(sub, D2D1::RectF(disc.right + 10, cy, conn.right - 10, cy + 18), 11.5f, t.subtext);
    };
    std::wstring wifiSub = radiosRead_ && !radios_.wifiPresent ? (status_.connected ? L"有线网络" : L"没有无线网卡")
                           : radiosRead_ && !radios_.wifiOn ? L"已关闭"
                           : status_.connected && status_.wifi ? (private_ ? std::wstring(L"已连接") : status_.network)
                           : status_.connected ? L"有线网络" : L"未连接";
    bool wifiOn = radiosRead_ ? radios_.wifiOn : status_.wifi && status_.connected;
    row(0, L"wifi", L"\uE701", L"Wi-Fi", wifiSub, wifiOn, !radiosRead_ || radios_.wifiPresent);
    int connected = int(std::count_if(devices_.begin(), devices_.end(), [](const BluetoothDevice& d) { return d.connected; }));
    std::wstring btSub = !radiosRead_ ? L"…" : !radios_.btPresent ? L"不可用" : !radios_.btOn ? L"已关闭"
                         : connected ? std::to_wstring(connected) + L" 台设备已连接" : L"已打开";
    row(1, L"bt", L"\uE702", L"蓝牙", btSub, radios_.btOn, radios_.btPresent);

    // Battery: a small ring, the level and the state.
    {
        float bh = Hover(c, L"battery", hover_ == L"battery");
        if (bh > 0.001f || pressed_ == L"battery")
            c.Fill(batt, modR, pressed_ == L"battery" ? t.press : Alpha(t.hover, bh));
        float cx = batt.left + 32, cy = (batt.top + batt.bottom) / 2;
        if (status_.battery) {
            int pct = std::clamp(status_.batteryPercent, 0, 100);
            D2D1_COLOR_F level = status_.charging ? D2D1::ColorF(0x30D158) : pct <= 20 ? D2D1::ColorF(0xFF453A) : t.text;
            c.Arc(cx, cy, 15, -90, 270, Alpha(t.text, 0.18f), 4);
            c.Arc(cx, cy, 15, -90, -90 + 360.f * float(pct) / 100.f, level, 4);
            if (status_.charging)
                c.Glyph(L"\uE945", D2D1::RectF(cx - 12, cy - 12, cx + 12, cy + 12), 11, t.text);
            std::wstring state = status_.charging ? (pct >= 100 ? L"已充满" : L"正在充电")
                                 : status_.batteryMinutes >= 0
                                     ? L"剩余 " + (status_.batteryMinutes >= 60 ? std::to_wstring(status_.batteryMinutes / 60) + L" 小时"
                                                                                     : std::to_wstring(status_.batteryMinutes) + L" 分钟")
                                     : std::wstring(L"使用电池");
            c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(cx + 24, cy - 18, batt.right - 8, cy + 1), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(state, D2D1::RectF(cx + 24, cy + 1, batt.right - 8, cy + 18), 11.5f, t.subtext);
        } else {
            c.Circle(cx, cy, 16, Alpha(t.text, 0.16f));
            c.Glyph(L"\uE945", D2D1::RectF(cx - 14, cy - 14, cx + 14, cy + 14), 13, t.text);
            c.Text(L"电源", D2D1::RectF(cx + 24, cy - 18, batt.right - 8, cy + 1), 13.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(L"已接通电源", D2D1::RectF(cx + 24, cy + 1, batt.right - 8, cy + 18), 11.5f, t.subtext);
        }
        hits_.push_back({batt, L"battery"});
    }
    // Casting, like macOS's screen mirroring: its page lists the wireless displays.
    {
        float ch = Hover(c, L"cast", hover_ == L"cast");
        if (ch > 0.001f || pressed_ == L"cast")
            c.Fill(cast, modR, pressed_ == L"cast" ? t.press : Alpha(t.hover, ch));
        float cx = cast.left + 32, cy = (cast.top + cast.bottom) / 2;
        c.Circle(cx, cy, 16, Alpha(t.text, 0.16f));
        c.Glyph(L"\uE7F4", D2D1::RectF(cx - 14, cy - 14, cx + 14, cy + 14), 13, t.text);
        c.Text(L"投屏", D2D1::RectF(cx + 24, cy - 18, cast.right - 8, cy + 1), 13.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(projecting_.empty() ? L"无线显示器" : L"已连接", D2D1::RectF(cx + 24, cy + 1, cast.right - 8, cy + 18), 11.5f,
               projecting_.empty() ? t.subtext : t.accent);
        hits_.push_back({cast, L"cast"});
    }
    if (brightness_ >= 0)
        PaintSlider(c, t, bright, L"bright", L"显示器", L"\uE706", brightness_, false, std::to_wstring(brightness_) + L"%", brightTrack_);
    std::wstring level = !status_.audio ? L"没有播放设备" : status_.muted ? L"静音" : std::to_wstring(status_.volume) + L"%";
    const wchar_t* g = !status_.audio || status_.muted ? L"\uE74F" : status_.volume < 34 ? L"\uE993" : status_.volume < 67 ? L"\uE994" : L"\uE995";
    PaintSlider(c, t, sound, L"slider", L"声音", g, status_.audio ? status_.volume : 0, status_.muted, level, volumeTrack_);
}

void ControlCenter::PaintSlider(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& id, const std::wstring& title, const std::wstring& glyph,
                                int value, bool dim, const std::wstring& right, D2D1_RECT_F& track)
{
    c.Text(title, D2D1::RectF(card.left + kPad, card.top + 8, card.right - 130, card.top + 28), 12.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    // The sound opens its page (the output device, each app) from the chevron or the title.
    const bool more = id == L"slider";
    c.Text(right, D2D1::RectF(card.right - 130, card.top + 8, card.right - kPad - (more ? 30 : 0), card.top + 28), 11.5f, t.subtext,
           DWRITE_FONT_WEIGHT_NORMAL, 2);
    if (more) {
        D2D1_RECT_F chev = D2D1::RectF(card.right - kPad - 24, card.top + 6, card.right - kPad, card.top + 30);
        float mh = Hover(c, L"sound-page", hover_ == L"sound-page");
        c.Fill(chev, 12, pressed_ == L"sound-page" ? t.press : Alpha(t.text, 0.08f + 0.08f * mh));
        c.Glyph(L"\uE76C", chev, 9.5f, t.text);
        hits_.push_back({D2D1::RectF(card.left, card.top, card.right, card.top + 30), L"sound-page"});
    }
    // A wide pill filled from the left, as on macOS, the symbol at its start.
    track = D2D1::RectF(card.left + kPad, card.top + 34, card.right - kPad, card.top + 58);
    const float th = track.bottom - track.top, r = th / 2;
    float hv = Hover(c, id, hover_ == id || dragging_ == id);
    c.Fill(track, r, Alpha(t.text, 0.14f + 0.04f * hv));
    float k = float(std::clamp(value, 0, 100)) / 100.f;
    D2D1_RECT_F fill = D2D1::RectF(track.left, track.top, track.left + th + (track.right - track.left - th) * k, track.bottom);
    c.Fill(fill, r, dim ? Alpha(t.text, 0.45f) : Alpha(t.text, 0.9f));
    bool halo = c.halo;
    c.halo = false;
    // The symbol sits on the fill: the glass's side of the ink.
    D2D1_COLOR_F onFill = t.light ? D2D1::ColorF(1, 1, 1, 0.97f) : D2D1::ColorF(0.07f, 0.07f, 0.09f, 0.97f);
    D2D1_RECT_F g = D2D1::RectF(track.left + 2, track.top, track.left + th + 2, track.bottom);
    c.Glyph(glyph, g, 11, onFill);
    c.halo = halo;
    if (id == L"slider")
        hits_.push_back({D2D1::RectF(g.left - 2, track.top - 4, g.right, track.bottom + 4), L"mute"});
    hits_.push_back({D2D1::RectF(track.left - 4, track.top - 8, track.right + 4, track.bottom + 8), id});
}

float ControlCenter::PaintHeader(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& title, bool on, const std::wstring& toggleId)
{
    const float cy = card.top + kHeader / 2;
    D2D1_RECT_F back = D2D1::RectF(card.left + 12, cy - 16, card.left + 44, cy + 16);
    float bh = Hover(c, L"back", hover_ == L"back");
    c.Fill(back, 16, pressed_ == L"back" ? t.press : Alpha(t.hover, 0.5f + 0.5f * bh));
    c.Glyph(L"\uE76B", back, 11, t.text);
    hits_.push_back({back, L"back"});
    c.Text(title, D2D1::RectF(back.right + 10, cy - 12, card.right - 120, cy + 12), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Line(card.left + kPad + 4, card.top + kHeader, card.right - kPad - 4, card.top + kHeader, t.divider);
    if (toggleId.empty())
        return card.right - kPad;
    D2D1_RECT_F sw = D2D1::RectF(card.right - kPad - 6 - 44, cy - 13, card.right - kPad - 6, cy + 13);
    Switch(c, t, sw, on, Hover(c, toggleId, hover_ == toggleId));
    hits_.push_back({sw, toggleId});
    return sw.left;
}

void ControlCenter::PaintWifi(Canvas& c, const Theme& t, D2D1_RECT_F card)
{
    const float w = card.right - card.left;
    float swLeft = PaintHeader(c, t, card, L"Wi-Fi", radios_.wifiOn, L"wifi");
    if (radios_.wifiOn) {
        D2D1_RECT_F rescan = D2D1::RectF(swLeft - 42, card.top + kHeader / 2 - 15, swLeft - 12, card.top + kHeader / 2 + 15);
        float rh = Hover(c, L"rescan", hover_ == L"rescan");
        if (rh > 0.001f)
            c.Fill(rescan, 15, Alpha(t.hover, rh));
        c.Glyph(scanning_ ? L"\uE895" : L"\uE72C", rescan, 12, t.subtext);
        hits_.push_back({rescan, L"rescan"});
    }
    float y = card.top + kHeader;
    auto message = [&](const wchar_t* text) {
        c.Text(text, D2D1::RectF(kPad, y, w - kPad, y + kRowH), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += kRowH;
    };
    const bool on = !radiosRead_ || radios_.wifiOn;
    if (!on)
        message(L"Wi-Fi 已关闭");
    else if (networks_.empty())
        message(scanned_ ? L"没有找到 Wi-Fi 网络" : L"正在搜索网络…");
    for (int i = 0; on && i < kMaxRows && i < int(networks_.size()); ++i) {
        const WifiNetwork& n = networks_[size_t(i)];
        D2D1_RECT_F row = D2D1::RectF(kPad - 4, y + 2, w - kPad + 4, y + kRowH - 2);
        std::wstring id = L"net:" + std::to_wstring(i);
        bool hot = hover_ == id || (hover_ == L"disconnect" && n.connected);
        float hv = Hover(c, id, hot);
        if (hv > 0.001f)
            c.Fill(row, (row.bottom - row.top) / 2, Alpha(pressed_ == id ? t.press : t.hover, hv));
        float cy = (row.top + row.bottom) / 2;
        Bars(c, row.left + 16, cy, n.signal, n.connected ? t.accent : t.text);
        std::wstring ssid = private_ ? L"Wi-Fi " + std::to_wstring(i + 1) : n.ssid;
        c.Text(ssid, D2D1::RectF(row.left + 44, row.top, row.right - 110, row.bottom), 14, t.text, n.connected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        float right = row.right - 12;
        if (n.secured)
            c.Glyph(L"\uE72E", D2D1::RectF(row.right - 112, row.top, row.right - 92, row.bottom), 11, t.faint);
        if (n.connected) {
            if (hv > 0.05f) {
                std::wstring label = L"断开";
                float bw = c.TextWidth(label, 12) + 24;
                D2D1_RECT_F b = D2D1::RectF(right - bw, cy - 14, right, cy + 14);
                c.BeginGroup(hv, 1, (b.left + b.right) / 2, cy);
                c.Fill(b, 14, Alpha(hover_ == L"disconnect" ? t.press : t.hover, 1.4f));
                c.Text(label, b, 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
                c.EndGroup();
                hits_.push_back({b, L"disconnect"});
            } else {
                c.Text(L"已连接", D2D1::RectF(right - 80, row.top, right, row.bottom), 12, t.accent, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
            }
        } else if (hv > 0.05f) {
            c.Text(n.profile.empty() && n.secured ? L"输入密码…" : L"连接", D2D1::RectF(right - 90, row.top, right, row.bottom), 12,
                   Alpha(t.subtext, hv), DWRITE_FONT_WEIGHT_NORMAL, 2);
        }
        hits_.push_back({row, id});
        y += kRowH;
    }
    c.Line(kPad + 4, y + 2, w - kPad - 4, y + 2, t.divider);
    auto link = [&](const std::wstring& id, const std::wstring& label, float left, float right, int align) {
        D2D1_RECT_F b = D2D1::RectF(left, y + 9, right, y + kFooter - 3);
        float hv = Hover(c, id, hover_ == id);
        if (hv > 0.001f)
            c.Fill(b, (b.bottom - b.top) / 2, Alpha(t.hover, hv));
        c.Text(label, D2D1::RectF(b.left + 12, b.top, b.right - 12, b.bottom), 12.5f, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, align);
        hits_.push_back({b, id});
    };
    link(L"netsettings", L"网络和 Internet 设置", kPad - 4, w / 2 + 20, 0);
    link(L"netlist", L"更多 Wi-Fi 网络", w / 2 + 28, w - kPad + 4, 2);
}

void ControlCenter::PaintBluetooth(Canvas& c, const Theme& t, D2D1_RECT_F card)
{
    const float w = card.right - card.left;
    PaintHeader(c, t, card, L"蓝牙", radios_.btOn, L"bt");
    float y = card.top + kHeader;
    auto message = [&](const wchar_t* text) {
        c.Text(text, D2D1::RectF(kPad, y, w - kPad, y + kRowH), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += kRowH;
    };
    if (radiosRead_ && !radios_.btPresent)
        message(L"这台电脑没有蓝牙");
    else if (!radios_.btOn)
        message(L"蓝牙已关闭");
    else if (devices_.empty())
        message(devicesRead_ ? L"没有已配对的设备" : L"正在读取设备…");
    for (int i = 0; radios_.btOn && i < kMaxRows && i < int(devices_.size()); ++i) {
        const BluetoothDevice& d = devices_[size_t(i)];
        D2D1_RECT_F row = D2D1::RectF(kPad - 4, y + 2, w - kPad + 4, y + kRowH - 2);
        std::wstring id = L"bt:" + std::to_wstring(i);
        float hv = Hover(c, id, hover_ == id);
        if (hv > 0.001f)
            c.Fill(row, (row.bottom - row.top) / 2, Alpha(pressed_ == id ? t.press : t.hover, hv));
        float cy = (row.top + row.bottom) / 2;
        D2D1_RECT_F disc = D2D1::RectF(row.left + 8, cy - 14, row.left + 36, cy + 14);
        c.Circle((disc.left + disc.right) / 2, cy, 14, d.connected ? t.accent : Alpha(t.text, 0.14f));
        bool halo = c.halo;
        c.halo = false;
        c.Glyph(L"\uE702", disc, 12, d.connected ? D2D1::ColorF(1, 1, 1, 0.98f) : t.text);
        c.halo = halo;
        std::wstring name = private_ ? L"蓝牙设备 " + std::to_wstring(i + 1) : d.name;
        c.Text(name, D2D1::RectF(disc.right + 10, row.top, row.right - 80, row.bottom), 13.5f, t.text,
               d.connected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        c.Text(d.connected ? L"已连接" : L"未连接", D2D1::RectF(row.right - 80, row.top, row.right - 12, row.bottom), 12,
               d.connected ? t.accent : t.faint, d.connected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, 2);
        hits_.push_back({row, id});
        y += kRowH;
    }
    c.Line(kPad + 4, y + 2, w - kPad - 4, y + 2, t.divider);
    D2D1_RECT_F b = D2D1::RectF(kPad - 4, y + 9, w / 2 + 40, y + kFooter - 3);
    float hv = Hover(c, L"btsettings", hover_ == L"btsettings");
    if (hv > 0.001f)
        c.Fill(b, (b.bottom - b.top) / 2, Alpha(t.hover, hv));
    c.Text(L"蓝牙和其他设备设置", D2D1::RectF(b.left + 12, b.top, b.right - 12, b.bottom), 12.5f, t.subtext);
    hits_.push_back({b, L"btsettings"});
}

void ControlCenter::Link(Canvas& c, const Theme& t, const std::wstring& id, const std::wstring& label, float left, float right, float y, int align)
{
    D2D1_RECT_F b = D2D1::RectF(left, y + 9, right, y + kFooter - 3);
    float hv = Hover(c, id, hover_ == id);
    if (hv > 0.001f)
        c.Fill(b, (b.bottom - b.top) / 2, Alpha(t.hover, hv));
    c.Text(label, D2D1::RectF(b.left + 12, b.top, b.right - 12, b.bottom), 12.5f, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, align);
    hits_.push_back({b, id});
}

// The sound output, after the Windows taskbar's: the playback devices (a
// tick on the default one) and below, the volume mixer, one slider per app.
void ControlCenter::PaintSound(Canvas& c, const Theme& t, D2D1_RECT_F card)
{
    const float w = card.right - card.left;
    PaintHeader(c, t, card, L"声音输出", false, L"");
    float y = card.top + kHeader;
    auto message = [&](const wchar_t* text, float h) {
        c.Text(text, D2D1::RectF(kPad, y, w - kPad, y + h), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += h;
    };
    if (outputs_.empty())
        message(audioRead_ ? L"没有播放设备" : L"正在读取…", kRowH);
    for (int i = 0; i < kMaxOutputs && i < int(outputs_.size()); ++i) {
        const AudioOutput& o = outputs_[size_t(i)];
        D2D1_RECT_F row = D2D1::RectF(kPad - 4, y + 2, w - kPad + 4, y + kRowH - 2);
        std::wstring id = L"out:" + std::to_wstring(i);
        float hv = Hover(c, id, hover_ == id);
        if (hv > 0.001f)
            c.Fill(row, (row.bottom - row.top) / 2, Alpha(pressed_ == id ? t.press : t.hover, hv));
        float cy = (row.top + row.bottom) / 2;
        D2D1_RECT_F disc = D2D1::RectF(row.left + 8, cy - 14, row.left + 36, cy + 14);
        c.Circle((disc.left + disc.right) / 2, cy, 14, o.isDefault ? t.accent : Alpha(t.text, 0.14f));
        const wchar_t* g = o.kind == AudioOutput::Headphones ? L"\uE7F6" : o.kind == AudioOutput::Display ? L"\uE7F4" : L"\uE767";
        bool halo = c.halo;
        c.halo = false;
        c.Glyph(g, disc, 12, o.isDefault ? D2D1::ColorF(1, 1, 1, 0.98f) : t.text);
        c.halo = halo;
        float right = row.right - (o.isDefault ? 36 : 12);
        if (o.adapter.empty()) {
            c.Text(o.name, D2D1::RectF(disc.right + 10, row.top, right, row.bottom), 13.5f, t.text,
                   o.isDefault ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        } else {
            c.Text(o.name, D2D1::RectF(disc.right + 10, cy - 17, right, cy + 1), 13, t.text, o.isDefault ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
            c.Text(o.adapter, D2D1::RectF(disc.right + 10, cy, right, cy + 16), 11, t.subtext);
        }
        if (o.isDefault)
            c.Glyph(L"\uE73E", D2D1::RectF(row.right - 34, row.top, row.right - 10, row.bottom), 13, t.text);
        hits_.push_back({row, id});
        y += kRowH;
    }

    // The volume mixer.
    c.Line(kPad + 4, y + 2, w - kPad - 4, y + 2, t.divider);
    c.Text(L"音量合成器", D2D1::RectF(kPad, y + 8, w - kPad, y + kSection), 12, t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    y += kSection;
    appTracks_.assign(apps_.size(), D2D1_RECT_F{});
    if (apps_.empty())
        message(audioRead_ ? L"没有应用在播放声音" : L"正在读取…", kAppRowH);
    for (int i = 0; i < kMaxApps && i < int(apps_.size()); ++i) {
        const AudioApp& a = apps_[size_t(i)];
        const std::wstring sid = L"app:" + std::to_wstring(i), mid = L"amute:" + std::to_wstring(i);
        float cy = y + kAppRowH / 2;
        // The icon mutes the app; dimmed with a struck speaker when muted.
        D2D1_RECT_F icon = D2D1::RectF(kPad, cy - 13, kPad + 26, cy + 13);
        float ih = Hover(c, mid, hover_ == mid);
        if (ih > 0.001f || pressed_ == mid)
            c.Fill(D2D1::RectF(icon.left - 4, icon.top - 4, icon.right + 4, icon.bottom + 4), 9, pressed_ == mid ? t.press : Alpha(t.hover, ih));
        if (!a.icon.empty())
            c.Image(L"audio:" + a.key, a.icon, icon, a.muted ? 0.4f : 1.f);
        else
            c.Glyph(a.key == L"#system" ? L"\uE767" : L"\uE71D", icon, 14, a.muted ? t.faint : t.text);
        if (a.muted) {
            D2D1_RECT_F badge = D2D1::RectF(icon.right - 10, icon.bottom - 10, icon.right + 4, icon.bottom + 4);
            c.Circle((badge.left + badge.right) / 2, (badge.top + badge.bottom) / 2, 7, t.light ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.95f));
            c.Glyph(L"\uE74F", badge, 7.5f, t.text);
        }
        hits_.push_back({D2D1::RectF(icon.left - 4, icon.top - 4, icon.right + 4, icon.bottom + 4), mid});
        const float left = icon.right + 12;
        c.Text(a.name, D2D1::RectF(left, cy - 21, w - kPad - 44, cy - 3), 12.5f, a.active ? t.text : t.subtext,
               a.active ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        c.Text(a.muted ? L"静音" : std::to_wstring(a.volume) + L"%", D2D1::RectF(w - kPad - 60, cy - 21, w - kPad, cy - 3), 11, t.subtext,
               DWRITE_FONT_WEIGHT_NORMAL, 2);
        // A thin line with a knob, as on the Windows taskbar's mixer.
        const float knob = 7, ty = cy + 9;
        D2D1_RECT_F track = D2D1::RectF(left + knob, ty - 2, w - kPad - knob, ty + 2);
        float hv = Hover(c, sid, hover_ == sid || dragging_ == sid);
        c.Fill(track, 2, Alpha(t.text, 0.18f + 0.04f * hv));
        float k = float(std::clamp(a.volume, 0, 100)) / 100.f, kx = track.left + (track.right - track.left) * k;
        c.Fill(D2D1::RectF(track.left, track.top, kx, track.bottom), 2, a.muted ? Alpha(t.text, 0.4f) : Alpha(t.text, 0.85f));
        bool halo = c.halo;
        c.halo = false;
        c.Circle(kx, ty + 0.6f, knob + 0.5f + hv, D2D1::ColorF(0, 0, 0, 0.16f));
        c.Circle(kx, ty, knob + hv, D2D1::ColorF(1, 1, 1, 0.98f));
        c.halo = halo;
        appTracks_[size_t(i)] = track;
        hits_.push_back({D2D1::RectF(track.left - 4, track.top - 8, track.right + 4, track.bottom + 8), sid});
        y += kAppRowH;
    }
    c.Line(kPad + 4, y + 2, w - kPad - 4, y + 2, t.divider);
    Link(c, t, L"soundsettings", L"更多音量设置", kPad - 4, w / 2 + 10, y, 0);
    Link(c, t, L"mixer", L"音量合成器设置", w / 2 + 18, w - kPad + 4, y, 2);
}

namespace {

// Win+P's pictures: two screens, which of them shows the desktop and how.
void ModePicture(Canvas& c, ProjectionMode m, float cx, float cy, D2D1_COLOR_F ink)
{
    const float sw = 17, sh = 12, gap = m == ProjectionMode::Extend ? 0.f : 5.f;
    D2D1_RECT_F a = D2D1::RectF(cx - gap / 2 - sw, cy - sh / 2, cx - gap / 2, cy + sh / 2);
    D2D1_RECT_F b = D2D1::RectF(cx + gap / 2, cy - sh / 2, cx + gap / 2 + sw, cy + sh / 2);
    D2D1_COLOR_F off = Alpha(ink, 0.35f);
    switch (m) {
    case ProjectionMode::Internal:
        c.Fill(a, 2.5f, ink);
        c.Stroke(b, 2.5f, off, 1.2f);
        break;
    case ProjectionMode::External:
        c.Stroke(a, 2.5f, off, 1.2f);
        c.Fill(b, 2.5f, ink);
        break;
    case ProjectionMode::Clone:
        c.Fill(a, 2.5f, ink);
        c.Fill(b, 2.5f, ink);
        break;
    default: // extend: one desktop over both
        c.Fill(D2D1::RectF(a.left - 3, a.top, b.right + 3, b.bottom), 2.5f, ink);
        break;
    }
}

} // namespace

// Casting, after the Windows taskbar's cast panel: the wireless displays in
// range, then Win+P's projection modes.
void ControlCenter::PaintCast(Canvas& c, const Theme& t, D2D1_RECT_F card)
{
    const float w = card.right - card.left;
    float right = PaintHeader(c, t, card, L"投屏", false, L"");
    const bool searching = finder_.Searching();
    {
        D2D1_RECT_F rescan = D2D1::RectF(right - 30, card.top + kHeader / 2 - 15, right, card.top + kHeader / 2 + 15);
        float rh = Hover(c, L"castscan", hover_ == L"castscan");
        if (rh > 0.001f)
            c.Fill(rescan, 15, Alpha(t.hover, rh));
        c.Glyph(searching ? L"\uE895" : L"\uE72C", rescan, 12, t.subtext);
        hits_.push_back({rescan, L"castscan"});
    }
    float y = card.top + kHeader;
    if (displays_.empty()) {
        c.Text(searching ? L"正在搜索无线显示器…" : L"没有找到无线显示器", D2D1::RectF(kPad, y, w - kPad, y + kRowH), 13, t.subtext,
               DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += kRowH;
    }
    for (int i = 0; i < kMaxDisplays && i < int(displays_.size()); ++i) {
        const WirelessDisplay& d = displays_[size_t(i)];
        D2D1_RECT_F row = D2D1::RectF(kPad - 4, y + 2, w - kPad + 4, y + kRowH - 2);
        std::wstring id = L"disp:" + std::to_wstring(i);
        const bool on = d.id == projecting_, busy = d.id == connecting_;
        float hv = Hover(c, id, hover_ == id || (hover_ == L"castoff" && on));
        if (hv > 0.001f)
            c.Fill(row, (row.bottom - row.top) / 2, Alpha(pressed_ == id ? t.press : t.hover, hv));
        float cy = (row.top + row.bottom) / 2;
        D2D1_RECT_F disc = D2D1::RectF(row.left + 8, cy - 14, row.left + 36, cy + 14);
        c.Circle((disc.left + disc.right) / 2, cy, 14, on ? t.accent : Alpha(t.text, 0.14f));
        bool halo = c.halo;
        c.halo = false;
        c.Glyph(L"\uE7F4", disc, 12, on ? D2D1::ColorF(1, 1, 1, 0.98f) : t.text);
        c.halo = halo;
        std::wstring name = private_ ? L"无线显示器 " + std::to_wstring(i + 1) : d.name;
        c.Text(name, D2D1::RectF(disc.right + 10, row.top, row.right - 96, row.bottom), 13.5f, t.text,
               on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        float r = row.right - 12;
        if (busy) {
            c.Text(L"正在连接…", D2D1::RectF(r - 90, row.top, r, row.bottom), 12, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 2);
        } else if (on && hv > 0.05f) {
            std::wstring label = L"断开";
            float bw = c.TextWidth(label, 12) + 24;
            D2D1_RECT_F b = D2D1::RectF(r - bw, cy - 14, r, cy + 14);
            c.BeginGroup(hv, 1, (b.left + b.right) / 2, cy);
            c.Fill(b, 14, Alpha(hover_ == L"castoff" ? t.press : t.hover, 1.4f));
            c.Text(label, b, 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
            c.EndGroup();
            hits_.push_back({b, L"castoff"});
        } else if (on) {
            c.Text(L"已连接", D2D1::RectF(r - 80, row.top, r, row.bottom), 12, t.accent, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
        } else if (hv > 0.05f) {
            c.Text(L"连接", D2D1::RectF(r - 80, row.top, r, row.bottom), 12, Alpha(t.subtext, hv), DWRITE_FONT_WEIGHT_NORMAL, 2);
        }
        hits_.push_back({row, id});
        y += kRowH;
    }

    // Projection modes (Win+P); with a single screen only its own is there.
    c.Line(kPad + 4, y + 2, w - kPad - 4, y + 2, t.divider);
    c.Text(L"投影模式", D2D1::RectF(kPad, y + 8, w - kPad, y + kSection), 12, t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (monitors_ < 2)
        c.Text(L"只连接了一个显示器", D2D1::RectF(kPad, y + 8, w - kPad, y + kSection), 11, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
    y += kSection;
    const ProjectionMode modes[4] = {ProjectionMode::Internal, ProjectionMode::Clone, ProjectionMode::Extend, ProjectionMode::External};
    const wchar_t* labels[4] = {L"仅电脑屏幕", L"复制", L"扩展", L"仅第二屏幕"};
    const float tg = 8, tw = (w - 2 * kPad - 3 * tg) / 4;
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F tile = D2D1::RectF(kPad + float(i) * (tw + tg), y + 4, kPad + float(i) * (tw + tg) + tw, y + kModeH - 8);
        const bool cur = modes[i] == mode_, usable = monitors_ >= 2 || cur;
        std::wstring id = L"mode:" + std::to_wstring(i);
        float hv = usable ? Hover(c, id, hover_ == id) : 0.f;
        c.Fill(tile, 14, cur ? t.accent : pressed_ == id ? t.press : Alpha(t.text, 0.07f + 0.07f * hv));
        D2D1_COLOR_F ink = cur ? D2D1::ColorF(1, 1, 1, 0.98f) : usable ? t.text : Alpha(t.text, 0.42f);
        bool halo = c.halo;
        c.halo = !cur;
        ModePicture(c, modes[i], (tile.left + tile.right) / 2, tile.top + 22, ink);
        c.Text(labels[i], D2D1::RectF(tile.left + 2, tile.top + 36, tile.right - 2, tile.bottom - 4), 11, ink,
               cur ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, 1);
        c.halo = halo;
        if (usable && !cur)
            hits_.push_back({tile, id});
    }
    y += kModeH;
    c.Line(kPad + 4, y + 2, w - kPad - 4, y + 2, t.divider);
    Link(c, t, L"castproject", L"投影到此电脑", kPad - 4, w / 2 + 10, y, 0);
    Link(c, t, L"castpanel", L"系统投屏面板", w / 2 + 18, w - kPad + 4, y, 2);
}

std::wstring ControlCenter::HitAt(POINT pt) const
{
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    // Small controls first: they sit on larger rows.
    for (const Hit& h : hits_)
        if ((h.id == L"disconnect" || h.id == L"mute" || h.id == L"wifi" || h.id == L"bt" || h.id == L"rescan" || h.id == L"castoff" ||
             h.id == L"castscan" || h.id.rfind(L"amute:", 0) == 0) &&
            In(h.rect, x, y))
            return h.id;
    for (const Hit& h : hits_)
        if (In(h.rect, x, y))
            return h.id;
    return {};
}

void ControlCenter::SetSliderAt(const std::wstring& id, float x)
{
    if (id.rfind(L"app:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(4)));
        if (i >= apps_.size() || i >= appTracks_.size() || appTracks_[i].right <= appTracks_[i].left)
            return;
        const D2D1_RECT_F& tr = appTracks_[i];
        int v = int(std::lround(std::clamp((x - tr.left) / (tr.right - tr.left), 0.f, 1.f) * 100));
        apps_[i].volume = v;
        if (v > 0)
            apps_[i].muted = false;
        SetAppVolume(apps_[i].key, v);
        surface_.Invalidate();
        return;
    }
    const D2D1_RECT_F& tr = id == L"bright" ? brightTrack_ : volumeTrack_;
    if (tr.right <= tr.left)
        return;
    int v = int(std::lround(std::clamp((x - tr.left) / (tr.right - tr.left), 0.f, 1.f) * 100));
    if (id == L"bright") {
        brightness_ = v;
        brightPending_ = v;
        if (!brightBusy_) {
            // WMI is slow: one worker at a time sets the latest value.
            brightBusy_ = true;
            HWND h = surface_.Hwnd();
            OnWorker([h, v] {
                SetBrightness(v);
                PostMessageW(h, WM_CC_BRIGHT, WPARAM(v), 0);
            });
        }
    } else {
        SetVolume(v);
        status_.volume = v;
        if (v > 0)
            status_.muted = false;
    }
    surface_.Invalidate();
}

void ControlCenter::Activate(const std::wstring& id)
{
    if (id == L"wifi") {
        if (radios_.wifiPresent)
            SetRadioAsync(false, !radios_.wifiOn);
    } else if (id == L"bt") {
        if (radios_.btPresent)
            SetRadioAsync(true, !radios_.btOn);
    } else if (id == L"wifi-page") {
        ShowPage(Page::Wifi);
        Scan(true);
    } else if (id == L"bt-page") {
        ShowPage(Page::Bluetooth);
        ReadRadios(true);
    } else if (id == L"back") {
        ShowPage(Page::Main);
    } else if (id == L"rescan") {
        Scan(true);
        surface_.Invalidate();
    } else if (id == L"mute") {
        ToggleMute();
        status_ = QuerySystemStatus();
        surface_.Invalidate();
        if (onChanged)
            onChanged();
    } else if (id == L"cast") {
        ShowPage(Page::Cast);
        ReadMode();
        finder_.Start(surface_.Hwnd(), WM_CC_DISPLAYS);
    } else if (id == L"castscan") {
        finder_.Start(surface_.Hwnd(), WM_CC_DISPLAYS);
        surface_.Invalidate();
    } else if (id.rfind(L"disp:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(5)));
        if (i >= displays_.size() || !connecting_.empty() || displays_[i].id == projecting_)
            return;
        connecting_ = displays_[i].id;
        HWND h = surface_.Hwnd();
        std::wstring target = connecting_;
        OnWorker([h, target] {
            bool ok = ProjectTo(target);
            auto* d = new std::wstring(target);
            if (!PostMessageW(h, WM_CC_PROJECTED, ok ? 1 : 0, reinterpret_cast<LPARAM>(d)))
                delete d;
        });
        surface_.Invalidate();
    } else if (id == L"castoff") {
        HWND h = surface_.Hwnd();
        OnWorker([h] { PostMessageW(h, WM_CC_PROJECTED, StopProjecting() ? 1 : 0, 0); });
    } else if (id.rfind(L"mode:", 0) == 0) {
        const ProjectionMode modes[4] = {ProjectionMode::Internal, ProjectionMode::Clone, ProjectionMode::Extend, ProjectionMode::External};
        ProjectionMode m = modes[std::clamp(int(std::stoul(id.substr(5))), 0, 3)];
        // The screens change under the panel: it closes, as Win+P's does.
        Close();
        OnWorker([m] { SetProjectionMode(m); });
    } else if (id == L"castproject") {
        Close();
        OpenUri(L"ms-settings:project");
    } else if (id == L"castpanel") {
        Close();
        SendWinK();
    } else if (id == L"sound-page") {
        ShowPage(Page::Sound);
        ReadAudio(true);
    } else if (id.rfind(L"out:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(4)));
        if (i >= outputs_.size() || outputs_[i].isDefault)
            return;
        for (size_t k = 0; k < outputs_.size(); ++k)
            outputs_[k].isDefault = k == i; // at once; the next reading confirms it
        std::wstring target = outputs_[i].id;
        HWND h = surface_.Hwnd();
        int px = int(std::lround(26 * surface_.Scale()));
        OnWorker([h, target, px] {
            SetDefaultOutput(target);
            auto* r = new AudioRead();
            r->outputs = ListAudioOutputs();
            r->apps = ListAudioApps(px);
            r->icons = true;
            if (!PostMessageW(h, WM_CC_AUDIO, 1, reinterpret_cast<LPARAM>(r)))
                delete r;
        });
        surface_.Invalidate();
    } else if (id.rfind(L"amute:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(6)));
        if (i < apps_.size()) {
            apps_[i].muted = !apps_[i].muted;
            SetAppMute(apps_[i].key, apps_[i].muted);
            surface_.Invalidate();
        }
    } else if (id == L"soundsettings") {
        Close();
        OpenUri(L"ms-settings:sound");
    } else if (id == L"mixer") {
        Close();
        OpenUri(L"ms-settings:apps-volume");
    } else if (id == L"disconnect") {
        HWND h = surface_.Hwnd();
        std::thread([h] {
            DisconnectWifi();
            Sleep(1500);
            PostMessageW(h, WM_WIFI_AGAIN, 0, 0);
        }).detach();
    } else if (id.rfind(L"net:", 0) == 0) {
        size_t i = size_t(std::stoul(id.substr(4)));
        if (i >= networks_.size() || networks_[i].connected)
            return;
        WifiNetwork n = networks_[i];
        if (n.profile.empty()) {
            // A new network: Windows asks for its password.
            Close();
            OpenUri(L"ms-availablenetworks:");
            return;
        }
        HWND h = surface_.Hwnd();
        std::thread([h, n] {
            ConnectWifi(n);
            Sleep(4000);
            PostMessageW(h, WM_WIFI_AGAIN, 0, 0);
        }).detach();
        scanning_ = true;
        surface_.Invalidate();
    } else if (id == L"netsettings") {
        Close();
        OpenUri(L"ms-settings:network");
    } else if (id == L"netlist") {
        Close();
        OpenUri(L"ms-availablenetworks:");
    } else if (id == L"btsettings" || id.rfind(L"bt:", 0) == 0) {
        Close();
        OpenUri(L"ms-settings:bluetooth");
    } else if (id == L"battery") {
        Close();
        OpenUri(status_.battery ? L"ms-settings:batterysaver" : L"ms-settings:powersleep");
    }
    Log(L"control: %s", id.rfind(L"net:", 0) == 0 ? L"connect" : id.rfind(L"bt:", 0) == 0 ? L"device" : id.rfind(L"disp:", 0) == 0 ? L"project" : id.c_str());
}

void ControlCenter::TestClick(const std::wstring& id) { Activate(id); }

void ControlCenter::LogState() const
{
    const RECT r = surface_.Rect();
    const float s = surface_.Scale();
    std::wstring spots;
    for (const Hit& h : hits_)
        if ((h.id.rfind(L"net:", 0) != 0 && h.id.rfind(L"bt:", 0) != 0 && h.id.rfind(L"app:", 0) != 0 && h.id.rfind(L"amute:", 0) != 0 &&
             h.id.rfind(L"disp:", 0) != 0) ||
            h.id == L"net:0" || h.id == L"bt:0" || h.id == L"app:0" || h.id == L"amute:0" || h.id == L"disp:0")
            spots += L" " + h.id + L"@" + std::to_wstring(int(float(r.left) + (h.rect.left + h.rect.right) / 2 * s)) + L"," +
                     std::to_wstring(int(float(r.top) + (h.rect.top + h.rect.bottom) / 2 * s));
    Log(L"control: spots%s", spots.c_str());
    const wchar_t* page = page_ == Page::Main       ? L"main"
                          : page_ == Page::Wifi      ? L"wifi"
                          : page_ == Page::Bluetooth ? L"bluetooth"
                          : page_ == Page::Sound     ? L"sound"
                                                     : L"cast";
    int def = -1;
    for (size_t i = 0; i < outputs_.size(); ++i)
        if (outputs_[i].isDefault)
            def = int(i);
    Log(L"control: state visible=%d page=%s connected=%d wifi=%d wifiRadio=%d signal=%d networks=%zu scanning=%d bluetooth=%d btOn=%d devices=%zu "
        L"brightness=%d volume=%d muted=%d battery=%d percent=%d outputs=%zu defaultOutput=%d apps=%zu displays=%zu searching=%d mode=%d "
        L"monitors=%d shapes=%zu rect=%ld,%ld,%ld,%ld",
        surface_.Visible() ? 1 : 0, page, status_.connected ? 1 : 0, status_.wifi ? 1 : 0, radios_.wifiOn ? 1 : 0, status_.signal, networks_.size(),
        scanning_ ? 1 : 0, radios_.btPresent ? 1 : 0, radios_.btOn ? 1 : 0, devices_.size(), brightness_, status_.volume, status_.muted ? 1 : 0,
        status_.battery ? 1 : 0, status_.batteryPercent, outputs_.size(), def, apps_.size(), displays_.size(), finder_.Searching() ? 1 : 0,
        int(mode_), monitors_, surface_.Shapes(), r.left, r.top, r.right, r.bottom);
}

bool ControlCenter::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && surface_.Visible())
            Close();
        return false;
    case WM_TIMER:
        if (w == kStatusTimer) {
            SystemStatus s = QuerySystemStatus();
            if (dragging_.empty() && !(s == status_)) {
                status_ = s;
                surface_.Invalidate();
            }
            ReadRadios(page_ == Page::Bluetooth);
            if (page_ == Page::Sound && dragging_.empty())
                ReadAudio(false);
            if (page_ == Page::Cast)
                ReadMode();
            return true;
        }
        return false;
    case WM_CC_RADIOS: {
        std::unique_ptr<RadioRead> r(reinterpret_cast<RadioRead*>(l));
        float before = Size().height;
        radios_ = r->radios;
        radiosRead_ = true;
        if (r->devicesRead) {
            devices_ = std::move(r->devices);
            devicesRead_ = true;
        }
        if (r->brightness != -3 && dragging_ != L"bright" && !brightBusy_)
            brightness_ = r->brightness;
        if (Size().height != before && surface_.Visible())
            Place();
        surface_.Invalidate();
        return true;
    }
    case WM_CC_AUDIO: {
        std::unique_ptr<AudioRead> r(reinterpret_cast<AudioRead*>(l));
        if (!dragging_.empty() && dragging_ != L"slider")
            return true; // an app's slider is held: keep what it shows
        float before = Size().height;
        bool missing = false;
        for (AudioApp& a : r->apps) {
            if (!a.icon.empty())
                continue;
            auto old = std::find_if(apps_.begin(), apps_.end(), [&](const AudioApp& o) { return o.key == a.key; });
            if (old != apps_.end() && !old->icon.empty())
                a.icon = old->icon;
            else if (!r->icons && !a.path.empty() && std::find(iconTried_.begin(), iconTried_.end(), a.key) == iconTried_.end())
                missing = true;
            if (r->icons)
                iconTried_.push_back(a.key);
        }
        outputs_ = std::move(r->outputs);
        apps_ = std::move(r->apps);
        audioRead_ = true;
        if (w == 1) {
            // A new default device: its volume is the sound slider's now.
            status_ = QuerySystemStatus();
            if (onChanged)
                onChanged();
        }
        if (missing)
            ReadAudio(true);
        if (Size().height != before && surface_.Visible())
            Place();
        surface_.Invalidate();
        return true;
    }
    case WM_CC_DISPLAYS: {
        float before = Size().height;
        displays_ = finder_.Displays();
        if (Size().height != before && surface_.Visible() && page_ == Page::Cast)
            Place();
        surface_.Invalidate();
        return true;
    }
    case WM_CC_PROJECTED: {
        std::unique_ptr<std::wstring> d(reinterpret_cast<std::wstring*>(l));
        connecting_.clear();
        if (d && w) {
            projecting_ = *d;
        } else if (!d && w) {
            projecting_.clear();
        } else if (surface_.Visible()) {
            // Windows refused (or the display wants a PIN): its own panel takes over.
            Close();
            SendWinK();
            return true;
        }
        surface_.Invalidate();
        return true;
    }
    case WM_CC_BRIGHT:
        brightBusy_ = false;
        if (brightPending_ >= 0 && brightPending_ != int(w)) {
            int v = brightPending_;
            SetSliderAt(L"bright", brightTrack_.left + (brightTrack_.right - brightTrack_.left) * float(v) / 100.f);
        } else {
            brightPending_ = -1;
        }
        return true;
    case WM_WIFI_LIST: {
        std::unique_ptr<std::vector<WifiNetwork>> list(reinterpret_cast<std::vector<WifiNetwork>*>(l));
        unsigned gen = unsigned(w);
        if (gen > acceptedGen_) {
            acceptedGen_ = gen;
            float before = Size().height;
            networks_ = std::move(*list);
            scanned_ = true;
            scanning_ = gen < scanGen_;
            if (Size().height != before && surface_.Visible())
                Place();
            surface_.Invalidate();
        }
        return true;
    }
    case WM_WIFI_AGAIN:
        if (surface_.Visible()) {
            status_ = QuerySystemStatus();
            Scan(false);
            if (onChanged)
                onChanged();
        }
        return true;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (dragging_.empty() && pressed_ == L"mute" && status_.audio && (w & MK_LBUTTON) &&
            std::abs(pt.x - downX_) > int(4 * surface_.Scale())) {
            // The symbol is part of the slider: a press there that moves drags it.
            dragging_ = L"slider";
            pressed_ = dragging_;
        }
        if (!dragging_.empty()) {
            SetSliderAt(dragging_, float(pt.x) / surface_.Scale());
            return true;
        }
        std::wstring h = HitAt(pt);
        if (h != hover_) {
            hover_ = h;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (dragging_.empty()) {
            hover_.clear();
            pressed_.clear();
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        pressed_ = HitAt(pt);
        if ((pressed_ == L"slider" && status_.audio) || pressed_ == L"bright" || pressed_.rfind(L"app:", 0) == 0) {
            dragging_ = pressed_;
            SetCapture(surface_.Hwnd());
            SetSliderAt(dragging_, float(pt.x) / surface_.Scale());
        } else if (pressed_ == L"mute") {
            // Mute on release in place; a drag from here moves the slider.
            downX_ = pt.x;
            SetCapture(surface_.Hwnd());
        } else if (!pressed_.empty()) {
            float s = surface_.Scale();
            surface_.Ripple(float(pt.x) / s, float(pt.y) / s);
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        if (!dragging_.empty()) {
            bool volume = dragging_ == L"slider";
            dragging_.clear();
            ReleaseCapture();
            pressed_.clear();
            if (volume && onChanged)
                onChanged();
            surface_.Invalidate();
            return true;
        }
        if (GetCapture() == surface_.Hwnd())
            ReleaseCapture();
        std::wstring h = HitAt(pt);
        bool same = !h.empty() && h == pressed_;
        pressed_.clear();
        surface_.Invalidate();
        if (same)
            Activate(h);
        return true;
    }
    case WM_MOUSEWHEEL: {
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (notches && status_.audio && page_ == Page::Main) {
            status_.volume = AdjustVolume(2 * notches);
            if (notches > 0)
                status_.muted = false;
            surface_.Invalidate();
            if (onChanged)
                onChanged();
        }
        return true;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || (w == VK_BACK && page_ != Page::Main)) {
            if (page_ != Page::Main)
                ShowPage(Page::Main);
            else
                Close();
            return true;
        }
        return false;
    }
    (void)result;
    return false;
}

} // namespace atrium
