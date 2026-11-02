#include "ui/desktop_widgets.h"

#include "clock.h"
#include "log.h"
#include "motion.h"
#include "settings.h"

#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>

#pragma comment(lib, "dwmapi.lib")

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr float kMargin = 24;
constexpr double kGlideMs = 240;
constexpr UINT_PTR kTickTimer = 1, kGlideTimer = 3, kSafetyTimer = 4, kCaretTimer = 5, kSaveTimer = 6;
constexpr UINT kCaretMs = 530, kSaveMs = 800;
constexpr ULONGLONG kPhotoMs = 10 * 60 * 1000; // a new photo every ten minutes
constexpr const wchar_t* kHostClass = L"AtriumWidgetsHost";
constexpr UINT WM_WIDGET_REMOVE = WM_APP + 1;  // lParam Widget*: removed once its own message is done
constexpr UINT WM_WIDGET_RESTACK = WM_APP + 2; // a burst of window events, one restack
constexpr UINT WM_WIDGET_PHOTO = WM_APP + 3;   // lParam PhotoRead*: a photo was read

DesktopWidgets* g_widgets = nullptr; // the WinEvent hook's way back

struct KindName {
    WidgetKind kind;
    const wchar_t* name;
};
const KindName kKinds[] = {{WidgetKind::Clock, L"clock"},     {WidgetKind::Calendar, L"calendar"},   {WidgetKind::Battery, L"battery"},
                           {WidgetKind::Digital, L"digital"}, {WidgetKind::System, L"system"},       {WidgetKind::Notes, L"notes"},
                           {WidgetKind::Reminders, L"reminders"}, {WidgetKind::Photos, L"photos"}};

const wchar_t* NameOf(WidgetKind k)
{
    for (const KindName& n : kKinds)
        if (n.kind == k)
            return n.name;
    return L"clock";
}
bool ParseKind(const std::wstring& s, WidgetKind& k)
{
    for (const KindName& n : kKinds)
        if (s == n.name) {
            k = n.kind;
            return true;
        }
    return false;
}
const wchar_t* SizeName(WidgetSize s) { return s == WidgetSize::Small ? L"small" : s == WidgetSize::Medium ? L"medium" : L"large"; }
WidgetSize ParseSize(const std::wstring& s) { return s == L"large" ? WidgetSize::Large : s == L"medium" ? WidgetSize::Medium : WidgetSize::Small; }
bool Editable(WidgetKind k) { return k == WidgetKind::Notes || k == WidgetKind::Reminders; }

std::wstring LayoutPath()
{
    std::wstring p = SettingsPath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\widgets.json";
}

std::wstring ClassOf(HWND h)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return cls;
}

bool IsDesktopWindow(HWND h)
{
    std::wstring c = ClassOf(h);
    return c == L"Progman" || c == L"WorkerW";
}

// The first city of the local time zone's name: "(UTC+08:00) 北京，重庆，…" -> "北京".
std::wstring LocalCity()
{
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID)
        return {};
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\" + std::wstring(tz.TimeZoneKeyName);
    wchar_t display[256] = {};
    DWORD size = sizeof(display);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Display", RRF_RT_REG_SZ, nullptr, display, &size) != ERROR_SUCCESS)
        return {};
    std::wstring d = display;
    size_t open = d.find(L") ");
    if (open != std::wstring::npos)
        d = d.substr(open + 2);
    size_t end = d.find_first_of(L"，,");
    return end == std::wstring::npos ? d : d.substr(0, end);
}

std::wstring PicturesFolder()
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

// The pictures of a folder and its subfolders (two levels, a few thousand at most).
void ListPhotos(const std::wstring& dir, std::vector<std::wstring>& out, int depth = 0)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 2)
                ListPhotos(dir + L"\\" + name, out, depth + 1);
            continue;
        }
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos)
            continue;
        std::wstring ext = name.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        for (const wchar_t* e : {L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".heic", L".tif", L".tiff"})
            if (ext == e) {
                out.push_back(dir + L"\\" + name);
                break;
            }
    } while (out.size() < 5000 && FindNextFileW(h, &fd));
    FindClose(h);
}

// A photo cropped to fill w x h px (centred) and turned upright, premultiplied BGRA.
IconImage ReadPhoto(const std::wstring& path, UINT w, UINT h)
{
    IconImage out;
    winrt::com_ptr<IWICImagingFactory> f;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()))) ||
        FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())))
        return out;
    winrt::com_ptr<IWICBitmapSource> src = frame;
    // The camera's orientation (EXIF): phone pictures are often stored sideways.
    WICBitmapTransformOptions turn = WICBitmapTransformRotate0;
    winrt::com_ptr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(meta.put()))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2)
            turn = v.uiVal == 3   ? WICBitmapTransformRotate180
                   : v.uiVal == 6 ? WICBitmapTransformRotate90
                   : v.uiVal == 8 ? WICBitmapTransformRotate270
                                  : WICBitmapTransformRotate0;
        PropVariantClear(&v);
    }
    if (turn != WICBitmapTransformRotate0) {
        winrt::com_ptr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(f->CreateBitmapFlipRotator(rot.put())) && SUCCEEDED(rot->Initialize(src.get(), turn)))
            src = rot;
    }
    UINT sw = 0, sh = 0;
    if (FAILED(src->GetSize(&sw, &sh)) || !sw || !sh)
        return out;
    double k = std::max(double(w) / sw, double(h) / sh);
    UINT cw = std::min(sw, UINT(std::lround(w / k))), ch = std::min(sh, UINT(std::lround(h / k)));
    WICRect crop{INT((sw - cw) / 2), INT((sh - ch) / 2), INT(cw), INT(ch)};
    winrt::com_ptr<IWICBitmapClipper> clip;
    winrt::com_ptr<IWICBitmapScaler> scale;
    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(f->CreateBitmapClipper(clip.put())) || FAILED(clip->Initialize(src.get(), &crop)) || FAILED(f->CreateBitmapScaler(scale.put())) ||
        FAILED(scale->Initialize(clip.get(), w, h, WICBitmapInterpolationModeHighQualityCubic)) || FAILED(f->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(scale.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return out;
    out.bgra.resize(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data()))) {
        out.bgra.clear();
        return out;
    }
    out.width = int(w);
    out.height = int(h);
    return out;
}

} // namespace

