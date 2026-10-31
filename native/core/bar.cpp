#include "bar.h"

#include "log.h"
#include "render/capture.h"
#include "render/glass_renderer.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace atrium {

namespace {

constexpr wchar_t kVisualClass[] = L"AtriumBarVisual";
constexpr wchar_t kInputClass[] = L"AtriumBarInput";
constexpr wchar_t kClockKey[] = L"#clock";
constexpr double kInsetDip = 14; // room for the drop shadow around the shape
constexpr DWORD kStaleMs = 2000;  // present at least this often (echo filter cap)
constexpr UINT_PTR kTooltipTimer = 1;
constexpr UINT kTooltipDelayMs = 450;
constexpr UINT_PTR kFileHoverTimer = 2;
constexpr UINT kFileHoverDelayMs = 600;

int Px(double dip, float scale) { return int(std::lround(dip * scale)); }

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

    render_ = std::thread([this] { RenderThread(); });
}

Bar::~Bar()
{
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
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // never take focus from the user's app
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
        if (pressedLeft_ && !pressed_.empty() && pressed_ != kClockKey && (w & MK_LBUTTON) &&
            (std::abs(pt.x - pressPt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - pressPt_.y) > GetSystemMetrics(SM_CYDRAG))) {
            BeginDrag(pt);
            return 0;
        }
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        std::wstring hover = HitTest(pt);
        if (hover != scene_.hoverKey) {
            scene_.hoverKey = hover;
            HideTooltip();
            if (!hover.empty())
                SetTimer(h, kTooltipTimer, kTooltipDelayMs, nullptr);
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
            if (!fileHoverKey_.empty() && fileHoverKey_ != kClockKey && onFileDragHover)
                onFileDragHover(fileHoverKey_);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        HideTooltip();
        if (!dragging_ && (!scene_.hoverKey.empty() || !pressed_.empty())) {
            scene_.hoverKey.clear();
            scene_.pressedKey.clear();
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
        if (key == kClockKey) {
            if (m == WM_LBUTTONUP && onClockClick)
                onClockClick();
        } else if (launch ? bool(onLaunch) : bool(onActivate)) {
            (launch ? onLaunch : onActivate)(key);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_ && reinterpret_cast<HWND>(l) != h)
            EndDrag(false); // capture taken away (e.g. a system menu opened)
        return 0;
    case WM_RBUTTONUP: {
        if (dragging_)
            return 0;
        std::wstring key = HitTest(pt);
        POINT screen = pt;
        ClientToScreen(h, &screen);
        if (!key.empty() && key != kClockKey && onItemMenu)
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
    if (key == kClockKey)
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

    // Metrics (px). Every icon sits centred on a square plate (hover and
    // active highlight); the plate has the same gap `v` to the top and bottom
    // edge, and its corner radius is the bar's minus that gap, so plate and
    // bar corners are concentric.
    const int inset = Px(kInsetDip, s);
    const int barH = Px(st.height, s);
    const int icon = std::clamp(Px(st.iconSize, s), Px(12, s), barH - Px(16, s));
    int plate = std::max(icon + Px(4, s), std::min(barH - Px(12, s), int(std::lround(icon * 1.55))));
    int gap = std::max(Px(4, s), int(std::lround(icon * 0.18)));
    const int sepGap = Px(8, s);

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

    int n = int(items_.size());
    auto padFor = [&](int p) { return (barH - p) / 2 + Px(4, s); }; // ends: plate gap plus a little air
    auto contentWidth = [&](int count, int p, int g) {
        int w = 2 * padFor(p) + (count ? count * p + (count - 1) * g : 0);
        if (clockW)
            w += (count ? 2 * sepGap : 0) + clockW;
        return std::max(w, barH * 2); // an empty bar is still a pleasant capsule
    };
    const int maxW = std::max(barH * 2, std::min(Px(st.width, s), int(wa.right - wa.left) - 2 * inset));
    // Too many apps: tighten the gaps, then the plates, then drop the ones that do not fit.
    while (n > 0 && contentWidth(n, plate, gap) > maxW && gap > Px(2, s))
        --gap;
    while (n > 0 && contentWidth(n, plate, gap) > maxW && plate > icon + Px(4, s))
        --plate;
    while (n > 0 && contentWidth(n, plate, gap) > maxW)
        --n;
    const int barW = st.autoWidth ? std::min(contentWidth(n, plate, gap), maxW) : maxW;
    const int pad = padFor(plate);
    const int v = (barH - plate) / 2;
    const int pitch = plate + gap;
    const int radius = std::min(Px(st.cornerRadius, s), std::min(barW, barH) / 2);

    const int winW = barW + 2 * inset, winH = barH + 2 * inset;
    const int x = wa.left + (wa.right - wa.left - winW) / 2;
    const int y = wa.bottom - Px(st.margin, s) - barH - inset;

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

    // Scene in window coordinates.
    BarScene scene;
    scene.scale = s;
    scene.bar = D2D1::RectF(float(inset), float(inset), float(inset + barW), float(inset + barH));
    scene.plateRadius = float(std::clamp(radius - v, Px(3, s), plate / 2));
    scene.hoverKey = dragging_ ? std::wstring() : scene_.hoverKey;
    scene.pressedKey = scene_.pressedKey;
    scene.dragKey = dragging_ ? dragKey_ : std::wstring();
    hits_.clear();
    const int clockRight = inset + barW - pad;
    const int clockLeft = clockRight - clockW;
    // Apps are centred in the space left of the clock (matters when autoWidth is off).
    const int appsSpace = (clockW ? clockLeft - (n ? 2 * sepGap : 0) : inset + barW - pad) - (inset + pad);
    const int appsW = n ? n * plate + (n - 1) * gap : 0;
    const int appsLeft = inset + pad + std::max(0, (appsSpace - appsW) / 2);
    const float plateTop = float(inset + v);
    const float iconTop = float(inset) + float(barH - icon) / 2;
    std::map<std::wstring, CachedIcon> keep;
    for (int k = 0; k < n; ++k) {
        const int i = order[size_t(k)];
        const TaskItem& t = items_[size_t(i)];
        SceneItem it;
        it.key = t.key;
        float left = float(appsLeft + k * pitch);
        if (i == dragIndex) // follows the pointer, kept within the app strip
            left = std::clamp(float(dragX_ - grabDx_) - float(plate) / 2, float(appsLeft - gap), float(appsLeft + appsW - plate + gap));
        it.plate = D2D1::RectF(left, plateTop, left + float(plate), plateTop + float(plate));
        float iconLeft = left + float(plate - icon) / 2;
        it.icon = D2D1::RectF(iconLeft, iconTop, iconLeft + float(icon), iconTop + float(icon));
        it.running = !t.windows.empty();
        it.active = t.active;
        it.attention = t.attention;
        // Icons are cached per key until the model reports a new version.
        CachedIcon c;
        if (auto f = icons_.find(t.key); f != icons_.end() && f->second.version == t.iconVersion && f->second.px == icon && f->second.image)
            c = f->second;
        else if (iconProvider) {
            IconImage img = iconProvider(t.key, icon);
            if (!img.empty())
                c = {t.iconVersion, icon, nextIconId_++, std::make_shared<const IconImage>(std::move(img))};
        }
        if (c.image) {
            it.image = c.image;
            it.imageId = c.id;
            keep[t.key] = c;
        }
        scene.items.push_back(std::move(it));
        // Hit cells split the gaps; the first and last reach the bar's ends.
        int hl = k == 0 ? inset : appsLeft + k * pitch - gap / 2;
        int hr = k == n - 1 ? (clockW ? clockLeft - sepGap : inset + barW) : appsLeft + k * pitch + plate + (gap - gap / 2);
        hits_.push_back({t.key, RECT{hl, inset, hr, inset + barH}});
    }
    icons_ = std::move(keep);
    appsLeft_ = appsLeft;
    pitch_ = pitch;
    plate_ = plate;
    gap_ = gap;
    shown_ = n;
    if (clockW) {
        scene.clock = D2D1::RectF(float(clockLeft), plateTop, float(clockRight), plateTop + float(plate));
        scene.timePx = timePx;
        scene.datePx = datePx;
        if (n)
            scene.separatorX = float(clockLeft - sepGap);
        hits_.push_back({kClockKey, RECT{clockLeft - (n ? sepGap : pad), inset, inset + barW, inset + barH}});
    }
    if (!scene.hoverKey.empty() && std::none_of(hits_.begin(), hits_.end(), [&](const Hit& h) { return h.key == scene.hoverKey; }))
        scene.hoverKey.clear(); // the hovered app went away
    scene_ = std::move(scene);

    // The window only changes when the bar's size does (not on every drag move).
    RECT win{x, y, x + winW, y + winH};
    if (!dragging_ || !EqualRect(&win, &applied_) || visible_ != appliedVisible_) {
        SetWindowPos(visual_, HWND_TOPMOST, x, y, winW, winH, SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // Input window right below the visual one, region = the rounded shape.
        SetWindowPos(input_, visual_, x, y, winW, winH, SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        applied_ = win;
        appliedVisible_ = visible_;
    }
    if (appliedRgn_[0] != barW || appliedRgn_[1] != barH || appliedRgn_[2] != radius || appliedRgn_[3] != inset) {
        SetWindowRgn(input_, CreateRoundRectRgn(inset, inset, inset + barW + 1, inset + barH + 1, 2 * radius, 2 * radius), TRUE);
        appliedRgn_[0] = barW;
        appliedRgn_[1] = barH;
        appliedRgn_[2] = radius;
        appliedRgn_[3] = inset;
    }

    window_ = win;
    monitor_ = mi.rcMonitor;
    hmonitor_ = mon;
    scale_ = s;
    UpdateTooltips();
    Publish();
    if (debugLayout_ && !dragging_)
        for (const auto& hit : hits_)
            Log(L"layout: %s %ld,%ld,%ld,%ld", hit.key.c_str(), x + hit.rect.left, y + hit.rect.top, x + hit.rect.right, y + hit.rect.bottom);
}

std::wstring Bar::TooltipText(const std::wstring& key) const
{
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
    int top = window_.top + int(scene_.bar.top) - h - Px(8, scale_);
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
        snapshot_.visible = visible_;
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
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    // Per-minute counters: captured frames, presents, frames skipped as echoes.
    unsigned frames = 0, presents = 0, skipped = 0;
    std::vector<double> workMs;    // capture copy + comparison (+ draw and present), per wake-up
    std::vector<double> presentMs; // the same, only for wake-ups that presented
    winrt::com_ptr<ID3D11Texture2D> frozen; // --debug-capturable only
    ULONGLONG statsStart = GetTickCount64();
    Log(L"render: started on %s, window %ldx%ld", gpu.adapterName.c_str(), snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
    HANDLE handles[] = {quit_, wake_, frame_};
    for (;;) {
        // Animations run at display rate; pending readbacks are polled soon.
        DWORD timeout = animating ? 6 : renderer.ReadbackPending() ? 3 : 1000;
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
        if (!snap.visible) {
            if (frame)
                frame.Close();
            animating = false;
            continue;
        }
        if (snap.hmonitor != MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY))
            return true; // primary monitor changed: new device and capture
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
                if (!dark && lum > 0.66f)
                    dark = true;
                else if (dark && lum < 0.54f)
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
        LARGE_INTEGER nowQpc;
        QueryPerformanceCounter(&nowQpc);
        animating = painter.Animate(snap.scene, double(nowQpc.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart));
        ULONGLONG now = GetTickCount64();
        bool force = snap.generation != drawnGeneration || textKey != drawnText || dark != drawnDark || darkIcons != drawnDarkIcons ||
                     animating || now - lastPresent > kStaleMs;
        if (!changed && !force) {
            ++skipped; // echo of our own present, or nothing new under the glass
            if (hadBackdrop)
                workMs.push_back(elapsedMs());
            continue;
        }
        bool ok = renderer.Present(ParamsFor(snap.settings, snap.scale),
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
