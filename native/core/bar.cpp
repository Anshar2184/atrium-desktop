#include "bar.h"

#include "clock.h"
#include "log.h"
#include "render/capture.h"
#include "render/glass_renderer.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace atrium {

namespace {

constexpr wchar_t kVisualClass[] = L"AtriumBarVisual";
constexpr wchar_t kInputClass[] = L"AtriumBarInput";
constexpr wchar_t kClockKey[] = L"#clock";
constexpr wchar_t kStartKey[] = L"#start";
constexpr wchar_t kNotifyKey[] = L"#notify";
constexpr wchar_t kTrayKey[] = L"#tray";
constexpr wchar_t kStatusKey[] = L"#status";
constexpr wchar_t kMoreKey[] = L"#more";
// Bar entries and the clock start with '#'; app keys are AUMIDs or paths.
bool IsEntry(const std::wstring& key) { return !key.empty() && key[0] == L'#'; }
constexpr double kInsetDip = 14; // room for the drop shadow around the shape
constexpr DWORD kStaleMs = 2000;  // present at least this often (echo filter cap)
constexpr UINT_PTR kTooltipTimer = 1;
constexpr UINT kTooltipDelayMs = 450;
constexpr UINT_PTR kFileHoverTimer = 2;
constexpr UINT kFileHoverDelayMs = 600;
constexpr UINT_PTR kGuardTimer = 3;
constexpr UINT kGuardMs = 1000;
constexpr UINT kGuardFastMs = 150; // while below a system taskbar the user is using

Bar* g_guard = nullptr; // the bar whose stacking the WinEvent hooks look after

// Windows the bar never climbs over: menus, tooltips, the shell's own
// surfaces, input method windows.
bool IgnoredCover(HWND w)
{
    static const wchar_t* const kClasses[] = {L"#32768", L"tooltips_class32", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                                              L"TopLevelWindowForOverflowXamlIsland", L"Xaml_WindowedPopupClass", L"XamlExplorerHostIslandWindow",
                                              L"Windows.UI.Core.CoreWindow", L"SysShadow", L"IME", L"MSCTFIME UI", L"ForegroundStaging",
                                              L"MultitaskingViewFrame", L"Shell_InputSwitchTopLevelWindow", L"Progman", L"WorkerW"};
    wchar_t cls[96] = {};
    GetClassNameW(w, cls, 96);
    for (const wchar_t* c : kClasses)
        if (wcscmp(cls, c) == 0)
            return true;
    LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED))
        return true; // click-through overlays
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked != FALSE;
}

int Px(double dip, float scale) { return int(std::lround(dip * scale)); }

// Ink over the glass (mean sRGB luminance of what it shows, 0..1): black and
// white text read equally well near 0.46, so dark ink from 0.50 up and light
// ink again below 0.42. Light grey windows (0.5..0.65) used to keep white text.
constexpr float kDarkInkAbove = 0.50f, kLightInkBelow = 0.42f;

GlassParams ParamsFor(const Settings& s, float scale)
{
    GlassParams p{};
    p.radius = float(s.cornerRadius * scale);
    p.edge = float(s.edge * scale);
    p.refract = float(s.refraction * scale);
    p.dispersion = float(s.dispersion);
    // Blur strength is mostly the mip level; mixing a sharp and a blurred sample
    // only at the low end avoids double images at medium settings.
    p.blurMix = float(std::min(1.0, s.blur * 4.0));
    p.blurLod = float(1.0 + 4.5 * s.blur);
    p.tintMix = float(1.0 - s.transparency);
    p.tintR = float((s.tint >> 16) & 0xFF) / 255.f;
    p.tintG = float((s.tint >> 8) & 0xFF) / 255.f;
    p.tintB = float(s.tint & 0xFF) / 255.f;
    p.tintA = 1;
    p.highlight = float(s.highlight);
    p.shadow = float(s.shadow);
    p.lightX = -0.6f;
    p.lightY = -0.8f;
    p.inset = float(kInsetDip * scale);
    return p;
}

// The popups' glass: exactly the bar's material (D26), one set of parameters
// per piece of glass in the window.
std::vector<GlassParams> PanelParams(const Settings& s, float scale, const GlassPanel& p)
{
    std::vector<GlassParams> out;
    GlassParams g = ParamsFor(s, scale);
    g.inset = p.inset;
    if (p.shapes.empty()) {
        g.radius = p.radius;
        out.push_back(g);
        return out;
    }
    for (const GlassShape& sh : p.shapes) {
        g.shapeL = sh.left;
        g.shapeT = sh.top;
        g.shapeR = sh.right;
        g.shapeB = sh.bottom;
        g.radius = std::min(sh.radius, std::min(sh.right - sh.left, sh.bottom - sh.top) / 2);
        out.push_back(g);
    }
    return out;
}

// Luminance of what the glass shows (backdrop mixed with the tint), the
// bar's rule for its ink.
float GlassLuma(const Settings& s, float backdrop)
{
    float tm = float(1.0 - s.transparency);
    float tintLuma = (0.2126f * float((s.tint >> 16) & 0xFF) + 0.7152f * float((s.tint >> 8) & 0xFF) + 0.0722f * float(s.tint & 0xFF)) / 255.f;
    return backdrop * (1 - tm) + tintLuma * tm;
}

void ClockStrings(std::wstring& time, std::wstring& date)
{
    wchar_t t[64] = {}, d[64] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, t, 64);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, d, 64, nullptr);
    time = t;
    date = d;
}

float TextWidth(IDWriteFactory3* dw, const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight)
{
    winrt::com_ptr<IDWriteTextFormat> f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"",
                                    f.put())) ||
        FAILED(dw->CreateTextLayout(text.c_str(), UINT32(text.size()), f.get(), 10000, 1000, layout.put())))
        return px * float(text.size()) * 0.6f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

// Widest rendering of a clock string: every digit replaced by the widest digit.
std::wstring WidestDigits(std::wstring s)
{
    for (auto& c : s)
        if (iswdigit(c))
            c = L'8';
    return s;
}