struct DesktopWidgets::Widget {
    WidgetKind kind = WidgetKind::Clock;
    WidgetSize size = WidgetSize::Small;
    int col = 0, row = 0;
    std::unique_ptr<Surface> surface;
    bool down = false, dragging = false;
    POINT downAt{};
    RECT downRect{};
    RECT glideFrom{}, glideTo{};
    double glideMs = 0;
    WidgetContent content;
    std::vector<WidgetHit> hits; // of the last paint
    ULONGLONG photoAt = 0;       // when the photo was last changed
    unsigned photoGen = 0;       // the photo being read
};

namespace {
struct PhotoRead {
    void* widget;
    unsigned gen;
    std::wstring path;
    IconImage image;
};
} // namespace

DesktopWidgets::DesktopWidgets(HINSTANCE instance) : instance_(instance)
{
    g_widgets = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    // A hidden top-level window: timers, display changes, the menu's owner.
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Atrium Widgets", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    world_.city = LocalCity();
    world_.lunar = &lunar_;
    wchar_t test[8] = {};
    test_ = GetEnvironmentVariableW(L"ATRIUM_WIDGETS_TEST", test, 8) && test[0] == L'1';
}

DesktopWidgets::~DesktopWidgets()
{
    SetEnabled(false);
    if (host_)
        DestroyWindow(host_);
    g_widgets = nullptr;
}

void DesktopWidgets::SetEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (on) {
        world_.status = QuerySystemStatus();
        SampleSystem();
        Load();
        Build();
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0, 0, flags);
        // A window that appears without taking the foreground, or uncloaks
        // after it did (WinUI), must also send raised widgets back down.
        showHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnWinEvent, 0, 0, flags);
        uncloakHook_ = SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnWinEvent, 0, 0, flags);
        SetTimer(host_, kTickTimer, 1000, nullptr);
        SetTimer(host_, kSafetyTimer, 3000, nullptr);
        Restack();
        Log(L"widgets: on (%zu)", widgets_.size());
    } else {
        for (HWINEVENTHOOK* h : {&hook_, &showHook_, &uncloakHook_}) {
            if (*h)
                UnhookWinEvent(*h);
            *h = nullptr;
        }
        KillTimer(host_, kTickTimer);
        KillTimer(host_, kSafetyTimer);
        KillTimer(host_, kCaretTimer);
        if (KillTimer(host_, kSaveTimer))
            Save(); // typing not saved yet
        Clear();
        Log(L"widgets: off");
    }
}

void DesktopWidgets::SetMonitor(HMONITOR monitor)
{
    if (monitor == monitor_)
        return;
    monitor_ = monitor;
    if (enabled_)
        for (auto& w : widgets_)
            Place(*w, false);
}

// ---- layout ------------------------------------------------------------------

RECT DesktopWidgets::WorkArea() const
{
    MONITORINFO mi{sizeof(mi)};
    HMONITOR m = monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(m, &mi);
    return mi.rcWork;
}

float DesktopWidgets::Scale() const { return PopupScale(WorkArea()); }

int DesktopWidgets::Cols() const
{
    RECT wa = WorkArea();
    return std::max(1, int((float(wa.right - wa.left) / Scale() - 2 * kMargin + kWidgetGap) / (kWidgetCell + kWidgetGap)));
}

int DesktopWidgets::Rows() const
{
    RECT wa = WorkArea();
    // The bar floats over the bottom of the work area: leave it a row's worth of room.
    return std::max(1, int((float(wa.bottom - wa.top) / Scale() - 2 * kMargin - 90 + kWidgetGap) / (kWidgetCell + kWidgetGap)));
}

RECT DesktopWidgets::CellRect(int col, int row, WidgetSize size) const
{
    RECT wa = WorkArea();
    float s = Scale();
    int x = wa.left + int(std::lround((kMargin + float(col) * (kWidgetCell + kWidgetGap)) * s));
    int y = wa.top + int(std::lround((kMargin + float(row) * (kWidgetCell + kWidgetGap)) * s));
    return RECT{x, y, x + int(std::lround(WidgetWidth(size) * s)), y + int(std::lround(WidgetHeight(size) * s))};
}

bool DesktopWidgets::Fits(int col, int row, WidgetSize size, const Widget* except) const
{
    if (col < 0 || row < 0 || col + WidgetCols(size) > Cols() || row + WidgetRows(size) > Rows())
        return false;
    for (const auto& w : widgets_) {
        if (w.get() == except)
            continue;
        bool apart = col + WidgetCols(size) <= w->col || w->col + WidgetCols(w->size) <= col || row + WidgetRows(size) <= w->row ||
                     w->row + WidgetRows(w->size) <= row;
        if (!apart)
            return false;
    }
    return true;
}

bool DesktopWidgets::FreeCellNear(float col, float row, WidgetSize size, const Widget* except, int& outCol, int& outRow) const
{
    float best = 1e9f;
    for (int r = 0; r < Rows(); ++r)
        for (int c = 0; c < Cols(); ++c) {
            if (!Fits(c, r, size, except))
                continue;
            float d = (float(c) - col) * (float(c) - col) + (float(r) - row) * (float(r) - row);
            if (d < best) {
                best = d;
                outCol = c;
                outRow = r;
            }
        }
    return best < 1e9f;
}

