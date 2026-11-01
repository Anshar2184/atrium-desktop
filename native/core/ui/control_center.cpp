#include "ui/control_center.h"

#include "clock.h"
#include "log.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

namespace atrium {

namespace {

constexpr float kW = 360, kGap = 10, kHeader = 64, kRowH = 44, kFooter = 44, kPad = 12;
constexpr int kMaxRows = 6;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT WM_WIFI_LIST = WM_APP + 0x50;  // wParam generation, lParam std::vector<WifiNetwork>*
constexpr UINT WM_WIFI_AGAIN = WM_APP + 0x51; // after connecting: read the state and the list again

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

void OpenUri(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

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

} // namespace

ControlCenter::ControlCenter(HINSTANCE instance) : surface_(instance, L"Atrium Control Center", true)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onMargin = [this] { Close(); };
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
}

int ControlCenter::Rows() const
{
    if (networks_.empty())
        return 1; // "searching", "no networks", "no Wi-Fi"
    return std::min(kMaxRows, int(networks_.size()));
}

float ControlCenter::NetworkHeight() const { return kHeader + float(Rows()) * kRowH + kFooter + 4; }

D2D1_SIZE_F ControlCenter::Size() const
{
    float h = NetworkHeight() + kGap + barH_;
    if (status_.battery)
        h += kGap + barH_;
    return D2D1::SizeF(kW, h);
}

void ControlCenter::Place()
{
    RECT r = PopupRect(bar_, Size(), PopupAlign::Right);
    if (surface_.Visible())
        surface_.Move(r); // pops out of its entry on the bar
    else
        surface_.Show(r, true, POINT{(anchor_.left + anchor_.right) / 2, r.bottom});
}

void ControlCenter::Open(RECT anchor, RECT bar)
{
    anchor_ = anchor;
    bar_ = bar;
    hover_.clear();
    pressed_.clear();
    dragging_ = false;
    hoverAnim_.clear();
    openedMs_ = ClockMs();
    status_ = QuerySystemStatus();
    Place();
    Scan(false); // what the adapter already knows, at once
    Scan(true);  // then a fresh scan
    SetTimer(surface_.Hwnd(), kStatusTimer, 1500, nullptr);
    Log(L"control: opened");
}

void ControlCenter::Close()
{
    if (!surface_.Visible())
        return;
    KillTimer(surface_.Hwnd(), kStatusTimer);
    if (dragging_)
        ReleaseCapture();
    dragging_ = false;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
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
    const float w = kW, netH = NetworkHeight();
    D2D1_RECT_F card = D2D1::RectF(0, 0, w, netH);
    D2D1_RECT_F volume = D2D1::RectF(0, netH + kGap, w, netH + kGap + barH_);
    D2D1_RECT_F battery = D2D1::RectF(0, volume.bottom + kGap, w, volume.bottom + kGap + barH_);
    std::vector<ShapeDip> shapes{{card, radius_}, {volume, barH_ / 2}};
    if (status_.battery)
        shapes.push_back({battery, barH_ / 2});
    if (c.panel)
        for (const ShapeDip& sh : shapes)
            c.Fill(sh.rect, sh.radius, t.panel);
    surface_.SetShapes(shapes);
    PaintNetwork(c, t, card);
    PaintVolume(c, t, volume);
    if (status_.battery)
        PaintBattery(c, t, battery);
}

void ControlCenter::PaintNetwork(Canvas& c, const Theme& t, D2D1_RECT_F card)
{
    const float w = card.right - card.left;
    // Header: the connection.
    D2D1_RECT_F disc = D2D1::RectF(16, 14, 52, 50);
    bool on = status_.connected;
    c.Circle(34, 32, 18, on ? Alpha(t.accent, 0.95f) : Alpha(t.hover, 1.6f));
    {
        bool halo = c.halo;
        c.halo = false;
        c.Glyph(!on ? L"\uEB55" : status_.wifi ? L"\uE701" : L"\uE839", disc, 16, on ? t.accentText : t.subtext);
        c.halo = halo;
    }
    std::wstring title = !on ? L"未连接" : private_ ? (status_.wifi ? L"Wi-Fi" : L"有线网络") : status_.network;
    std::wstring sub = !on ? L"选择下面的网络连接"
                           : (status_.wifi ? L"Wi-Fi · 信号 " + std::to_wstring(status_.signal) + L"%" : L"有线网络") +
                                 (status_.online ? L" · 可访问 Internet" : L" · 无 Internet");
    c.Text(title.empty() ? L"已连接" : title, D2D1::RectF(64, 12, w - 60, 34), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(sub, D2D1::RectF(64, 33, w - 60, 52), 12, t.subtext);
    D2D1_RECT_F rescan = D2D1::RectF(w - 52, 14, w - 16, 50);
    float rh = Hover(c, L"rescan", hover_ == L"rescan");
    c.Fill(rescan, 18, Alpha(pressed_ == L"rescan" ? t.press : t.hover, 0.4f + 0.6f * rh));
    c.Glyph(scanning_ ? L"\uE895" : L"\uE72C", rescan, 14, t.text);
    hits_.push_back({rescan, L"rescan"});
    c.Line(kPad + 8, kHeader, w - kPad - 8, kHeader, t.divider);

    // The networks in range.
    float y = kHeader;
    if (networks_.empty()) {
        std::wstring msg = scanning_ && !scanned_ ? L"正在搜索网络…" : scanned_ ? L"没有找到 Wi-Fi 网络" : L"正在搜索网络…";
        c.Text(msg, D2D1::RectF(kPad, y, w - kPad, y + kRowH), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += kRowH;
    }
    for (int i = 0; i < Rows() && i < int(networks_.size()); ++i) {
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
        if (n.secured) {
            c.Glyph(L"\uE72E", D2D1::RectF(row.right - 112, row.top, row.right - 92, row.bottom), 11, t.faint);
        }
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

    // Footer: the system's pages.
    c.Line(kPad + 8, y + 2, w - kPad - 8, y + 2, t.divider);
    auto link = [&](const std::wstring& id, const std::wstring& label, float left, float right, int align) {
        D2D1_RECT_F b = D2D1::RectF(left, y + 8, right, y + kFooter - 2);
        float hv = Hover(c, id, hover_ == id);
        if (hv > 0.001f)
            c.Fill(b, (b.bottom - b.top) / 2, Alpha(t.hover, hv));
        c.Text(label, D2D1::RectF(b.left + 12, b.top, b.right - 12, b.bottom), 12.5f, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, align);
        hits_.push_back({b, id});
    };
    link(L"netsettings", L"网络和 Internet 设置", kPad, w / 2 + 20, 0);
    link(L"netlist", L"更多 Wi-Fi 网络", w / 2 + 28, w - kPad, 2);
}

void ControlCenter::PaintVolume(Canvas& c, const Theme& t, D2D1_RECT_F cap)
{
    const float h = cap.bottom - cap.top, cy = (cap.top + cap.bottom) / 2, bh = std::min(44.f, h - 12);
    const float edge = (h - bh) / 2;
    D2D1_RECT_F mute = D2D1::RectF(cap.left + edge, cy - bh / 2, cap.left + edge + bh, cy + bh / 2);
    float mh = Hover(c, L"mute", hover_ == L"mute");
    if (mh > 0.001f || status_.muted)
        c.Fill(mute, bh / 2, Alpha(status_.muted ? t.press : t.hover, std::max(mh, status_.muted ? 1.f : 0.f)));
    const wchar_t* g = !status_.audio ? L"\uE74F" : status_.muted ? L"\uE74F" : status_.volume < 34 ? L"\uE993" : status_.volume < 67 ? L"\uE994" : L"\uE995";
    c.Glyph(g, mute, 16, status_.audio ? t.text : t.faint);
    hits_.push_back({mute, L"mute"});

    std::wstring pct = !status_.audio ? L"" : status_.muted ? L"静音" : std::to_wstring(status_.volume) + L"%";
    D2D1_RECT_F label = D2D1::RectF(cap.right - edge - 56, cap.top, cap.right - edge - 8, cap.bottom);
    track_ = D2D1::RectF(mute.right + 14, cy - 3, label.left - 10, cy + 3);
    if (!status_.audio) {
        c.Text(L"没有播放设备", D2D1::RectF(track_.left, cap.top, cap.right - edge, cap.bottom), 13, t.subtext);
        return;
    }
    float k = float(status_.volume) / 100.f, x = track_.left + (track_.right - track_.left) * k;
    float sh = Hover(c, L"slider", hover_ == L"slider" || dragging_);
    c.Fill(track_, 3, Alpha(t.text, 0.18f));
    c.Fill(D2D1::RectF(track_.left, track_.top, x, track_.bottom), 3, status_.muted ? Alpha(t.text, 0.4f) : t.accent);
    float knob = 8 + 2 * sh;
    c.Circle(x, cy, knob + 1, Alpha(D2D1::ColorF(0, 0, 0), 0.18f));
    c.Circle(x, cy, knob, D2D1::ColorF(1, 1, 1, 0.98f));
    c.Text(pct, label, 13, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    hits_.push_back({D2D1::RectF(track_.left - 10, cap.top + 6, track_.right + 10, cap.bottom - 6), L"slider"});
}

void ControlCenter::PaintBattery(Canvas& c, const Theme& t, D2D1_RECT_F cap)
{
    const float h = cap.bottom - cap.top, cy = (cap.top + cap.bottom) / 2;
    float hv = Hover(c, L"battery", hover_ == L"battery");
    if (hv > 0.001f)
        c.Fill(D2D1::RectF(cap.left + 6, cap.top + 6, cap.right - 6, cap.bottom - 6), (h - 12) / 2, Alpha(t.hover, hv));
    // The battery, drawn like the bar's icon, larger.
    const float bx = cap.left + 22, bw = 34, bh = 17;
    D2D1_RECT_F body = D2D1::RectF(bx, cy - bh / 2, bx + bw, cy + bh / 2);
    c.Stroke(body, 5, Alpha(t.text, 0.5f), 1.4f);
    c.Fill(D2D1::RectF(body.right + 1.5f, cy - 3.5f, body.right + 4, cy + 3.5f), 1.2f, Alpha(t.text, 0.5f));
    int pct = std::clamp(status_.batteryPercent, 0, 100);
    D2D1_RECT_F fill = D2D1::RectF(body.left + 3, body.top + 3, body.left + 3 + (bw - 6) * float(pct) / 100.f, body.bottom - 3);
    c.Fill(fill, 2.5f, status_.charging ? D2D1::ColorF(0x30D158) : pct <= 20 ? D2D1::ColorF(0xFF453A) : t.text);
    c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(bx + bw + 16, cap.top, bx + bw + 76, cap.bottom), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::wstring state;
    if (status_.charging)
        state = pct >= 100 ? L"已充满 · 已接通电源" : L"正在充电";
    else if (status_.batteryMinutes >= 0)
        state = L"剩余 " + (status_.batteryMinutes >= 60 ? std::to_wstring(status_.batteryMinutes / 60) + L" 小时 " : L"") +
                std::to_wstring(status_.batteryMinutes % 60) + L" 分钟";
    else
        state = L"使用电池";
    c.Text(state, D2D1::RectF(bx + bw + 78, cap.top, cap.right - 44, cap.bottom), 12.5f, t.subtext);
    c.Glyph(L"\uE76C", D2D1::RectF(cap.right - 44, cap.top, cap.right - 16, cap.bottom), 11, t.faint);
    hits_.push_back({cap, L"battery"});
}

std::wstring ControlCenter::HitAt(POINT pt) const
{
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    // Small buttons first (the disconnect button sits on its row).
    for (const Hit& h : hits_)
        if (h.id == L"disconnect" && In(h.rect, x, y))
            return h.id;
    for (const Hit& h : hits_)
        if (In(h.rect, x, y))
            return h.id;
    return {};
}

void ControlCenter::SetVolumeAt(float x)
{
    if (track_.right <= track_.left)
        return;
    int v = int(std::lround(std::clamp((x - track_.left) / (track_.right - track_.left), 0.f, 1.f) * 100));
    SetVolume(v);
    status_.volume = v;
    if (v > 0)
        status_.muted = false;
    surface_.Invalidate();
}

void ControlCenter::Activate(const std::wstring& id)
{
    if (id == L"rescan") {
        Scan(true);
        surface_.Invalidate();
    } else if (id == L"mute") {
        ToggleMute();
        status_ = QuerySystemStatus();
        surface_.Invalidate();
        if (onChanged)
            onChanged();
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
    } else if (id == L"battery") {
        Close();
        OpenUri(L"ms-settings:batterysaver");
    }
    Log(L"control: %s", id.rfind(L"net:", 0) == 0 ? L"connect" : id.c_str());
}

void ControlCenter::TestClick(const std::wstring& id) { Activate(id); }

void ControlCenter::LogState() const
{
    const RECT r = surface_.Rect();
    const float s = surface_.Scale();
    std::wstring spots;
    for (const Hit& h : hits_)
        if (h.id.rfind(L"net:", 0) != 0 || h.id == L"net:0")
            spots += L" " + h.id + L"@" + std::to_wstring(int(float(r.left) + (h.rect.left + h.rect.right) / 2 * s)) + L"," +
                     std::to_wstring(int(float(r.top) + (h.rect.top + h.rect.bottom) / 2 * s));
    Log(L"control: spots%s", spots.c_str());
    Log(L"control: state visible=%d connected=%d wifi=%d signal=%d networks=%zu scanning=%d volume=%d muted=%d battery=%d percent=%d shapes=%zu "
        L"rect=%ld,%ld,%ld,%ld",
        surface_.Visible() ? 1 : 0, status_.connected ? 1 : 0, status_.wifi ? 1 : 0, status_.signal, networks_.size(), scanning_ ? 1 : 0, status_.volume,
        status_.muted ? 1 : 0, status_.battery ? 1 : 0, status_.batteryPercent, surface_.Shapes(), r.left, r.top, r.right, r.bottom);
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
            if (!dragging_ && !(s == status_)) {
                bool resize = s.battery != status_.battery;
                status_ = s;
                if (resize)
                    Place();
                surface_.Invalidate();
            }
            return true;
        }
        return false;
    case WM_WIFI_LIST: {
        std::unique_ptr<std::vector<WifiNetwork>> list(reinterpret_cast<std::vector<WifiNetwork>*>(l));
        unsigned gen = unsigned(w);
        if (gen > acceptedGen_) {
            acceptedGen_ = gen;
            size_t rows = size_t(Rows());
            networks_ = std::move(*list);
            scanned_ = true;
            scanning_ = gen < scanGen_;
            if (size_t(Rows()) != rows && surface_.Visible())
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
        if (dragging_) {
            SetVolumeAt(float(pt.x) / surface_.Scale());
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
        if (!dragging_) {
            hover_.clear();
            pressed_.clear();
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        pressed_ = HitAt(pt);
        if (pressed_ == L"slider" && status_.audio) {
            dragging_ = true;
            SetCapture(surface_.Hwnd());
            SetVolumeAt(float(pt.x) / surface_.Scale());
        } else if (!pressed_.empty()) {
            float s = surface_.Scale();
            surface_.Ripple(float(pt.x) / s, float(pt.y) / s);
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            pressed_.clear();
            if (onChanged)
                onChanged();
            surface_.Invalidate();
            return true;
        }
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
        if (notches && status_.audio) {
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
        if (w == VK_ESCAPE) {
            Close();
            return true;
        }
        return false;
    }
    (void)result;
    return false;
}

} // namespace atrium