void RegisterClasses(HINSTANCE instance)
{
    static bool done = false;
    if (done)
        return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = DefWindowProcW; // replaced per window once `this` is attached
    wc.lpszClassName = kVisualClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kInputClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

// OLE drop target on the input window: files dragged over an app bring it to
// the front after a moment; dropping opens them with that app.
struct BarDropTarget : winrt::implements<BarDropTarget, IDropTarget> {
    Bar* bar;
    bool files = false;
    explicit BarDropTarget(Bar* b) : bar(b) {}

    DWORD Effect(POINTL pt)
    {
        std::wstring key = bar->FileDragOver(POINT{pt.x, pt.y});
        return files && !key.empty() && bar->canOpenFiles && bar->canOpenFiles(key) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }
    HRESULT __stdcall DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files = data && data->QueryGetData(&fe) == S_OK;
        *effect = Effect(pt);
        return S_OK;
    }
    HRESULT __stdcall DragOver(DWORD, POINTL pt, DWORD* effect) override
    {
        *effect = Effect(pt);
        return S_OK;
    }
    HRESULT __stdcall DragLeave() override
    {
        bar->FileDragLeave();
        return S_OK;
    }
    HRESULT __stdcall Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        *effect = Effect(pt);
        std::wstring key = bar->fileHoverKey_;
        bar->FileDragLeave();
        if (*effect == DROPEFFECT_NONE || !data)
            return S_OK;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (FAILED(data->GetData(&fe, &sm)))
            return S_OK;
        std::vector<std::wstring> paths;
        if (auto drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count && i < 64; ++i) {
                std::wstring p(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                p.resize(DragQueryFileW(drop, i, p.data(), UINT(p.size())));
                paths.push_back(std::move(p));
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
        if (!paths.empty() && bar->onDropFiles)
            bar->onDropFiles(key, paths);
        return S_OK;
    }
};

Bar::Bar(HINSTANCE instance, bool debugCapturable, bool debugLayout) : debugCapturable_(debugCapturable), debugLayout_(debugLayout)
{
    RegisterClasses(instance);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    frame_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(dwrite_.put()));

    // Visual: never hit-tested (layered + transparent), no redirection bitmap.
    visual_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                              kVisualClass, L"Atrium Bar", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(visual_, 0, 255, LWA_ALPHA);
    // Input: draws nothing, so it is invisible; its window region is the shape.
    input_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kInputClass,
                             L"Atrium Bar Input", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(input_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::InputProc));
    SetWindowLongPtrW(visual_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(visual_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::VisualProc));
    if (debugCapturable_) {
        Log(L"bar: --debug-capturable, backdrop frozen after the first frame");
    } else {
        if (!SetWindowDisplayAffinity(visual_, WDA_EXCLUDEFROMCAPTURE))
            Log(L"bar: WDA_EXCLUDEFROMCAPTURE failed (%lu); the glass would capture itself", GetLastError());
        SetWindowDisplayAffinity(input_, WDA_EXCLUDEFROMCAPTURE);
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    // One tracking tooltip, positioned by hand centred above the hovered item
    // (a standard tooltip would appear under the cursor, on top of the bar).
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                               input_, nullptr, instance, nullptr);
    TTTOOLINFOW ti{sizeof(ti)};
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = input_;
    ti.uId = 1;
    ti.lpszText = const_cast<wchar_t*>(L"");
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, LPARAM(&ti));
    if (!debugCapturable_)
        SetWindowDisplayAffinity(tooltip_, WDA_EXCLUDEFROMCAPTURE); // it may overlap the glass

    // Drag and drop needs OLE on this (STA) thread; balanced in the destructor.
    oleReady_ = SUCCEEDED(OleInitialize(nullptr));
    if (oleReady_) {
        dropTarget_ = winrt::make<BarDropTarget>(this);
        if (FAILED(RegisterDragDrop(input_, dropTarget_.get()))) {
            Log(L"bar: RegisterDragDrop failed; dropping files on apps is unavailable");
            dropTarget_ = nullptr;
        }
    }

    // Stacking guard: raised again when covered, hidden behind full-screen apps.
    g_guard = this;
    hooks_[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Bar::WinEvent, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hooks_[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, &Bar::WinEvent, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(input_, kGuardTimer, kGuardMs, nullptr);

    render_ = std::thread([this] { RenderThread(); });
}

Bar::~Bar()
{
    for (HWINEVENTHOOK h : hooks_)
        if (h)
            UnhookWinEvent(h);
    if (g_guard == this)
        g_guard = nullptr;
    SetEvent(quit_);
    if (render_.joinable())
        render_.join();
    if (dropTarget_)
        RevokeDragDrop(input_);
    dropTarget_ = nullptr;
    DestroyWindow(tooltip_);
    DestroyWindow(input_);
    DestroyWindow(visual_);
    CloseHandle(wake_);
    CloseHandle(quit_);
    CloseHandle(frame_);
    if (oleReady_)
        OleUninitialize();
}

LRESULT CALLBACK Bar::VisualProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_DPICHANGED && self) {
        self->Relayout();
        return 0;
    }
    if (m == WM_CLOSE && self) {
        if (self->onCloseRequest)
            self->onCloseRequest(); // the app decides; never destroy just this window
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Bar::InputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->OnInput(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

std::wstring Bar::HitTest(POINT pt) const
{
    for (const auto& hit : hits_)
        if (PtInRect(&hit.rect, pt))
            return hit.key;
    return {};
}

LRESULT Bar::OnInput(HWND h, UINT m, WPARAM w, LPARAM l)
{
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_MOUSEACTIVATE: {
        // Never take focus from the user's app, except for the logo and the
        // bell: the start menu and the notification list need the keyboard,
        // and only the foreground process may hand the foreground to its own window.
        POINT cur;
        GetCursorPos(&cur);
        ScreenToClient(h, &cur);
        if ((HitTest(cur) == kStartKey || HitTest(cur) == kNotifyKey) && HIWORD(l) == WM_LBUTTONDOWN) {
            previousForeground_ = GetForegroundWindow();
            if (previousForeground_ == input_ || previousForeground_ == visual_)
                previousForeground_ = nullptr;
            return MA_ACTIVATE;
        }
        return MA_NOACTIVATE;
    }
    case WM_CLOSE:
        if (onCloseRequest)
            onCloseRequest();
        return 0;
    case WM_MOUSEMOVE: {
        if (dragging_) {
            UpdateDrag(pt);
            return 0;
        }
        // A press on an app that moves past the system drag threshold starts a drag.
        if (pressedLeft_ && !pressed_.empty() && !IsEntry(pressed_) && (w & MK_LBUTTON) &&
            (std::abs(pt.x - pressPt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - pressPt_.y) > GetSystemMetrics(SM_CYDRAG))) {
            BeginDrag(pt);
            return 0;
        }
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
            SetRegion(true); // magnified icons reach above the shape
        }
        std::wstring hover = HitTest(pt);
        bool magnify = scene_.magnify > 1.001f;
        if (hover != scene_.hoverKey || (magnify && scene_.pointerX != float(pt.x))) {
            if (hover != scene_.hoverKey) {
                scene_.hoverKey = hover;
                HideTooltip();
                if (!hover.empty())
                    SetTimer(h, kTooltipTimer, kTooltipDelayMs, nullptr);
            }
            scene_.pointerX = float(pt.x);
            Publish();
        }
        return 0;
    }
    case WM_TIMER:
        if (w == kTooltipTimer) {
            KillTimer(h, kTooltipTimer);
            if (pressed_.empty() && !dragging_)
                ShowTooltip();
            return 0;
        }
        if (w == kFileHoverTimer) {
            KillTimer(h, kFileHoverTimer);
            if (!fileHoverKey_.empty() && !IsEntry(fileHoverKey_) && onFileDragHover)
                onFileDragHover(fileHoverKey_);
            return 0;
        }
        if (w == kGuardTimer) {
            GuardStacking(nullptr);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        HideTooltip();
        if (!dragging_) {
            SetRegion(false);
            scene_.hoverKey.clear();
            scene_.pressedKey.clear();
            scene_.pointerX = -1;
            pressed_.clear();
            Publish();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
        HideTooltip();
        pressed_ = HitTest(pt);
        pressedLeft_ = m == WM_LBUTTONDOWN;
        pressPt_ = pt;
        scene_.pressedKey = pressed_;
        if (m == WM_LBUTTONDOWN) { // the glass gives way under the press
            scene_.rippleX = float(pt.x);
            scene_.rippleY = float(pt.y);
            scene_.rippleMs = ClockMs();
        }
        SetCapture(h);
        Publish();
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
        if (dragging_) {
            // Dropped well above or below the bar: cancel, like the system taskbar.
            int mid = int((scene_.bar.top + scene_.bar.bottom) / 2);
            EndDrag(std::abs(pt.y - mid) <= int(scene_.bar.bottom - scene_.bar.top));
            ReleaseCapture();
            POINT cur;
            GetCursorPos(&cur);
            if (WindowFromPoint(cur) != input_) { // released off the bar: no hover is left
                tracking_ = false;
                SetRegion(false);
                scene_.hoverKey.clear();
                scene_.pointerX = -1;
                Publish();
            }
            return 0;
        }
        ReleaseCapture();
        std::wstring key = HitTest(pt);
        bool same = !pressed_.empty() && key == pressed_;
        pressed_.clear();
        pressedLeft_ = false;
        scene_.pressedKey.clear();
        Publish();
        if (!same)
            return 0;
        // Shift+click starts a new instance, as on the Windows taskbar.
        bool launch = m == WM_MBUTTONUP || (w & MK_SHIFT);
        if (IsEntry(key)) {
            if (m != WM_LBUTTONUP)
                return 0;
            RECT anchor = EntryRect(key);
            if (key == kClockKey && onClockClick)
                onClockClick();
            else if (key == kStartKey && onStartClick)
                onStartClick(anchor);
            else if (key == kNotifyKey && onNotifyClick)
                onNotifyClick(anchor);
            else if (key == kTrayKey && onTrayClick)
                onTrayClick(anchor);
            else if (key == kStatusKey && onStatusClick)
                onStatusClick(anchor);
            else if (key == kMoreKey && onOverflow)
                onOverflow(hidden_, anchor);
        } else if (launch ? bool(onLaunch) : bool(onActivate)) {
            // Starting an app (or another instance) hops its icon, as in the macOS Dock.
            auto t = std::find_if(items_.begin(), items_.end(), [&](const TaskItem& i) { return i.key == key; });
            if (t != items_.end() && (launch || t->windows.empty()))
                Bounce(key, t->windows.empty() ? 4 : 1);
            (launch ? onLaunch : onActivate)(key);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_ && reinterpret_cast<HWND>(l) != h) {
            EndDrag(false); // capture taken away (e.g. a system menu opened)
            SetRegion(false);
        }
        return 0;
    case WM_MOUSEWHEEL: { // over the status icon: the volume
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (notches && HitTest(p) == kStatusKey && onStatusWheel)
            onStatusWheel(notches);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (dragging_)
            return 0;
        std::wstring key = HitTest(pt);
        POINT screen = pt;
        ClientToScreen(h, &screen);
        if (!key.empty() && !IsEntry(key) && onItemMenu)
            onItemMenu(key, screen);
        else if (onContextMenu)
            onContextMenu(screen);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

void Bar::BeginDrag(POINT pt)
{
    int from = -1;
    for (int i = 0; i < int(scene_.items.size()); ++i)
        if (scene_.items[size_t(i)].key == pressed_)
            from = i;
    if (from < 0)
        return;
    const auto& plate = scene_.items[size_t(from)].plate;
    dragging_ = true;
    dragKey_ = pressed_;
    dragFrom_ = dragInsert_ = from;
    grabDx_ = pressPt_.x - int((plate.left + plate.right) / 2);
    dragX_ = pt.x;
    dragY_ = pt.y;
    scene_.pressedKey.clear();
    HideTooltip();
    Relayout();
}

void Bar::UpdateDrag(POINT pt)
{
    dragX_ = pt.x;
    dragY_ = pt.y;
    // Insertion point: the slot whose centre is nearest to the dragged plate's.
    float centre = float(pt.x - grabDx_);
    int slot = int(std::lround((centre - float(appsLeft_) - float(plate_) / 2) / float(pitch_)));
    dragInsert_ = std::clamp(slot, 0, std::max(shown_ - 1, 0));
    Relayout();
}

void Bar::EndDrag(bool commit)
{
    if (!dragging_)
        return;
    dragging_ = false;
    std::wstring key = dragKey_;
    dragKey_.clear();
    pressed_.clear();
    pressedLeft_ = false;
    scene_.pressedKey.clear();
    int from = -1;
    for (int i = 0; i < shown_ && i < int(items_.size()); ++i)
        if (items_[size_t(i)].key == key)
            from = i;
    bool moved = commit && from >= 0 && dragInsert_ >= 0 && dragInsert_ != from && dragInsert_ < int(items_.size());
    if (moved) {
        // Show the new order right away; the model confirms it a moment later
        // with the same order, which then needs no relayout.
        TaskItem t = std::move(items_[size_t(from)]);
        items_.erase(items_.begin() + from);
        items_.insert(items_.begin() + dragInsert_, std::move(t));
    }
    scene_.hoverKey = moved ? key : scene_.hoverKey;
    Relayout(); // the dropped icon slides from the pointer into its slot
    if (moved && onReorder)
        onReorder(key, dragInsert_);
}

std::wstring Bar::FileDragOver(POINT screen)
{
    POINT pt{screen.x - window_.left, screen.y - window_.top};
    std::wstring key = HitTest(pt);
    if (IsEntry(key))
        key.clear();
    if (key != fileHoverKey_) {
        fileHoverKey_ = key;
        scene_.hoverKey = key;
        KillTimer(input_, kFileHoverTimer);
        if (!key.empty())
            SetTimer(input_, kFileHoverTimer, kFileHoverDelayMs, nullptr);
        Publish();
    }
    return key;
}

void Bar::FileDragLeave()
{
    KillTimer(input_, kFileHoverTimer);
    fileHoverKey_.clear();
    scene_.hoverKey.clear();
    Publish();
}

void Bar::Apply(const Settings& s)
{
    settings_ = s;
    Relayout();
}

void Bar::SetItems(std::vector<TaskItem> items)
{
    // Title changes (a browser switching tabs) only affect the tooltips; skip
    // the relayout and the redraw unless something visible changed.
    auto visible = [](const std::vector<TaskItem>& v) {
        std::wstring sig;
        for (const auto& t : v)
            sig += t.key + L"|" + std::to_wstring(t.windows.size() > 0) + std::to_wstring(t.active) + std::to_wstring(t.attention) + L"|" +
                   std::to_wstring(t.iconVersion) + L"\n";
        return sig;
    };
    bool same = visible(items) == visible(items_);
    items_ = std::move(items);
    if (same)
        UpdateTooltips();
    else
        Relayout();
}

void Bar::SetVisible(bool visible)
{
    visible_ = visible;
    Relayout();
}

void Bar::Relayout()
{
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float s = float(dpiX) / 96.f;
    const RECT& wa = mi.rcWork;
    const Settings& st = settings_;

    // Metrics (px). Every icon sits centred on a square plate (press and drag
    // feedback); the plate has the same gap `v` to the top and bottom edge,
    // and its corner radius is the bar's minus that gap, so plate and bar
    // corners are concentric.
    const int inset = Px(kInsetDip, s);
    const int barH = Px(st.height, s);
    const int icon = std::clamp(Px(st.iconSize, s), Px(12, s), barH - Px(16, s));
    int plate = std::max(icon + Px(4, s), std::min(barH - Px(12, s), int(std::lround(icon * 1.55))));
    int gap = std::max(Px(4, s), int(std::lround(icon * 0.18)));
    const int sepGap = Px(8, s);
    const float mag = float(std::clamp(st.magnify, 1.0, 2.0));

    // Clock block sized for its widest possible text.
    float timePx = float(barH) * 0.25f, datePx = float(barH) * 0.19f;
    int clockW = 0;
    if (st.showClock) {
        std::wstring time, date;
        ClockStrings(time, date);
        float w = std::max(TextWidth(dwrite_.get(), WidestDigits(time), timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                           TextWidth(dwrite_.get(), WidestDigits(date), datePx, DWRITE_FONT_WEIGHT_NORMAL));
        clockW = int(std::ceil(w)) + Px(22, s);
    }

    // Left to right: [logo] [apps ... (overflow)] | [tray] [status] [clock] [bell].
    // The logo, the overflow entry, the tray and the bell use the same plates
    // as the apps; the status icon (Wi-Fi with battery or volume) a wider one.
    const int statusW = std::max(plate, int(std::lround(float(icon) * 1.75f)) + Px(12, s));
    const int total = int(items_.size());
    int n = total;
    bool more = false;
    auto padFor = [&](int p) { return (barH - p) / 2 + Px(4, s); }; // ends: plate gap plus a little air
    auto contentWidth = [&](int count, int p, int g, bool overflow) {
        int slots = count + (overflow ? 1 : 0);
        int w = 2 * padFor(p) + p + 2 * g;               // logo and the air after it
        w += slots ? slots * p + (slots - 1) * g : 0;    // apps
        w += 2 * sepGap + p + g + statusW + g;            // separator, tray, status
        w += (clockW ? clockW + g : 0) + p;               // clock, bell
        return std::max(w, barH * 2);
    };
    // The magnified bar stretches by up to (mag - 1) * range on top of its width.
    auto stretchFor = [&](int p, int g) { return mag > 1.001f ? int(std::ceil((mag - 1) * float(p + g) * 2.3f)) : 0; };
    const int screenW = int(wa.right - wa.left) - 2 * inset;
    const int maxW = std::max(barH * 2, std::min(Px(st.width, s), screenW - stretchFor(plate, gap)));
    // Too many apps: tighten the gaps, then the plates; what still does not fit
    // goes behind the overflow entry (never silently dropped).
    while (n > 0 && contentWidth(n, plate, gap, false) > maxW && gap > Px(2, s))
        --gap;
    while (n > 0 && contentWidth(n, plate, gap, false) > maxW && plate > icon + Px(4, s))
        --plate;
    if (contentWidth(n, plate, gap, false) > maxW) {
        more = true;
        while (n > 0 && contentWidth(n, plate, gap, true) > maxW)
            --n;
    }
    hidden_.clear();
    for (int i = n; i < total; ++i)
        hidden_.push_back(items_[size_t(i)].key);
    // Never narrower than what is always there (logo, overflow, tray, status, clock, bell).
    const int barW = std::max(st.autoWidth ? std::min(contentWidth(n, plate, gap, more), maxW) : maxW, std::min(contentWidth(0, plate, gap, more), screenW));
    const int pad = padFor(plate);
    const int v = (barH - plate) / 2;
    const int pitch = plate + gap;
    const int radius = std::min(Px(st.cornerRadius, s), std::min(barW, barH) / 2);

    // Room in the windows around the shape: the shadow everywhere; at the
    // sides half of the magnified stretch (the bar grows on both sides of the
    // pointer); above, the magnified and hopping icons.
    const float range = float(pitch) * 2.3f;
    const int stretch = stretchFor(plate, gap);
    const int insetX = inset + (stretch + 1) / 2 + (stretch ? Px(2, s) : 0);
    const int lift = std::max(int(std::ceil(float(icon) * (mag - 1))), int(std::ceil(float(icon) * 0.55f)));
    const int rise = std::max(0, lift - (barH - icon) / 2 + Px(6, s));
    const int insetTop = std::max(inset, rise);

    const int winW = barW + 2 * insetX, winH = barH + insetTop + inset;
    const int x = wa.left + (wa.right - wa.left - winW) / 2;
    const int y = wa.bottom - Px(st.margin, s) - barH - insetTop;

    // Display order: during a drag the dragged app leaves its slot and the
    // others close up around the insertion point.
    std::vector<int> order(size_t(std::max(n, 0)));
    for (int i = 0; i < n; ++i)
        order[size_t(i)] = i;
    int dragIndex = -1;
    if (dragging_) {
        for (int i = 0; i < n; ++i)
            if (items_[size_t(i)].key == dragKey_)
                dragIndex = i;
        if (dragIndex < 0) {
            dragging_ = false; // the app went away mid-drag
            dragKey_.clear();
        } else {
            dragInsert_ = std::clamp(dragInsert_, 0, n - 1);
            order.erase(order.begin() + dragIndex);
            order.insert(order.begin() + dragInsert_, dragIndex);
        }
    }

    // Launch bounces older than their longest possible run are over.
    const double now = ClockMs();
    for (auto it = bounces_.begin(); it != bounces_.end();)
        it = now - it->second.first > 4000 ? bounces_.erase(it) : std::next(it);

    // Scene in window coordinates.
    BarScene scene;
    scene.scale = s;
    scene.bar = D2D1::RectF(float(insetX), float(insetTop), float(insetX + barW), float(insetTop + barH));
    scene.plateRadius = float(std::clamp(radius - v, Px(3, s), plate / 2));
    scene.hoverKey = dragging_ ? std::wstring() : scene_.hoverKey;
    scene.pressedKey = scene_.pressedKey;
    scene.dragKey = dragging_ ? dragKey_ : std::wstring();
    scene.magnify = mag;
    scene.magnifyRange = range;
    // Keep the pointer and the ripple; only the geometry changes. The pointer
    // moves with the window if it was rebuilt at another x.
    scene.pointerX = scene_.pointerX < 0 ? -1 : scene_.pointerX + float(window_.left - x);
    scene.rippleX = scene_.rippleX + float(window_.left - x);
    scene.rippleY = scene_.rippleY + float(window_.top - y);
    scene.rippleMs = scene_.rippleMs;
    hits_.clear();
    // Hit cells reach up to the window's top: magnified icons rise above the
    // shape, and the input region then covers that room too.
    const int hitTop = 0, hitBottom = insetTop + barH;
    const float plateTop = float(insetTop + v);
    const float iconTop = float(insetTop) + float(barH - icon) / 2;
    auto entry = [&](const wchar_t* key, int left, const wchar_t* glyph) {
        SceneItem it;
        it.key = key;
        it.plate = D2D1::RectF(float(left), plateTop, float(left + plate), plateTop + float(plate));
        float il = float(left) + float(plate - icon) / 2;
        it.icon = D2D1::RectF(il, iconTop, il + float(icon), iconTop + float(icon));
        it.glyph = glyph;
        return it;
    };
    // Right group, from the right edge: bell, clock, separator.
    const int startLeft = insetX + pad;
    const int bellLeft = insetX + barW - pad - plate;
    const int clockRight = bellLeft - (clockW ? gap : 0);
    const int clockLeft = clockRight - clockW;
    const int statusRight = (clockW ? clockLeft : bellLeft) - gap;
    const int statusLeft = statusRight - statusW;
    const int trayLeft = statusLeft - gap - plate;
    const int rightLeft = trayLeft;
    const int appsMin = startLeft + plate + 2 * gap;
    const int slots = n + (more ? 1 : 0);
    const int appsW = slots ? slots * plate + (slots - 1) * gap : 0;
    // Apps are centred in the space between the logo and the separator (matters when autoWidth is off).
    const int appsLeft = appsMin + std::max(0, (rightLeft - 2 * sepGap - appsMin - appsW) / 2);
    scene.extras.push_back(entry(kStartKey, startLeft, L""));
    hits_.push_back({kStartKey, RECT{insetX, hitTop, appsMin - gap / 2, hitBottom}});
    // Icons are rendered for their magnified size, so they stay sharp when they grow.
    const int iconPx = int(std::lround(float(icon) * mag));
    std::map<std::wstring, CachedIcon> keep;
    for (int k = 0; k < n; ++k) {
        const int i = order[size_t(k)];
        const TaskItem& t = items_[size_t(i)];
        SceneItem it;
        it.key = t.key;
        float left = float(appsLeft + k * pitch);
        if (i == dragIndex) // follows the pointer, kept within the app strip
            left = std::clamp(float(dragX_ - grabDx_) - float(plate) / 2, float(appsLeft - gap), float(appsLeft + n * pitch - gap - plate + gap));
        it.plate = D2D1::RectF(left, plateTop, left + float(plate), plateTop + float(plate));
        float iconLeft = left + float(plate - icon) / 2;
        it.icon = D2D1::RectF(iconLeft, iconTop, iconLeft + float(icon), iconTop + float(icon));
        it.running = !t.windows.empty();
        it.active = t.active;
        it.attention = t.attention;
        if (auto b = bounces_.find(t.key); b != bounces_.end()) {
            it.bounceMs = b->second.first;
            it.bounces = b->second.second;
        }
        // Icons are cached per key until the model reports a new version.
        CachedIcon c;
        if (auto f = icons_.find(t.key); f != icons_.end() && f->second.version == t.iconVersion && f->second.px == iconPx && f->second.image)
            c = f->second;
        else if (iconProvider) {
            IconImage img = iconProvider(t.key, iconPx);
            if (!img.empty())
                c = {t.iconVersion, iconPx, nextIconId_++, std::make_shared<const IconImage>(std::move(img))};
        }
        if (c.image) {
            it.image = c.image;
            it.imageId = c.id;
            keep[t.key] = c;
        }
        scene.items.push_back(std::move(it));
        // Hit cells split the gaps; the last one reaches the separator.
        int hl = k == 0 ? appsMin - gap / 2 : appsLeft + k * pitch - gap / 2;
        int hr = k == slots - 1 ? rightLeft - sepGap : appsLeft + k * pitch + plate + (gap - gap / 2);
        hits_.push_back({t.key, RECT{hl, hitTop, hr, hitBottom}});
    }
    if (more) {
        int left = appsLeft + n * pitch;
        scene.extras.push_back(entry(kMoreKey, left, L"\uE712"));
        hits_.push_back({kMoreKey, RECT{n ? left - gap / 2 : appsMin - gap / 2, hitTop, rightLeft - sepGap, hitBottom}});
    }
    icons_ = std::move(keep);
    appsLeft_ = appsLeft;
    pitch_ = pitch;
    plate_ = plate;
    gap_ = gap;
    shown_ = n;
    scene.separatorX = float(rightLeft - sepGap);
    scene.extras.push_back(entry(kTrayKey, trayLeft, L"\uE70E"));
    hits_.push_back({kTrayKey, RECT{rightLeft - sepGap, hitTop, trayLeft + plate + gap / 2, hitBottom}});
    SceneItem status = entry(kStatusKey, statusLeft, L"");
    status.plate = D2D1::RectF(float(statusLeft), plateTop, float(statusRight), plateTop + float(plate));
    status.icon = D2D1::RectF(float(statusLeft), iconTop, float(statusRight), iconTop + float(icon));
    scene.extras.push_back(std::move(status));
    hits_.push_back({kStatusKey, RECT{trayLeft + plate + gap / 2, hitTop, statusRight + gap / 2, hitBottom}});
    scene.status = status_;
    if (clockW) {
        scene.clock = D2D1::RectF(float(clockLeft), plateTop, float(clockRight), plateTop + float(plate));
        scene.timePx = timePx;
        scene.datePx = datePx;
        hits_.push_back({kClockKey, RECT{statusRight + gap / 2, hitTop, clockRight + gap / 2, hitBottom}});
    }
    SceneItem bell = entry(kNotifyKey, bellLeft, L"\uEA8F");
    bell.badge = unread_;
    scene.extras.push_back(std::move(bell));
    hits_.push_back({kNotifyKey, RECT{clockW ? clockRight + gap / 2 : statusRight + gap / 2, hitTop, insetX + barW, hitBottom}});
    if (!scene.hoverKey.empty() && std::none_of(hits_.begin(), hits_.end(), [&](const Hit& h) { return h.key == scene.hoverKey; }))
        scene.hoverKey.clear(); // the hovered app went away
    scene_ = std::move(scene);

    // The window only changes when the bar's size does (not on every drag move).
    const bool shown = visible_ && !fullscreen_;
    RECT win{x, y, x + winW, y + winH};
    if (!dragging_ || !EqualRect(&win, &applied_) || shown != appliedVisible_) {
        SetWindowPos(visual_, HWND_TOPMOST, x, y, winW, winH, SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // Input window right below the visual one, region = the rounded shape.
        SetWindowPos(input_, visual_, x, y, winW, winH, SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        applied_ = win;
        appliedVisible_ = shown;
    }
    window_ = win;
    monitor_ = mi.rcMonitor;
    hmonitor_ = mon;
    scale_ = s;
    inset_ = inset;
    insetX_ = insetX;
    insetTop_ = insetTop;
    rise_ = rise;
    radius_ = radius;
    SetRegion(regionExtended_);
    UpdateTooltips();
    Publish();
    if (debugLayout_ && !dragging_)
        for (const auto& hit : hits_)
            Log(L"layout: %s %ld,%ld,%ld,%ld", hit.key.c_str(), x + hit.rect.left, y + insetTop, x + hit.rect.right, y + hit.rect.bottom);
}

void Bar::SetRegion(bool extended)
{
    regionExtended_ = extended;
    const int barW = int(scene_.bar.right - scene_.bar.left), barH = int(scene_.bar.bottom - scene_.bar.top);
    const int ext = extended ? rise_ : 0;
    if (appliedRgn_[0] == barW && appliedRgn_[1] == barH && appliedRgn_[2] == radius_ && appliedRgn_[3] == insetX_ && appliedRgn_[4] == insetTop_ &&
        appliedRgn_[5] == ext)
        return;
    HRGN rgn = CreateRoundRectRgn(insetX_, insetTop_, insetX_ + barW + 1, insetTop_ + barH + 1, 2 * radius_, 2 * radius_);
    if (ext > 0) {
        // While the pointer is on the bar, the room above it belongs to the
        // magnified icons (clicks there go to the icon below the pointer).
        HRGN top = CreateRectRgn(insetX_ + radius_ / 2, insetTop_ - ext, insetX_ + barW - radius_ / 2, insetTop_ + radius_);
        CombineRgn(rgn, rgn, top, RGN_OR);
        DeleteObject(top);
    }
    SetWindowRgn(input_, rgn, FALSE);
    appliedRgn_[0] = barW;
    appliedRgn_[1] = barH;
    appliedRgn_[2] = radius_;
    appliedRgn_[3] = insetX_;
    appliedRgn_[4] = insetTop_;
    appliedRgn_[5] = ext;
}

void Bar::LogState()
{
    // Place in the z-order: windows above the bar that overlap it.
    RECT bar = BarRect(), r{}, overlap{};
    int above = 0, overlapping = 0;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    bool trayAbove = false;
    for (HWND w = GetTopWindow(nullptr); w && w != visual_; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w))
            continue;
        ++above;
        if (w == tray)
            trayAbove = true;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() && GetWindowRect(w, &r) && IntersectRect(&overlap, &r, &bar) && !IgnoredCover(w) && w != tray)
            ++overlapping;
    }
    Log(L"bar: state visible=%d fullscreen=%d topmost=%d shown=%d coveredBy=%d trayAbove=%d bar=%ld,%ld,%ld,%ld", visible_ ? 1 : 0,
        fullscreen_ ? 1 : 0, (GetWindowLongW(visual_, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0, IsWindowVisible(visual_) ? 1 : 0, overlapping,
        trayAbove ? 1 : 0, bar.left, bar.top, bar.right, bar.bottom);
    frameLog_ = true;
    SetEvent(wake_);
}

void Bar::Bounce(const std::wstring& key, int hops)
{
    Log(L"bar: bounce %s (%d)", key.c_str(), hops);
    double now = ClockMs();
    bounces_[key] = {now, hops};
    for (auto& it : scene_.items)
        if (it.key == key) {
            it.bounceMs = now;
            it.bounces = hops;
        }
    Publish();
}

void Bar::SetPanel(const GlassPanel& panel)
{
    if (panel.visible)
        panels_[panel.hwnd] = panel;
    else
        panels_.erase(panel.hwnd);
    Publish();
}

void CALLBACK Bar::WinEvent(HWINEVENTHOOK, DWORD event, HWND, LONG idObject, LONG, DWORD, DWORD)
{
    if (g_guard && idObject == OBJID_WINDOW)
        g_guard->GuardStacking(event == EVENT_SYSTEM_FOREGROUND ? L"foreground" : L"move");
}

// A full-screen app in front on the bar's monitor: covers the whole monitor
// and is neither maximized (with an auto-hidden taskbar maximized windows
// cover it too) nor the desktop.
bool Bar::ForegroundIsFullscreen() const
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    fg = GetAncestor(fg, GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId() || IsZoomed(fg) || IsIconic(fg) || !IsWindowVisible(fg))
        return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    for (const wchar_t* c : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (wcscmp(cls, c) == 0)
            return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked || MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != hmonitor_)
        return false;
    RECT r{};
    GetWindowRect(fg, &r);
    return r.left <= monitor_.left && r.top <= monitor_.top && r.right >= monitor_.right && r.bottom >= monitor_.bottom;
}

void Bar::Raise()
{
    SetWindowPos(visual_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(input_, visual_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// Keeps the bar on top: hidden behind a full-screen app, below a revealed
// system taskbar that overlaps it while the user is using that taskbar (the
// pointer on it outside the bar, or the keyboard in it), and otherwise raised again when
// another topmost window covers it. A taskbar held up only by an app's
// flashing button does not push the bar down (the bar shows the app's
// attention itself). A window that keeps pushing itself over the bar
// wins after a few rounds, so two always-on-top windows never fight.
void Bar::GuardStacking(const wchar_t* why)
{
    bool full = ForegroundIsFullscreen();
    if (full != fullscreen_) {
        fullscreen_ = full;
        Log(L"bar: %s", full ? L"hidden behind a full-screen app" : L"shown again (no full-screen app in front)");
        if (full) {
            HideTooltip();
            SetRegion(false);
        }
        Relayout();
    }
    if (!visible_ || fullscreen_ || dragging_)
        return;
    RECT bar = BarRect(), r{}, overlap{}, trayRect{};
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    bool trayOver = tray && IsWindowVisible(tray) && GetWindowRect(tray, &trayRect) && IntersectRect(&overlap, &trayRect, &bar) &&
                    overlap.bottom - overlap.top > 2;
    bool usingTray = false;
    POINT cur{};
    wchar_t fgCls[64] = {};
    if (trayOver) {
        GetCursorPos(&cur);
        HWND fg = GetForegroundWindow();
        GetClassNameW(fg, fgCls, 64);
        // The pointer counts on the system taskbar outside the bar (the screen's
        // bottom edge reveals it): over the bar it is meant for the bar.
        RECT ours{window_.left, window_.top, window_.right, bar.bottom};
        usingTray = (PtInRect(&trayRect, cur) && !PtInRect(&ours, cur)) || fg == tray || wcscmp(fgCls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
                    wcscmp(fgCls, L"Shell_TrayWnd") == 0;
    }
    bool trayAbove = false;
    HWND cover = nullptr;
    for (HWND w = GetTopWindow(nullptr); w && w != visual_; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == tray) {
            trayAbove = true;
            continue;
        }
        if (w == input_ || !IsWindowVisible(w))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId())
            continue;
        if (!(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            cover = w; // the bar has lost its topmost place
            break;
        }
        if (GetWindowRect(w, &r) && IntersectRect(&overlap, &r, &bar) && !IgnoredCover(w)) {
            cover = w;
            break;
        }
    }
    if (trayOver && usingTray) {
        // The revealed taskbar stays in front; it slides away again by itself.
        if (!trayAbove) {
            SetWindowPos(visual_, tray, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            SetWindowPos(input_, visual_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            if (!belowTray_)
                Log(L"bar: below the revealed system taskbar (pointer %ld,%ld, foreground %s)", cur.x, cur.y, fgCls);
        }
        if (!belowTray_) // the system taskbar gets the pointer now: watch for it coming back to the bar
            SetTimer(input_, kGuardTimer, kGuardFastMs, nullptr);
        belowTray_ = true;
        return;
    }
    if (trayOver && trayAbove) {
        // Up without the user (an app's button flashing): the bar stays in front.
        Raise();
        if (belowTray_)
            SetTimer(input_, kGuardTimer, kGuardMs, nullptr);
        if (belowTray_ || !trayHeldUp_)
            Log(L"bar: over the system taskbar, which an app holds up");
        belowTray_ = false;
        trayHeldUp_ = true;
        return;
    }
    if (belowTray_) {
        Log(L"bar: the system taskbar went down again");
        SetTimer(input_, kGuardTimer, kGuardMs, nullptr);
    }
    belowTray_ = false;
    trayHeldUp_ = trayOver;
    if (!cover)
        return;
    ULONGLONG now = GetTickCount64();
    for (auto it = raises_.begin(); it != raises_.end();)
        it = now - it->second.second > 10000 || !IsWindow(it->first) ? raises_.erase(it) : std::next(it);
    auto& count = raises_.try_emplace(cover, 0, now).first->second;
    if (++count.first > 4) {
        if (count.first == 5) {
            wchar_t cls[64] = {};
            GetClassNameW(cover, cls, 64);
            Log(L"bar: %s keeps covering the bar; leaving it in front", cls);
        }
        return;
    }
    Raise();
    if (cover != lastCover_) {
        wchar_t cls[64] = {};
        GetClassNameW(cover, cls, 64);
        Log(L"bar: raised over %s (%s)", cls, why ? why : L"check");
        lastCover_ = cover;
    }
}

void Bar::SetStatus(const SystemStatus& status)
{
    if (status == status_)
        return;
    status_ = status;
    scene_.status = status;
    Publish();
    UpdateTooltips();
}

void Bar::SetUnread(int count)
{
    if (count == unread_)
        return;
    unread_ = count;
    Relayout();
}

RECT Bar::EntryRect(const std::wstring& key) const
{
    auto rect = [&](const SceneItem& it) {
        return RECT{window_.left + LONG(it.plate.left), window_.top + LONG(it.plate.top), window_.left + LONG(it.plate.right),
                    window_.top + LONG(it.plate.bottom)};
    };
    for (const auto& it : scene_.extras)
        if (it.key == key)
            return rect(it);
    for (const auto& it : scene_.items)
        if (it.key == key)
            return rect(it);
    if (key == kClockKey)
        return RECT{window_.left + LONG(scene_.clock.left), window_.top + LONG(scene_.clock.top), window_.left + LONG(scene_.clock.right),
                    window_.top + LONG(scene_.clock.bottom)};
    return {};
}

RECT Bar::BarRect() const
{
    return RECT{window_.left + LONG(scene_.bar.left), window_.top + LONG(scene_.bar.top), window_.left + LONG(scene_.bar.right),
                window_.top + LONG(scene_.bar.bottom)};
}

std::wstring Bar::TooltipText(const std::wstring& key) const
{
    if (key == kStartKey)
        return L"\u5F00\u59CB"; // 开始
    if (key == kNotifyKey)
        return unread_ ? std::to_wstring(unread_) + L" \u6761\u672A\u8BFB\u901A\u77E5" : L"\u901A\u77E5"; // N 条未读通知 / 通知
    if (key == kTrayKey)
        return L"\u6258\u76D8"; // 托盘
    if (key == kStatusKey) {
        const SystemStatus& st = status_;
        std::wstring net = !st.connected ? L"\u672A\u8FDE\u63A5\u7F51\u7EDC" // 未连接网络
                                         : (st.wifi ? L"Wi-Fi " : L"") + st.network + (st.wifi ? L" \u00B7 \u4FE1\u53F7 " + std::to_wstring(st.signal) + L"%" : L"");
        std::wstring more;
        if (st.battery)
            more = L"\n\u7535\u6C60 " + std::to_wstring(st.batteryPercent) + L"%" + (st.charging ? L" \u00B7 \u5DF2\u63A5\u901A\u7535\u6E90" : L""); // 电池 N% · 已接通电源
        if (st.audio)
            more += L"\n\u97F3\u91CF " + (st.muted ? std::wstring(L"\u9759\u97F3") : std::to_wstring(st.volume) + L"%"); // 音量 N% / 静音
        return net + more;
    }
    if (key == kMoreKey)
        return L"\u53E6\u5916 " + std::to_wstring(hidden_.size()) + L" \u4E2A\u5E94\u7528"; // 另外 N 个应用
    if (key == kClockKey) {
        wchar_t buf[128] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, nullptr, nullptr, buf, 128, nullptr);
        return buf;
    }
    for (const auto& t : items_)
        if (t.key == key)
            return t.windows.size() > 1 ? t.name + L" \u00B7 " + std::to_wstring(t.windows.size()) + L" \u4E2A\u7A97\u53E3" : t.name;
    return {};
}

void Bar::ShowTooltip()
{
    const std::wstring& key = scene_.hoverKey;
    auto hit = std::find_if(hits_.begin(), hits_.end(), [&](const Hit& h) { return h.key == key; });
    std::wstring text = TooltipText(key);
    if (hit == hits_.end() || text.empty())
        return;
    TTTOOLINFOW ti{sizeof(ti)};
    ti.hwnd = input_;
    ti.uId = 1;
    ti.lpszText = text.data();
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, LPARAM(&ti));
    DWORD size = DWORD(SendMessageW(tooltip_, TTM_GETBUBBLESIZE, 0, LPARAM(&ti)));
    int w = LOWORD(size), h = HIWORD(size);
    int cx = window_.left + (hit->rect.left + hit->rect.right) / 2;
    // Above the magnified icons when they grow.
    int rise = scene_.magnify > 1.001f ? rise_ : 0;
    int top = window_.top + int(scene_.bar.top) - rise - h - Px(8, scale_);
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(cx - w / 2, top));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, LPARAM(&ti));
    tooltipShown_ = true;
}

void Bar::HideTooltip()
{
    KillTimer(input_, kTooltipTimer);
    if (!tooltipShown_)
        return;
    TTTOOLINFOW ti{sizeof(ti)};
    ti.hwnd = input_;
    ti.uId = 1;
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, LPARAM(&ti));
    tooltipShown_ = false;
}

void Bar::UpdateTooltips()
{
    if (tooltipShown_)
        ShowTooltip(); // e.g. the window count changed while hovering
}

void Bar::Publish()
{
    {
        std::lock_guard g(lock_);
        snapshot_.settings = settings_;
        snapshot_.scene = scene_;
        snapshot_.window = window_;
        snapshot_.monitor = monitor_;
        snapshot_.hmonitor = hmonitor_;
        snapshot_.scale = scale_;
        snapshot_.visible = visible_ && !fullscreen_;
        snapshot_.panels.clear();
        for (const auto& [hwnd, p] : panels_)
            snapshot_.panels.push_back(p);
        ++snapshot_.generation;
    }
    SetEvent(wake_);
}

void Bar::RenderThread()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        bool rebuild = false;
        try {
            rebuild = RenderSession();
        } catch (const winrt::hresult_error& e) {
            Log(L"render: 0x%08X %s; rebuilding in 1 s", static_cast<unsigned>(e.code()), e.message().c_str());
            rebuild = true;
        }
        if (!rebuild || WaitForSingleObject(quit_, 1000) != WAIT_TIMEOUT)
            break;
    }
    winrt::uninit_apartment();
}

bool Bar::RenderSession()
{
    // Wait for the first layout.
    HANDLE first[] = {quit_, wake_};
    Snapshot snap;
    for (;;) {
        {
            std::lock_guard g(lock_);
            snap = snapshot_;
        }
        if (snap.hmonitor)
            break;
        if (WaitForMultipleObjects(2, first, FALSE, INFINITE) == WAIT_OBJECT_0)
            return false;
    }

    Gpu gpu = Gpu::ForMonitor(snap.hmonitor);
    MonitorCapture capture(gpu, snap.hmonitor, frame_);
    GlassRenderer renderer(gpu, visual_, snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
    BarPainter painter(renderer.D2D(), renderer.DWrite());

    unsigned drawnGeneration = 0;
    std::wstring drawnText;
    bool darkClock = false, darkApps = false, drawnDark = false, drawnDarkIcons = false, animating = false;
    ULONGLONG lastPresent = 0;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    // Per-minute counters: captured frames, presents, frames skipped as echoes.
    unsigned frames = 0, presents = 0, skipped = 0;
    std::vector<double> workMs;    // capture copy + comparison (+ draw and present), per wake-up
    std::vector<double> presentMs; // the same, only for wake-ups that presented
    winrt::com_ptr<ID3D11Texture2D> frozen; // --debug-capturable only
    ULONGLONG statsStart = GetTickCount64();
    Log(L"render: started on %s, window %ldx%ld", gpu.adapterName.c_str(), snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
    // Glass layers of the popups, created while they are shown.
    struct PanelState {
        std::unique_ptr<GlassRenderer> renderer;
        unsigned pop = 0;
        bool ready = false;   // presented once (the popup was told)
        int ink = -1;         // ink last posted: 1 dark, 0 light, -1 none yet
        bool dark = false;
        std::vector<GlassParams> params;
    };
    std::map<HWND, PanelState> panels;
    // Draws the panels; true while one of them animates.
    auto renderPanels = [&](const Snapshot& snap, ID3D11Texture2D* source, bool& lost) {
        for (auto it = panels.begin(); it != panels.end();) {
            bool listed = std::any_of(snap.panels.begin(), snap.panels.end(), [&](const GlassPanel& p) { return p.hwnd == it->first; });
            it = listed ? std::next(it) : panels.erase(it);
        }
        bool moving = false;
        double now = ClockMs();
        for (const GlassPanel& p : snap.panels) {
            int w = p.window.right - p.window.left, h = p.window.bottom - p.window.top;
            if (w <= 0 || h <= 0 || !IsWindow(p.hwnd))
                continue;
            PanelState& ps = panels[p.hwnd];
            if (!ps.renderer) {
                try {
                    ps.renderer = std::make_unique<GlassRenderer>(gpu, p.hwnd, w, h, false, false);
                } catch (const winrt::hresult_error& e) {
                    Log(L"render: popup glass unavailable: 0x%08X", static_cast<unsigned>(e.code()));
                    continue;
                }
            }
            GlassRenderer& r = *ps.renderer;
            bool changed = r.Width() != w || r.Height() != h;
            r.Resize(w, h);
            if (source)
                r.UpdateBackdrop(source, p.window.left - snap.monitor.left, p.window.top - snap.monitor.top);
            std::vector<GlassParams> gp = PanelParams(snap.settings, snap.scale, p);
            if (auto rb = r.Poll()) {
                changed |= rb->changed;
                // One ink for the popup, from what its glass shows on average
                // (area-weighted over its pieces); hysteresis as on the bar.
                const float pad = float(GlassRenderer::kPad);
                float sum = 0, area = 0;
                for (const GlassParams& g : gp) {
                    float l = g.shapeR > g.shapeL ? g.shapeL : p.inset, t = g.shapeB > g.shapeT ? g.shapeT : p.inset;
                    float rr = g.shapeR > g.shapeL ? g.shapeR : float(w) - p.inset, b = g.shapeB > g.shapeT ? g.shapeB : float(h) - p.inset;
                    float a = std::max(0.f, rr - l) * std::max(0.f, b - t);
                    sum += rb->RegionLuma(l + pad, t + pad, rr + pad, b + pad) * a;
                    area += a;
                }
                float lum = GlassLuma(snap.settings, area > 0 ? sum / area : 0.5f);
                if (!ps.dark && lum > kDarkInkAbove)
                    ps.dark = true;
                else if (ps.dark && lum < kLightInkBelow)
                    ps.dark = false;
            }
            const std::wstring& tc = snap.settings.textColor;
            int ink = tc == L"dark" ? 1 : tc == L"light" ? 0 : ps.dark ? 1 : 0;
            if (ink != ps.ink) {
                ps.ink = ink;
                PostMessageW(p.hwnd, WM_GLASS_INK, WPARAM(ink), 0);
            }
            if (p.pop != ps.pop) {
                r.Pop(p.popOpen, p.popX, p.popY);
                ps.pop = p.pop;
            }
            if (p.rippleMs > 0 && now - p.rippleMs < 720) {
                float u = float((now - p.rippleMs) / 720);
                for (GlassParams& g : gp) {
                    g.rippleX = p.rippleX;
                    g.rippleY = p.rippleY;
                    g.rippleR = 150 * snap.scale * (1 - (1 - u) * (1 - u) * (1 - u));
                    g.rippleA = 0.8f * (1 - u) * (1 - u);
                }
                moving = true;
            }
            if (!r.HasBackdrop())
                continue; // waits for the first capture frame under it
            bool same = gp.size() == ps.params.size() && memcmp(gp.data(), ps.params.data(), gp.size() * sizeof(GlassParams)) == 0;
            if (!changed && r.HasPresented() && same)
                continue;
            if (!r.Present(gp, nullptr)) {
                lost = true;
                return false;
            }
            ps.params = gp;
            if (!ps.ready) {
                ps.ready = true;
                PostMessageW(p.hwnd, WM_GLASS_READY, 0, 0);
                wchar_t title[64] = {};
                GetWindowTextW(p.hwnd, title, 64);
                Log(L"render: popup glass ready for %s (%dx%d px, %zu shapes, radius %.0f px, tint %.2f, ink %s)", title, w, h, gp.size(),
                    gp.empty() ? 0.f : gp[0].radius, gp.empty() ? 0.f : gp[0].tintMix, ink ? L"dark" : L"light");
            }
        }
        return moving;
    };

    HANDLE handles[] = {quit_, wake_, frame_};
    bool panelsMoving = false;
    for (;;) {
        // Animations run at display rate; pending readbacks are polled soon.
        DWORD timeout = animating || panelsMoving ? 6 : renderer.ReadbackPending() ? 3 : 1000;
        DWORD w = WaitForMultipleObjects(3, handles, FALSE, timeout);
        if (w == WAIT_OBJECT_0)
            return false;
        if (GetTickCount64() - statsStart >= 60000) {
            std::sort(workMs.begin(), workMs.end());
            auto pct = [&](double q) { return workMs.empty() ? 0.0 : workMs[std::min(workMs.size() - 1, size_t(q * double(workMs.size())))]; };
            std::sort(presentMs.begin(), presentMs.end());
            double presentP95 = presentMs.empty() ? 0.0 : presentMs[std::min(presentMs.size() - 1, size_t(0.95 * double(presentMs.size())))];
            Log(L"render: last minute %u frames, %u presents, %u echoes skipped; work p50 %.2f ms, p95 %.2f ms, max %.2f ms; "
                L"presenting p95 %.2f ms",
                frames, presents, skipped, pct(0.5), pct(0.95), workMs.empty() ? 0.0 : workMs.back(), presentP95);
            frames = presents = skipped = 0;
            workMs.clear();
            presentMs.clear();
            statsStart = GetTickCount64();
        }
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        auto elapsedMs = [&] {
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        };
        {
            std::lock_guard g(lock_);
            snap = snapshot_;
        }
        auto frame = capture.TryLatest();
        if (snap.hmonitor != MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY))
            return true; // primary monitor changed: new device and capture
        if (!snap.visible) {
            bool lost = false;
            panelsMoving = renderPanels(snap, frame ? MonitorCapture::TextureOf(frame).get() : nullptr, lost);
            if (frame)
                frame.Close();
            if (lost)
                return true;
            animating = false;
            continue;
        }
        renderer.Resize(snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);

        // Backdrop source: the newest capture frame, or in --debug-capturable
        // mode a private copy of the first one (later frames contain the glass).
        winrt::com_ptr<ID3D11Texture2D> source;
        if (frame) {
            source = MonitorCapture::TextureOf(frame);
            ++frames;
            if (debugCapturable_) {
                if (!frozen) {
                    D3D11_TEXTURE2D_DESC d{};
                    source->GetDesc(&d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    d.MiscFlags = 0;
                    winrt::check_hresult(gpu.device->CreateTexture2D(&d, nullptr, frozen.put()));
                    gpu.context->CopyResource(frozen.get(), source.get());
                }
                source = frozen;
            }
        } else if (frozen && snap.generation != drawnGeneration) {
            source = frozen; // size-dependent textures were recreated
        }
        bool hadBackdrop = source != nullptr;
        if (source)
            renderer.UpdateBackdrop(source.get(), snap.window.left - snap.monitor.left, snap.window.top - snap.monitor.top);
        {
            bool lost = false;
            panelsMoving = renderPanels(snap, source.get(), lost);
            if (lost) {
                Log(L"render: device lost (popup glass)");
                return true;
            }
        }
        source = nullptr;
        if (frame)
            frame.Close();

        bool changed = !renderer.HasPresented();
        if (auto rb = renderer.Poll()) {
            changed |= rb->changed;
            // Ink per region, from what the glass shows there: the backdrop
            // luminance mixed with the tint. Hysteresis avoids flicker.
            float tm = float(1.0 - snap.settings.transparency);
            uint32_t tint = snap.settings.tint;
            float tintLuma = (0.2126f * float((tint >> 16) & 0xFF) + 0.7152f * float((tint >> 8) & 0xFF) + 0.0722f * float(tint & 0xFF)) / 255.f;
            auto decide = [&](bool& dark, D2D1_RECT_F r) {
                if (r.right <= r.left)
                    return;
                const float pad = float(GlassRenderer::kPad);
                float lum = rb->RegionLuma(r.left + pad, r.top + pad, r.right + pad, r.bottom + pad) * (1 - tm) + tintLuma * tm;
                if (!dark && lum > kDarkInkAbove)
                    dark = true;
                else if (dark && lum < kLightInkBelow)
                    dark = false;
            };
            decide(darkClock, snap.scene.clock);
            if (!snap.scene.items.empty())
                decide(darkApps, D2D1::RectF(snap.scene.items.front().icon.left, snap.scene.bar.top, snap.scene.items.back().icon.right,
                                             snap.scene.bar.bottom));
        }
        auto ink = [&](bool autoDark) { return snap.settings.textColor == L"dark" || (snap.settings.textColor == L"auto" && autoDark); };
        bool dark = ink(darkClock), darkIcons = ink(darkApps);

        std::wstring time, date;
        if (snap.settings.showClock)
            ClockStrings(time, date);
        std::wstring textKey = time + L"|" + date;
        animating = painter.Animate(snap.scene, ClockMs());
        if (frameLog_.exchange(false)) {
            const BarFrame& f = painter.Frame();
            Log(L"bar: frame shape=%.0f-%.0f base=%.0f-%.0f magnified=%.2f hop=%.2f ripple=%.2f", f.shape.left, f.shape.right, snap.scene.bar.left,
                snap.scene.bar.right, f.magnified, f.hop, f.rippleA);
        }
        ULONGLONG now = GetTickCount64();
        bool force = snap.generation != drawnGeneration || textKey != drawnText || dark != drawnDark || darkIcons != drawnDarkIcons ||
                     animating || now - lastPresent > kStaleMs;
        if (!changed && !force) {
            ++skipped; // echo of our own present, or nothing new under the glass
            if (hadBackdrop)
                workMs.push_back(elapsedMs());
            continue;
        }
        GlassParams params = ParamsFor(snap.settings, snap.scale);
        const BarFrame& bf = painter.Frame();
        params.radius = std::min(params.radius, (bf.shape.bottom - bf.shape.top) / 2);
        params.shapeL = bf.shape.left;
        params.shapeT = bf.shape.top;
        params.shapeR = bf.shape.right;
        params.shapeB = bf.shape.bottom;
        params.rippleX = bf.rippleX;
        params.rippleY = bf.rippleY;
        params.rippleR = bf.rippleR;
        params.rippleA = bf.rippleA;
        bool ok = renderer.Present(params,
                                   [&](ID2D1DeviceContext2* d2d) {
                                       painter.Draw(d2d, snap.scene, {dark, darkIcons}, time, date,
                                                    std::to_wstring(snap.generation) + textKey + (dark ? L"d" : L"l") + (darkIcons ? L"d" : L"l"),
                                                    animating);
                                   });
        if (!ok) {
            Log(L"render: device lost");
            return true;
        }
        ++presents;
        workMs.push_back(elapsedMs());
        presentMs.push_back(workMs.back());
        drawnGeneration = snap.generation;
        drawnText = textKey;
        drawnDark = dark;
        drawnDarkIcons = darkIcons;
        lastPresent = now;
    }
}

} // namespace atrium