void DesktopWidgets::Load()
{
    widgets_.clear();
    unplaced_.clear();
    std::wstring text = ReadTextFile(LayoutPath());
    bool read = false;
    auto add = [&](WidgetKind k, WidgetSize s, int c, int r, WidgetContent content) {
        if (!WidgetSupports(k, s))
            s = WidgetSize::Small;
        if (!Fits(c, r, s, nullptr) && !FreeCellNear(float(c), float(r), s, nullptr, c, r))
            return false; // no room left on this screen
        Create(k, s, c, r).content = std::move(content);
        return true;
    };
    if (!text.empty()) {
        json::JsonObject o{nullptr};
        if (json::JsonObject::TryParse(text, o) && o.HasKey(L"widgets")) {
            read = true;
            for (const auto& v : o.GetNamedArray(L"widgets")) {
                if (v.ValueType() != json::JsonValueType::Object)
                    continue;
                json::JsonObject e = v.GetObject();
                WidgetKind k;
                if (!ParseKind(std::wstring(e.GetNamedString(L"kind", L"")), k))
                    continue;
                WidgetContent content;
                content.text = e.GetNamedString(L"text", L"");
                content.folder = e.GetNamedString(L"folder", L"");
                if (e.HasKey(L"items") && e.GetNamedValue(L"items").ValueType() == json::JsonValueType::Array)
                    for (const auto& iv : e.GetNamedArray(L"items")) {
                        if (iv.ValueType() != json::JsonValueType::Object)
                            continue;
                        json::JsonObject io = iv.GetObject();
                        content.items.push_back({std::wstring(io.GetNamedString(L"text", L"")), io.GetNamedBoolean(L"done", false)});
                    }
                if (!add(k, ParseSize(std::wstring(e.GetNamedString(L"size", L"small"))), int(e.GetNamedNumber(L"col", 0)),
                         int(e.GetNamedNumber(L"row", 0)), std::move(content)))
                    unplaced_.emplace_back(e.Stringify());
            }
        } else {
            Log(L"widgets: %s is not valid, starting afresh", LayoutPath().c_str());
        }
    }
    if (!read) { // first run: a column of widgets at the top left, as macOS starts
        add(WidgetKind::Clock, WidgetSize::Small, 0, 0, {});
        add(WidgetKind::Battery, WidgetSize::Small, 1, 0, {});
        add(WidgetKind::Calendar, WidgetSize::Medium, 0, 1, {});
        add(WidgetKind::Clock, WidgetSize::Medium, 0, 2, {});
    }
}

void DesktopWidgets::Save() const
{
    json::JsonArray list;
    for (const auto& w : widgets_) {
        json::JsonObject e;
        e.SetNamedValue(L"kind", json::JsonValue::CreateStringValue(NameOf(w->kind)));
        e.SetNamedValue(L"size", json::JsonValue::CreateStringValue(SizeName(w->size)));
        e.SetNamedValue(L"col", json::JsonValue::CreateNumberValue(w->col));
        e.SetNamedValue(L"row", json::JsonValue::CreateNumberValue(w->row));
        const WidgetContent& c = w->content;
        if (w->kind == WidgetKind::Notes)
            e.SetNamedValue(L"text", json::JsonValue::CreateStringValue(c.text));
        if (w->kind == WidgetKind::Reminders) {
            json::JsonArray items;
            for (const Reminder& r : c.items) {
                if (r.text.empty())
                    continue; // one being typed
                json::JsonObject io;
                io.SetNamedValue(L"text", json::JsonValue::CreateStringValue(r.text));
                io.SetNamedValue(L"done", json::JsonValue::CreateBooleanValue(r.done));
                items.Append(io);
            }
            e.SetNamedValue(L"items", items);
        }
        if (w->kind == WidgetKind::Photos && !c.folder.empty())
            e.SetNamedValue(L"folder", json::JsonValue::CreateStringValue(c.folder));
        list.Append(e);
    }
    for (const auto& s : unplaced_)
        if (json::JsonObject e{nullptr}; json::JsonObject::TryParse(s, e))
            list.Append(e);
    json::JsonObject o;
    o.SetNamedValue(L"widgets", list);
    if (!WriteTextFile(LayoutPath(), std::wstring(o.Stringify())))
        Log(L"widgets: could not save the layout");
}

void DesktopWidgets::SaveSoon() { SetTimer(host_, kSaveTimer, kSaveMs, nullptr); }

// ---- windows -----------------------------------------------------------------

DesktopWidgets::Widget& DesktopWidgets::Create(WidgetKind kind, WidgetSize size, int col, int row)
{
    auto w = std::make_unique<Widget>();
    w->kind = kind;
    w->size = size;
    w->col = col;
    w->row = row;
    widgets_.push_back(std::move(w));
    return *widgets_.back();
}

bool DesktopWidgets::Has(const Widget* w) const
{
    return std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& p) { return p.get() == w; });
}

void DesktopWidgets::Build()
{
    for (auto& wp : widgets_) {
        Widget& w = *wp;
        if (w.surface)
            continue;
        // Notes and reminders take the keyboard: their windows can be activated.
        w.surface = std::make_unique<Surface>(instance_, L"Atrium Widget", Editable(w.kind), 14.f);
        w.surface->SetDesktopLayer(true);
        w.surface->SetRadius(kWidgetRadius);
        w.surface->paint = [this, &w](Canvas& c) { Paint(w, c); };
        w.surface->message = [this, &w](UINT m, WPARAM wp2, LPARAM lp, LRESULT& r) { return Message(w, m, wp2, lp, r); };
        Place(w, false);
        if (w.kind == WidgetKind::Photos)
            LoadPhoto(w, true);
    }
}

void DesktopWidgets::Clear()
{
    for (auto& w : widgets_)
        if (w->surface)
            w->surface->Hide(false);
    widgets_.clear();
}

void DesktopWidgets::Place(Widget& w, bool glide)
{
    if (!w.surface)
        return;
    RECT to = CellRect(w.col, w.row, w.size);
    if (!w.surface->Visible()) {
        w.surface->Show(to, false, POINT{(to.left + to.right) / 2, (to.top + to.bottom) / 2}, true);
        Restack();
        return;
    }
    if (!glide) {
        w.glideMs = 0;
        w.surface->Move(to);
        return;
    }
    w.glideFrom = w.surface->Rect();
    w.glideTo = to;
    w.glideMs = ClockMs();
    SetTimer(host_, kGlideTimer, 8, nullptr);
}

void DesktopWidgets::Glide()
{
    bool any = false;
    double now = ClockMs();
    for (auto& w : widgets_) {
        if (w->glideMs <= 0 || !w->surface)
            continue;
        float k = motion::EaseOut(float((now - w->glideMs) / kGlideMs));
        // Size changes too (a widget made larger or smaller).
        auto lerp = [&](LONG a, LONG b) { return LONG(std::lround(float(a) + float(b - a) * k)); };
        RECT r{lerp(w->glideFrom.left, w->glideTo.left), lerp(w->glideFrom.top, w->glideTo.top), lerp(w->glideFrom.right, w->glideTo.right),
               lerp(w->glideFrom.bottom, w->glideTo.bottom)};
        w->surface->Move(r);
        if (k >= 1)
            w->glideMs = 0;
        else
            any = true;
    }
    if (!any)
        KillTimer(host_, kGlideTimer);
}

bool DesktopWidgets::Add(WidgetKind kind, WidgetSize size)
{
    if (!WidgetSupports(kind, size))
        size = WidgetSize::Small;
    int c = 0, r = 0;
    if (!FreeCellNear(0, 0, size, nullptr, c, r)) {
        Log(L"widgets: no room for another");
        return false;
    }
    Create(kind, size, c, r);
    if (enabled_)
        Build();
    Save();
    Log(L"widgets: added %s %s at %d,%d", NameOf(kind), SizeName(size), c, r);
    return true;
}

bool DesktopWidgets::AddNamed(const std::wstring& kind, const std::wstring& size)
{
    WidgetKind k;
    return ParseKind(kind, k) && Add(k, ParseSize(size));
}

void DesktopWidgets::Remove(Widget& w)
{
    // Its window may be handling the message that asked for this.
    PostMessageW(host_, WM_WIDGET_REMOVE, 0, reinterpret_cast<LPARAM>(&w));
}

void DesktopWidgets::RemoveNow(Widget& w)
{
    auto it = std::find_if(widgets_.begin(), widgets_.end(), [&](const auto& p) { return p.get() == &w; });
    if (it == widgets_.end())
        return;
    std::unique_ptr<Widget> gone = std::move(*it);
    widgets_.erase(it);
    if (gone->surface)
        gone->surface->Hide(false);
    Save();
    Log(L"widgets: removed %s", NameOf(gone->kind));
}

void DesktopWidgets::Resize(Widget& w, WidgetSize size)
{
    if (w.size == size || !WidgetSupports(w.kind, size))
        return;
    int c = w.col, r = w.row;
    if (!Fits(c, r, size, &w) && !FreeCellNear(float(c), float(r), size, &w, c, r))
        return;
    w.size = size;
    w.col = c;
    w.row = r;
    Place(w, true);
    w.surface->Invalidate();
    if (w.kind == WidgetKind::Photos)
        LoadPhoto(w, false); // the same photo, cropped for the new size
    Save();
}

bool DesktopWidgets::IsOurs(HWND h) const
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// ---- the layer -----------------------------------------------------------------

void CALLBACK DesktopWidgets::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND h, LONG obj, LONG child, DWORD, DWORD)
{
    if (!g_widgets || obj != OBJID_WINDOW || child != CHILDID_SELF)
        return;
    // Shown or uncloaked windows matter only to widgets over the desktop, and
    // only top-level ones (menus, tooltips and child windows show all the time).
    bool appeared = event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED;
    if (appeared && (!g_widgets->raised_ || !h || GetAncestor(h, GA_ROOT) != h))
        return;
    if (!g_widgets->restackPosted_) {
        g_widgets->restackPosted_ = true; // a burst of events, one restack right after it
        PostMessageW(g_widgets->host_, WM_WIDGET_RESTACK, 0, 0);
    }
}

// Just above the desktop, under every window; over it when every app is
// minimised (Win+D raises the desktop over windows at the bottom, D14). A
// note or reminders being typed into stays where its click put it.
void DesktopWidgets::Restack()
{
    if (!enabled_ || widgets_.empty())
        return;
    if (std::any_of(widgets_.begin(), widgets_.end(), [](const auto& w) { return w->dragging; }))
        return; // the one being dragged stays over the windows
    if (test_) {
        // Screenshots: over the test backdrop.
        for (auto& w : widgets_)
            if (w->surface)
                SetWindowPos(w->surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    HWND fg = GetForegroundWindow();
    // Atrium's own windows (the bar, its menus) and the taskbar change nothing.
    bool neutral = fg && (IsOurs(fg) || ClassOf(fg) == L"Shell_TrayWnd" || ClassOf(fg) == L"Shell_SecondaryTrayWnd");
    // Every app on this screen minimised (or none), and none in front.
    struct Scan {
        const DesktopWidgets* self;
        HMONITOR monitor;
        bool open = false;
    } scan{this, monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* s = reinterpret_cast<Scan*>(l);
            if (!IsWindowVisible(h) || IsIconic(h) || s->self->IsOurs(h) || IsDesktopWindow(h))
                return TRUE;
            LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if ((ex & WS_EX_TOOLWINDOW) || (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) || GetWindowTextLengthW(h) == 0)
                return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            std::wstring c = ClassOf(h);
            if (cloaked || c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd" || c == L"Windows.UI.Core.CoreWindow")
                return TRUE;
            if (MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != s->monitor)
                return TRUE;
            s->open = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&scan));
    bool raise = !scan.open && (!fg || IsDesktopWindow(fg) || neutral);
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (raise) {
        for (auto& w : widgets_)
            if (w->surface)
                SetWindowPos(w->surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, flags);
    } else {
        // The window just above the desktop's top window (Progman, or the
        // WorkerW with the icons); the widgets go right under it.
        HWND above = nullptr, h = GetTopWindow(nullptr);
        bool found = false;
        for (; h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (IsWindowVisible(h) && IsDesktopWindow(h)) {
                found = true;
                break;
            }
            bool widget = std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& w) { return w->surface && w->surface->Hwnd() == h; });
            if (!widget)
                above = h;
        }
        if (!found)
            above = nullptr; // no desktop window: keep them at the bottom of the windows
        HWND after = found ? (above ? above : HWND_TOP) : HWND_BOTTOM;
        for (auto& w : widgets_) {
            if (!w->surface || w->content.editing)
                continue;
            // Placed after a window that is not topmost, a topmost one stops being topmost.
            SetWindowPos(w->surface->Hwnd(), after, 0, 0, 0, 0, flags);
            after = w->surface->Hwnd();
        }
    }
    raised_ = raise;
}

LRESULT CALLBACK DesktopWidgets::HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<DesktopWidgets*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        switch (m) {
        case WM_TIMER:
            if (w == kTickTimer) {
                self->Tick();
            } else if (w == kSafetyTimer) {
                self->Restack();
            } else if (w == kGlideTimer) {
                self->Glide();
            } else if (w == kSaveTimer) {
                KillTimer(h, kSaveTimer);
                self->Save();
            } else if (w == kCaretTimer) {
                bool any = false;
                for (auto& wd : self->widgets_)
                    if (wd->content.editing) {
                        wd->content.caretOn = !wd->content.caretOn;
                        wd->surface->Invalidate();
                        any = true;
                    }
                if (!any)
                    KillTimer(h, kCaretTimer);
            }
            return 0;
        case WM_WIDGET_RESTACK:
            self->restackPosted_ = false;
            self->Restack();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            if (self->enabled_)
                for (auto& wd : self->widgets_)
                    self->Place(*wd, false);
            return 0;
        case WM_SETTINGCHANGE:
            if (w == SPI_SETWORKAREA && self->enabled_)
                for (auto& wd : self->widgets_)
                    self->Place(*wd, false);
            return 0;
        case WM_WIDGET_REMOVE: {
            auto* gone = reinterpret_cast<Widget*>(l);
            if (self->Has(gone))
                self->RemoveNow(*gone);
            return 0;
        }
        case WM_WIDGET_PHOTO: {
            std::unique_ptr<PhotoRead> read(reinterpret_cast<PhotoRead*>(l));
            auto* wd = static_cast<Widget*>(read->widget);
            if (!self->Has(wd) || read->gen != wd->photoGen || !wd->surface)
                return 0; // removed, or a newer photo is on its way
            WidgetContent& c = wd->content;
            if (!c.imageKey.empty())
                wd->surface->ForgetImage(c.imageKey);
            c.photo = read->path;
            c.image = read->image.empty() ? nullptr : std::make_shared<const IconImage>(std::move(read->image));
            c.imageKey = L"photo:" + std::to_wstring(read->gen);
            wd->photoAt = GetTickCount64();
            wd->surface->Invalidate();
            return 0;
        }
        case WM_TIMECHANGE:
            self->world_.city = LocalCity();
            self->Tick();
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void DesktopWidgets::Tick()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    bool minute = t.wMinute != lastMinute_;
    lastMinute_ = t.wMinute;
    if (minute || t.wSecond % 30 == 0)
        world_.status = QuerySystemStatus();
    bool sample = t.wSecond % 2 == 0;
    if (sample)
        SampleSystem();
    ULONGLONG now = GetTickCount64();
    for (auto& w : widgets_) {
        if (!w->surface)
            continue;
        if (w->kind == WidgetKind::Clock || minute || (sample && w->kind == WidgetKind::System))
            w->surface->Invalidate();
        if (w->kind == WidgetKind::Photos && w->photoAt && now - w->photoAt > kPhotoMs)
            LoadPhoto(*w, true);
    }
}

void DesktopWidgets::SampleSystem()
{
    auto u64 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        uint64_t i = u64(idle), total = u64(kernel) + u64(user); // kernel time includes idle time
        if (cpuTotal_ && total > cpuTotal_)
            world_.cpu = std::clamp(1.f - float(double(i - cpuIdle_) / double(total - cpuTotal_)), 0.f, 1.f);
        cpuIdle_ = i;
        cpuTotal_ = total;
    }
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) {
        constexpr double kGb = 1024.0 * 1024 * 1024;
        world_.memory = float(ms.dwMemoryLoad) / 100.f;
        world_.memoryTotalGb = double(ms.ullTotalPhys) / kGb;
        world_.memoryGb = double(ms.ullTotalPhys - ms.ullAvailPhys) / kGb;
    }
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    std::wstring root = std::wstring(windows).substr(0, 3); // "C:\"
    ULARGE_INTEGER avail{}, total{}, free{};
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &free) && total.QuadPart) {
        world_.disk = 1.f - float(double(free.QuadPart) / double(total.QuadPart));
        world_.diskFreeGb = double(avail.QuadPart) / (1024.0 * 1024 * 1024);
    }
}

// ---- photos ------------------------------------------------------------------

void DesktopWidgets::PickFolder(Widget& w)
{
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return;
    FILEOPENDIALOGOPTIONS o = 0;
    dialog->GetOptions(&o);
    dialog->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"选择照片文件夹");
    Widget* wp = &w;
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(host_)) || FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    if (Has(wp)) { // the dialog ran its own message loop
        wp->content.folder = path;
        Save();
        LoadPhoto(*wp, true);
        Log(L"widgets: photos from a chosen folder");
    }
    CoTaskMemFree(path);
}

// another: a different picture of the folder; else the one shown, read again (a new size).
void DesktopWidgets::LoadPhoto(Widget& w, bool another)
{
    if (!w.surface)
        return;
    std::wstring folder = w.content.folder.empty() ? PicturesFolder() : w.content.folder;
    std::wstring current = w.content.photo;
    float s = Scale();
    UINT pw = UINT(std::lround(WidgetWidth(w.size) * s)), ph = UINT(std::lround(WidgetHeight(w.size) * s));
    unsigned gen = w.photoGen = ++photoGen_;
    w.photoAt = GetTickCount64(); // no second request while this one runs
    HWND host = host_;
    void* widget = &w;
    std::thread([=] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring path = current;
        if (another || path.empty()) {
            std::vector<std::wstring> all;
            ListPhotos(folder, all);
            if (all.size() > 1)
                all.erase(std::remove(all.begin(), all.end(), current), all.end());
            path.clear();
            if (!all.empty()) {
                std::mt19937 rng{std::random_device{}()};
                path = all[std::uniform_int_distribution<size_t>(0, all.size() - 1)(rng)];
            }
        }
        auto* read = new PhotoRead{widget, gen, path, path.empty() ? IconImage{} : ReadPhoto(path, pw, ph)};
        CoUninitialize();
        if (!PostMessageW(host, WM_WIDGET_PHOTO, 0, reinterpret_cast<LPARAM>(read)))
            delete read;
    }).detach();
}

// ---- input -------------------------------------------------------------------

bool DesktopWidgets::Message(Widget& w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r)
{
    (void)r;
    switch (m) {
    case WM_LBUTTONDOWN:
        w.down = true;
        w.dragging = false;
        GetCursorPos(&w.downAt);
        w.downRect = w.surface->Rect();
        SetCapture(w.surface->Hwnd());
        return true;
    case WM_MOUSEMOVE: {
        if (!w.down)
            return true;
        POINT p;
        GetCursorPos(&p);
        int dx = p.x - w.downAt.x, dy = p.y - w.downAt.y;
        if (!w.dragging && std::abs(dx) + std::abs(dy) > int(6 * Scale())) {
            w.dragging = true;
            w.glideMs = 0;
            EndEdit(w);
            // Over the windows while it moves, as macOS lifts a dragged widget.
            SetWindowPos(w.surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (w.dragging)
            w.surface->Move(RECT{w.downRect.left + dx, w.downRect.top + dy, w.downRect.right + dx, w.downRect.bottom + dy});
        return true;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
        if (!w.down)
            return m == WM_LBUTTONUP;
        w.down = false;
        bool dragged = w.dragging;
        w.dragging = false;
        if (m == WM_LBUTTONUP && GetCapture() == w.surface->Hwnd())
            ReleaseCapture();
        if (dragged) {
            // Snaps to the nearest free place of the grid and glides there.
            RECT cur = w.surface->Rect(), wa = WorkArea();
            float s = Scale();
            float col = (float(cur.left - wa.left) / s - kMargin) / (kWidgetCell + kWidgetGap);
            float row = (float(cur.top - wa.top) / s - kMargin) / (kWidgetCell + kWidgetGap);
            int c = w.col, rr = w.row;
            if (FreeCellNear(col, row, w.size, &w, c, rr)) {
                w.col = c;
                w.row = rr;
            }
            Place(w, true);
            Save();
            Restack();
            Log(L"widgets: %s moved to %d,%d", NameOf(w.kind), w.col, w.row);
        } else if (m == WM_LBUTTONUP) {
            if (Editable(w.kind))
                Click(w, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            else
                Activate(w);
        }
        return true;
    }
    case WM_RBUTTONUP:
        Menu(w);
        return true;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        return true;
    case WM_CHAR:
    case WM_KEYDOWN:
        return w.content.editing && Key(w, m, wp);
    case WM_KILLFOCUS:
        EndEdit(w);
        return false;
    case WM_IME_STARTCOMPOSITION:
        PlaceImeWindow(w);
        return false; // the input method's own window
    }
    return false;
}

void DesktopWidgets::Activate(Widget& w)
{
    POINT p;
    GetCursorPos(&p);
    POINT c = w.surface->ScreenToContent(p);
    float s = w.surface->Scale();
    w.surface->Ripple(float(c.x) / s, float(c.y) / s);
    std::wstring open;
    switch (w.kind) {
    case WidgetKind::Clock:
    case WidgetKind::Digital:
        open = L"ms-clock:";
        break;
    case WidgetKind::Calendar:
        if (onCalendar)
            onCalendar();
        break;
    case WidgetKind::Battery:
        open = world_.status.battery ? L"ms-settings:batterysaver" : L"ms-settings:powersleep";
        break;
    case WidgetKind::System:
        open = L"taskmgr.exe";
        break;
    case WidgetKind::Photos:
        if (w.content.photo.empty())
            PickFolder(w);
        else
            open = w.content.photo;
        break;
    default:
        break;
    }
    if (!open.empty())
        ShellExecuteW(nullptr, L"open", open.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"widgets: %s opened", NameOf(w.kind));
}

void DesktopWidgets::Click(Widget& w, POINT content)
{
    if (w.kind == WidgetKind::Notes) {
        StartEdit(w, -1);
        return;
    }
    float s = w.surface->Scale();
    D2D1_POINT_2F p = D2D1::Point2F(float(content.x) / s, float(content.y) / s);
    std::wstring id;
    for (const WidgetHit& h : w.hits)
        if (h.id != L"caret" && p.x >= h.rect.left && p.x < h.rect.right && p.y >= h.rect.top && p.y < h.rect.bottom) {
            id = h.id;
            break;
        }
    WidgetContent& c = w.content;
    if (id.rfind(L"toggle:", 0) == 0) {
        size_t i = size_t(_wtoi(id.c_str() + 7));
        if (c.editing && c.item == int(i))
            EndEdit(w);
        if (i < c.items.size() && !c.items[i].text.empty()) {
            c.items[i].done = !c.items[i].done;
            SaveSoon();
        }
        w.surface->Invalidate();
    } else if (id.rfind(L"item:", 0) == 0) {
        StartEdit(w, _wtoi(id.c_str() + 5));
    } else if (id == L"add") {
        EndEdit(w);
        c.items.push_back({});
        StartEdit(w, int(c.items.size()) - 1);
    } else {
        EndEdit(w);
    }
}

void DesktopWidgets::StartEdit(Widget& w, int item)
{
    WidgetContent& c = w.content;
    if (c.editing && c.item != item) {
        // Another reminder: an empty one being left goes, which may move this one up.
        if (w.kind == WidgetKind::Reminders && c.item >= 0 && c.item < item && c.items[size_t(c.item)].text.empty())
            --item;
        EndEdit(w);
    }
    if (w.kind == WidgetKind::Reminders && (item < 0 || item >= int(c.items.size())))
        return;
    c.editing = true;
    c.item = item;
    c.caret = w.kind == WidgetKind::Notes ? c.text.size() : c.items[size_t(item)].text.size();
    c.caretOn = true;
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr);
    if (GetForegroundWindow() != w.surface->Hwnd())
        SetForegroundWindow(w.surface->Hwnd());
    SetFocus(w.surface->Hwnd());
    w.surface->Invalidate();
}

void DesktopWidgets::EndEdit(Widget& w)
{
    WidgetContent& c = w.content;
    if (!c.editing)
        return;
    c.editing = false;
    if (w.kind == WidgetKind::Reminders && c.item >= 0 && c.item < int(c.items.size()) && c.items[size_t(c.item)].text.empty())
        c.items.erase(c.items.begin() + c.item); // nothing was typed
    c.item = -1;
    KillTimer(host_, kSaveTimer);
    Save();
    if (w.surface)
        w.surface->Invalidate();
    PostMessageW(host_, WM_WIDGET_RESTACK, 0, 0); // back under the windows
}

void DesktopWidgets::Insert(Widget& w, const std::wstring& s)
{
    WidgetContent& c = w.content;
    std::wstring& text = w.kind == WidgetKind::Notes ? c.text : c.items[size_t(c.item)].text;
    std::wstring add = s;
    if (w.kind == WidgetKind::Reminders) // one line
        std::replace_if(add.begin(), add.end(), [](wchar_t ch) { return ch == L'\r' || ch == L'\n'; }, L' ');
    else
        add.erase(std::remove(add.begin(), add.end(), L'\r'), add.end());
    c.caret = std::min(c.caret, text.size());
    text.insert(c.caret, add);
    c.caret += add.size();
}

bool DesktopWidgets::Key(Widget& w, UINT m, WPARAM wp)
{
    WidgetContent& c = w.content;
    if (w.kind == WidgetKind::Reminders && (c.item < 0 || c.item >= int(c.items.size())))
        return false;
    std::wstring& text = w.kind == WidgetKind::Notes ? c.text : c.items[size_t(c.item)].text;
    size_t& at = c.caret;
    at = std::min(at, text.size());
    auto back = [&](size_t i) -> size_t { return i >= 2 && IS_LOW_SURROGATE(text[i - 1]) && IS_HIGH_SURROGATE(text[i - 2]) ? i - 2 : i ? i - 1 : 0; };
    auto ahead = [&](size_t i) -> size_t {
        return i + 1 < text.size() && IS_HIGH_SURROGATE(text[i]) && IS_LOW_SURROGATE(text[i + 1]) ? i + 2 : std::min(i + 1, text.size());
    };
    bool changed = false;
    if (m == WM_KEYDOWN) {
        switch (wp) {
        case VK_LEFT:
            at = back(at);
            break;
        case VK_RIGHT:
            at = ahead(at);
            break;
        case VK_HOME: {
            size_t nl = at ? text.rfind(L'\n', at - 1) : std::wstring::npos;
            at = nl == std::wstring::npos ? 0 : nl + 1;
            break;
        }
        case VK_END: {
            size_t nl = text.find(L'\n', at);
            at = nl == std::wstring::npos ? text.size() : nl;
            break;
        }
        case VK_DELETE:
            if (at < text.size()) {
                text.erase(at, ahead(at) - at);
                changed = true;
            }
            break;
        default:
            return false; // WM_CHAR follows for the rest
        }
    } else {
        wchar_t ch = wchar_t(wp);
        if (ch == 27) { // Esc
            EndEdit(w);
            return true;
        }
        if (ch == L'\r') {
            if (w.kind == WidgetKind::Notes) {
                Insert(w, L"\n");
                changed = true;
            } else if (text.empty()) {
                EndEdit(w);
                return true;
            } else { // the next reminder, right below
                int next = c.item + 1;
                c.items.insert(c.items.begin() + next, Reminder{});
                c.editing = false; // the one typed stays as it is
                SaveSoon();
                StartEdit(w, next);
                return true;
            }
        } else if (ch == 8) { // Backspace
            if (at) {
                size_t from = back(at);
                text.erase(from, at - from);
                at = from;
                changed = true;
            }
        } else if (ch == 22) { // Ctrl+V
            if (OpenClipboard(w.surface->Hwnd())) {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
                        Insert(w, p);
                        GlobalUnlock(data);
                        changed = true;
                    }
                CloseClipboard();
            }
        } else if (ch >= 0x20 || ch == L'\t') {
            Insert(w, std::wstring(1, ch));
            changed = true;
        } else {
            return true; // other control keys
        }
    }
    if (changed)
        SaveSoon();
    c.caretOn = true;
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr); // the caret stays lit while typing
    w.surface->Invalidate();
    return true;
}

// The input method's composition and candidates next to the caret.
void DesktopWidgets::PlaceImeWindow(Widget& w)
{
    auto caret = std::find_if(w.hits.begin(), w.hits.end(), [](const WidgetHit& h) { return h.id == L"caret"; });
    if (caret == w.hits.end() || !w.surface)
        return;
    float s = w.surface->Scale();
    POINT p = w.surface->ContentToScreen(POINT{LONG(caret->rect.left * s), LONG(caret->rect.top * s)});
    ScreenToClient(w.surface->Hwnd(), &p);
    HIMC imc = ImmGetContext(w.surface->Hwnd());
    if (!imc)
        return;
    COMPOSITIONFORM cf{CFS_POINT, p, {}};
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand{0, CFS_CANDIDATEPOS, POINT{p.x, p.y + LONG((caret->rect.bottom - caret->rect.top) * s) + 4}, {}};
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(w.surface->Hwnd(), imc);
}

void DesktopWidgets::Menu(Widget& w)
{
    if (!menu)
        return;
    enum { kSmall = 1, kMedium, kLarge, kRemove, kFolder, kNextPhoto, kClearDone, kAdd };
    std::vector<MenuEntry> entries;
    auto size = [&](int id, WidgetSize s, const wchar_t* label) { entries.push_back({label, id, w.size == s, !WidgetSupports(w.kind, s)}); };
    size(kSmall, WidgetSize::Small, L"小");
    size(kMedium, WidgetSize::Medium, L"中");
    size(kLarge, WidgetSize::Large, L"大");
    entries.push_back({});
    if (w.kind == WidgetKind::Photos) {
        entries.push_back({L"选择照片文件夹…", kFolder});
        entries.push_back({L"换一张", kNextPhoto});
        entries.push_back({});
    }
    if (w.kind == WidgetKind::Reminders) {
        bool anyDone = std::any_of(w.content.items.begin(), w.content.items.end(), [](const Reminder& r) { return r.done; });
        entries.push_back({L"清除已完成", kClearDone, false, !anyDone});
        entries.push_back({});
    }
    entries.push_back({L"移除小组件", kRemove});
    entries.push_back({L"添加小组件…", kAdd});
    POINT p;
    GetCursorPos(&p);
    Widget* target = &w;
    menu->Open(std::move(entries), p, MenuPlace::Below, [this, target](int cmd) {
        if (cmd == kAdd) {
            if (onAddWidgets)
                onAddWidgets();
            return;
        }
        if (!Has(target))
            return; // gone while the menu was open
        Widget& w = *target;
        switch (cmd) {
        case kSmall:
        case kMedium:
        case kLarge:
            Resize(w, cmd == kSmall ? WidgetSize::Small : cmd == kMedium ? WidgetSize::Medium : WidgetSize::Large);
            break;
        case kFolder:
            PickFolder(w);
            break;
        case kNextPhoto:
            LoadPhoto(w, true);
            break;
        case kClearDone:
            std::erase_if(w.content.items, [](const Reminder& r) { return r.done; });
            Save();
            w.surface->Invalidate();
            break;
        case kRemove:
            Remove(w); // w is gone
            break;
        }
    });
}

// ---- painting ------------------------------------------------------------------

void DesktopWidgets::Paint(Widget& w, Canvas& c)
{
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, WidgetWidth(w.size), WidgetHeight(w.size)), kWidgetRadius, c.theme.panel);
    GetLocalTime(&world_.now);
    PaintWidget(c, w.kind, w.size, w.content, world_, w.hits);
    if (w.content.editing)
        PlaceImeWindow(w);
}

// ---- tests ---------------------------------------------------------------------

void DesktopWidgets::LogState() const
{
    Log(L"widgets: state enabled=%d raised=%d count=%zu cols=%d rows=%d", enabled_ ? 1 : 0, raised_ ? 1 : 0, widgets_.size(), Cols(), Rows());
    for (size_t i = 0; i < widgets_.size(); ++i) {
        const Widget& w = *widgets_[i];
        RECT r = w.surface ? w.surface->Rect() : RECT{};
        // What is right under it: the next visible window that is not a widget.
        std::wstring under;
        bool topmost = false;
        if (w.surface) {
            topmost = (GetWindowLongPtrW(w.surface->Hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            for (HWND h = GetWindow(w.surface->Hwnd(), GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT)) {
                bool widget = std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& o) { return o->surface && o->surface->Hwnd() == h; });
                if (!widget && IsWindowVisible(h)) {
                    under = ClassOf(h);
                    break;
                }
            }
        }
        Log(L"widgets: w%zu kind=%s size=%s cell=%d,%d visible=%d topmost=%d under=%s rect=%ld,%ld,%ld,%ld text=%zu items=%zu photo=%d", i,
            NameOf(w.kind), SizeName(w.size), w.col, w.row, w.surface && w.surface->Visible() ? 1 : 0, topmost ? 1 : 0,
            under.empty() ? L"-" : under.c_str(), r.left, r.top, r.right, r.bottom, w.content.text.size(), w.content.items.size(),
            w.content.image ? 1 : 0);
    }
}

void DesktopWidgets::TestAction(const std::wstring& arg)
{
    auto part = [&](int n) {
        size_t b = 0;
        for (int i = 0; i < n && b != std::wstring::npos; ++i)
            b = arg.find(L':', b) == std::wstring::npos ? std::wstring::npos : arg.find(L':', b) + 1;
        if (b == std::wstring::npos)
            return std::wstring();
        size_t e = arg.find(L':', b);
        return arg.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
    };
    std::wstring verb = part(0);
    if (verb == L"add") {
        AddNamed(part(1), part(2));
    } else if (verb == L"remove" || verb == L"size" || verb == L"type") {
        size_t i = size_t(_wtoi(part(1).c_str()));
        if (i >= widgets_.size())
            return;
        Widget& w = *widgets_[i];
        if (verb == L"remove") {
            Remove(w);
        } else if (verb == L"size") {
            Resize(w, ParseSize(part(2)));
        } else if (Editable(w.kind) && w.surface) {
            if (w.kind == WidgetKind::Reminders)
                w.content.items.push_back({});
            StartEdit(w, w.kind == WidgetKind::Reminders ? int(w.content.items.size()) - 1 : -1);
            Insert(w, part(2));
            EndEdit(w);
        }
    } else if (verb == L"restack") {
        Restack();
    }
}

} // namespace atrium
