#include "ui/launchpad.h"

#include "clock.h"
#include "localization.h"
#include "shell/launchpad_order.h"
#include "ui/launchpad_page_ids.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace atrium {

namespace launchpad {

constexpr double kOpenMs = 300, kCloseMs = 200;
constexpr double kLoadWaitMs = 4000; // the longest the spinner waits for the icons
// The window: about two thirds of the work area, within these bounds (DIP), and 24 DIP clear of its edges.
constexpr float kMinW = 680, kMaxW = 960, kMinH = 480, kMaxH = 660, kMinRadius = 26, kClear = 24;
constexpr float kVeil = 0.45f; // launchpadMaterial tint: the black over the glass
constexpr float kSearchW = 280, kSearchH = 34, kSearchTop = 34;
constexpr float kLabel = 12.5f, kLabelH = 18, kLabelGap = 8;
constexpr float kDotsFromBottom = 40, kDotStep = 18, kDot = 4;
constexpr motion::Spring kPageSpring{0.42f, 0.9f};

} // namespace launchpad

using namespace launchpad;

namespace {

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

} // namespace

Launchpad::Launchpad(HINSTANCE instance, AppCatalog& catalog) : surface_(instance, L"Atrium Launchpad", true, 0), catalog_(catalog)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(false); };
}

void Launchpad::Open(RECT work, HWND previous)
{
    previous_ = previous;
    order_ = LoadLaunchpadOrder();
    lifted_ = -1;
    stale_ = false;
    tilePos_.clear();
    apps_ = LaunchableApps();
    Split();
    folderOpen_ = false;
    query_.clear();
    composition_.clear();
    searchFocused_ = false;
    caretX_ = 0;
    shown_.clear();
    Size(work);
    iconPx_ = IconPx(work);
    const float s = PopupScale(work);
    const LONG cx = (work.left + work.right) / 2, cy = (work.top + work.bottom) / 2;
    const LONG halfW = std::lround(w_ * s / 2), halfH = std::lround(h_ * s / 2);
    const RECT rect = full_ ? Monitor(work) : RECT{cx - halfW, cy - halfH, cx + halfW, cy + halfH};
    page_ = 0;
    pageShown_ = pageSpeed_ = 0;
    sel_ = hover_ = pressed_ = -1;
    down_ = dragging_ = closing_ = launched_ = false;
    wheel_ = 0;
    hoverAnim_.clear();
    KillTimer(surface_.Hwnd(), kCloseTimer);
    openedMs_ = lastPaint_ = revealedMs_ = ClockMs();
    pageIconIds_ = PageIconIds(page_);
    loading_ = !pageIconIds_.empty() && !catalog_.IconsReady(pageIconIds_, iconPx_); // current page icons first
    surface_.SetRadius(Radius());
    surface_.Show(rect, true, POINT{cx, cy}, false);
    UpdatePageTargets(false);
    PublishGlass(openedMs_);
    if (const UINT blink = GetCaretBlinkTime(); blink != INFINITE)
        SetTimer(surface_.Hwnd(), kCaretTimer, std::max(1u, blink), nullptr);
    PlaceIme();
    catalog_.Refresh();
}

void Launchpad::Size(RECT work)
{
    const float s = PopupScale(work);
    if (full_) { // the monitor, the bar's included
        const RECT m = Monitor(work);
        w_ = float(m.right - m.left) / s;
        h_ = float(m.bottom - m.top) / s;
        grid_ = ComputeLaunchpadGrid(w_, h_, true);
        return;
    }
    const float workW = float(work.right - work.left) / s, workH = float(work.bottom - work.top) / s;
    w_ = std::min(std::clamp(workW * 0.62f, kMinW, kMaxW), workW - 2 * kClear);
    h_ = std::min(std::clamp(workH * 0.66f, kMinH, kMaxH), workH - 2 * kClear);
    grid_ = ComputeLaunchpadGrid(w_, h_);
}

RECT Launchpad::Monitor(RECT work)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&work, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

std::vector<CatalogApp> Launchpad::LaunchableApps() const
{
    std::vector<CatalogApp> apps = catalog_.Apps();
    std::erase_if(apps, [](const CatalogApp& a) { return !AppCatalog::IsLaunchable(a); });
    // As on macOS: Windows' own first, then the apps in the order they came (by name among equals).
    std::ranges::stable_sort(apps, {}, &CatalogApp::installed);
    return apps;
}

std::vector<std::wstring> Launchpad::PageIconIds(int page) const { return ExpandLaunchpadPageIds(Shown(), system_, page, grid_.perPage, kFolderId); }

void Launchpad::UpdatePageTargets(bool prioritize)
{
    pageIconIds_ = PageIconIds(page_);
    if (!surface_.Visible() || closing_)
        return;
    std::vector<std::wstring> adjacent;
    const int pages = Pages();
    if (page_ + 1 < pages)
        adjacent = PageIconIds(page_ + 1);
    if (page_ > 0) {
        std::vector<std::wstring> prev = PageIconIds(page_ - 1);
        adjacent.insert(adjacent.end(), prev.begin(), prev.end());
    }
    catalog_.SetAheadIcons(adjacent, iconPx_);
    if (prioritize && !pageIconIds_.empty())
        catalog_.IconsReady(pageIconIds_, iconPx_);
}

int Launchpad::IconPx(RECT work) const { return int(std::lround(grid_.iconDip * PopupScale(work))); }

void Launchpad::Prewarm(RECT work)
{
    if (surface_.Visible() || !catalog_.Loaded())
        return;
    order_ = LoadLaunchpadOrder();
    apps_ = LaunchableApps();
    Split();
    Size(work);
    const int px = IconPx(work);
    std::vector<std::wstring> page0 = ExpandLaunchpadPageIds(tiles_, system_, 0, grid_.perPage, kFolderId);
    std::vector<std::wstring> page1 =
        PageCount(tiles_.size(), grid_.perPage) > 1 ? ExpandLaunchpadPageIds(tiles_, system_, 1, grid_.perPage, kFolderId) : std::vector<std::wstring>{};
    std::vector<std::wstring> keyIds = page0;
    keyIds.insert(keyIds.end(), page1.begin(), page1.end());
    if (prewarmed_ == std::make_pair(px, keyIds))
        return;
    prewarmed_ = {px, std::move(keyIds)};
    catalog_.SetAheadIcons(page0, px);
    if (!page1.empty())
        catalog_.PrefetchIcons(page1, px);
}

void Launchpad::Close(bool launched)
{
    if (!surface_.Visible() || closing_)
        return;
    closing_ = true;
    launched_ = launched;
    if (GetCapture() == surface_.Hwnd())
        ReleaseCapture();
    closedMs_ = ClockMs();
    lastClosed_ = GetTickCount64();
    KillTimer(surface_.Hwnd(), kCaretTimer);
    composition_.clear();
    searchFocused_ = false;
    SetTimer(surface_.Hwnd(), kCloseTimer, UINT(kCloseMs) + 20, nullptr);
    surface_.Invalidate();
}

void Launchpad::Finish()
{
    KillTimer(surface_.Hwnd(), kCloseTimer);
    closing_ = false;
    surface_.Hide(false); // the sheet has already faded
    catalog_.SetVisibleIcons(VisibleIconsOwner::Launchpad, {});
    askedIcons_.clear();
    if (!launched_ && previous_ && IsWindow(previous_)) {
        const HWND fg = GetForegroundWindow();
        wchar_t cls[64] = {};
        if (fg && fg != surface_.Hwnd())
            GetClassNameW(fg, cls, int(std::size(cls)));
        if (!fg || fg == surface_.Hwnd() || std::wstring_view(cls) == L"AtriumBarInput")
            SetForegroundWindow(previous_);
    }
}

void Launchpad::Refresh(bool listChanged)
{
    if (!surface_.Visible() || closing_)
        return;
    if (listChanged) {
        if (lifted_ >= 0) {
            stale_ = true;
            return;
        }
        auto apps = LaunchableApps();
        const bool changed = apps.size() != apps_.size() || !std::equal(apps.begin(), apps.end(), apps_.begin(), [](const CatalogApp& a, const CatalogApp& b) {
                                 return a.id == b.id && a.name == b.name;
                             });
        if (changed) {
            apps_ = std::move(apps);
            Split();
            if (!query_.empty())
                Filter();
            page_ = std::min(page_, Pages() - 1);
            UpdatePageTargets(true);
        }
    }
    surface_.Invalidate();
}

void Launchpad::Filter()
{
    shown_.clear();
    const AppCatalog::PreparedQuery q = AppCatalog::PrepareQuery(query_);
    for (const auto& app : apps_)
        if (AppCatalog::MatchesPrepared(app, q))
            shown_.push_back(app);
    page_ = 0;
    pageShown_ = pageSpeed_ = 0;
    sel_ = query_.empty() || shown_.empty() ? -1 : 0; // Enter opens the best match
    UpdatePageTargets(true);
}

// Windows' own apps go into one folder at the end, except its Settings, which people do open.
void Launchpad::Split()
{
    tiles_.clear();
    system_.clear();
    for (const CatalogApp& app : apps_) {
        const bool settings =
            CompareStringOrdinal(app.aumid.c_str(), -1, L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", -1, TRUE) ==
            CSTR_EQUAL;
        (app.system && !settings ? system_ : tiles_).push_back(app);
    }
    if (!system_.empty())
        tiles_.push_back(CatalogApp{.name = Tr(L"System apps"), .id = kFolderId});
    ApplyOrder();
}

void Launchpad::ShowFolder(bool open)
{
    folderOpen_ = open;
    sel_ = hover_ = pressed_ = -1;
    hoverAnim_.clear();
    revealedMs_ = ClockMs(); // its apps settle in as the pages' did
    if (open) {
        page_ = 0;
    } else {
        auto it = std::ranges::find(tiles_, std::wstring_view(kFolderId), &CatalogApp::id);
        const int idx = it != tiles_.end() ? int(it - tiles_.begin()) : int(tiles_.size()) - 1;
        page_ = std::clamp(idx / std::max(1, grid_.perPage), 0, Pages() - 1);
    }
    pageShown_ = float(page_);
    pageSpeed_ = 0;
    UpdatePageTargets(true);
    surface_.Invalidate();
}

int Launchpad::Pages() const { return std::max(1, PageCount(Shown().size(), grid_.perPage)); }

float Launchpad::OpenAmount(double now) const
{
    if (closing_)
        return 1 - motion::EaseOut(float((now - closedMs_) / kCloseMs));
    return motion::EaseOut(float((now - openedMs_) / kOpenMs));
}

D2D1_RECT_F Launchpad::Cell(int slot) const
{
    const int row = slot / grid_.cols, col = slot % grid_.cols;
    const float x = grid_.gridLeft + float(col) * grid_.cellW, y = grid_.gridTop + float(row) * grid_.cellH;
    return D2D1::RectF(x, y, x + grid_.cellW, y + grid_.cellH);
}

D2D1_RECT_F Launchpad::SearchRect() const { return D2D1::RectF(w_ / 2 - kSearchW / 2, kSearchTop, w_ / 2 + kSearchW / 2, kSearchTop + kSearchH); }

float Launchpad::Radius() const { return full_ ? 0.f : std::max(radius_, kMinRadius); }

// The window's glass, fading with the opening and the closing.
void Launchpad::PublishGlass(double now)
{
    const float a = OpenAmount(now);
    ShapeDip sheet{D2D1::RectF(0, 0, w_, h_), Radius()};
    sheet.fade = 1 - a;
    surface_.SetShapes({sheet}, a > 0 && a < 1);
}

float Launchpad::Hover(Canvas& c, int id, bool on)
{
    auto& f = hoverAnim_[id];
    const float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void Launchpad::Paint(Canvas& c)
{
    if (tint_) { // light ink on the veil, whatever the glass under it
        c.theme = Theme::For(false);
        c.inkDark = 0;
        c.ink = {};
    }
    const Theme& t = c.theme;
    const float dt = float(std::clamp(c.now - lastPaint_, 0.0, 100.0)) / 1000.f;
    lastPaint_ = c.now;
    glideMs_ = dt * 1000;
    if (!dragging_) {
        kPageSpring.Step(pageShown_, pageSpeed_, float(page_), dt);
        if (motion::Spring::AtRest(pageShown_, pageSpeed_, float(page_))) {
            pageShown_ = float(page_);
            pageSpeed_ = 0;
        } else {
            c.Continue();
        }
    }
    const float a = OpenAmount(c.now);
    PublishGlass(c.now);
    if (a > 0 && a < 1)
        c.Continue();
    const D2D1_RECT_F all = D2D1::RectF(0, 0, w_, h_);
    if (c.panel)
        c.Fill(all, Radius(), Alpha(t.panel, a));
    if (tint_)
        c.Fill(all, Radius(), D2D1::ColorF(0, 0, 0, kVeil * a));
    if (loading_ && !closing_) {
        if (pageIconIds_.empty() || catalog_.IconsReady(pageIconIds_, iconPx_) || c.now - openedMs_ > kLoadWaitMs) {
            loading_ = false;
            revealedMs_ = c.now;
        }
        c.Continue();
    }
    // The apps settle from a little larger as the sheet fades in (or once their icons are read), and grow
    // away as it fades out.
    c.BeginGroup(a, 1 + (closing_ ? 0.05f : 0.08f) * (1 - a), w_ / 2, h_ / 2);
    hits_.clear();
    visibleIcons_.clear();
    PaintSearch(c, t, a);
    if (loading_) {
        PaintSpinner(c, t, a);
    } else {
        const float r = closing_ || revealedMs_ <= openedMs_ ? 1.f : motion::EaseOut(float((c.now - revealedMs_) / kOpenMs));
        if (r < 1)
            c.Continue();
        c.BeginGroup(r, 1 + 0.08f * (1 - r), w_ / 2, h_ / 2);
        if (Shown().empty() && !query_.empty())
            c.Text(Tr(L"No matching apps"), D2D1::RectF(0, h_ / 2 - 14, w_, h_ / 2 + 14), 17, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        else if (folderOpen_ && query_.empty())
            c.Text(Tr(L"System apps"), D2D1::RectF(0, kSearchTop + kSearchH + 6, w_, grid_.gridTop), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        const int first = int(std::floor(pageShown_));
        for (int page = first; page <= first + 1; ++page)
            if (page >= 0 && page < Pages() && std::abs(float(page) - pageShown_) < 1)
                PaintPage(c, t, page, (float(page) - pageShown_) * w_);
        if (gliding_)
            c.Continue();
        gliding_ = false;
        PaintLifted(c, t);
        PaintDots(c, t);
        c.EndGroup();
    }
    c.EndGroup();
    if (visibleIcons_ != askedIcons_) {
        askedIcons_ = visibleIcons_;
        catalog_.SetVisibleIcons(VisibleIconsOwner::Launchpad, visibleIcons_); // the pages on screen first
    }
}

void Launchpad::PaintSearch(Canvas& c, const Theme& t, float shown)
{
    const D2D1_RECT_F r = SearchRect();
    const float radius = kSearchH / 2;
    c.Fill(r, radius, Alpha(t.hover, 1.3f));
    c.Stroke(r, radius, Alpha(t.text, 0.10f));
    c.Glyph(L"", D2D1::RectF(r.left + 12, r.top, r.left + 30, r.bottom), 13, t.subtext); // Search
    const D2D1_RECT_F text = D2D1::RectF(r.left + 36, r.top, r.right - 14, r.bottom);
    const float maxW = text.right - text.left;
    caretX_ = text.left + std::min(c.TextWidth(query_ + composition_, 13.5f), maxW);
    if (query_.empty() && composition_.empty()) {
        c.Text(Tr(L"Search"), text, 13.5f, t.subtext);
    } else {
        c.Text(query_ + composition_, text, 13.5f, t.text);
        if (!composition_.empty()) {
            const float x0 = text.left + std::min(c.TextWidth(query_, 13.5f), maxW);
            if (caretX_ > x0) {
                const float y = (text.top + text.bottom) / 2 + 9;
                c.Line(x0, y, caretX_, y, t.text, 1);
            }
        }
    }
    // The caret, blinking at the system's pace (kCaretTimer repaints).
    const UINT blink = GetCaretBlinkTime();
    if (shown >= 1 && searchFocused_ && (blink == INFINITE || (ULONGLONG(c.now) / std::max(1u, blink)) % 2 == 0))
        c.Fill(D2D1::RectF(caretX_, r.top + 9, caretX_ + 1.5f, r.bottom - 9), 0.75f, t.text);
    hits_.push_back({r, kSearch});
}

void Launchpad::PaintPage(Canvas& c, const Theme& t, int page, float dx)
{
    const LaunchpadSlice slice = PageSlice(Shown().size(), page, grid_.perPage);
    for (size_t i = slice.begin; i < slice.end; ++i) {
        const D2D1_RECT_F cell = Cell(int(i - slice.begin));
        const D2D1_RECT_F at = D2D1::RectF(cell.left + dx, cell.top, cell.right + dx, cell.bottom);
        if (at.right < 0 || at.left > w_)
            continue;
        if (int(i) == lifted_ && Arranging())
            continue; // under the pointer (PaintLifted)
        PaintTile(c, t, int(i), Arranging() ? Glide(Shown()[i].id, cell, page, dx) : at);
        if (std::abs(dx) < 1)
            hits_.push_back({at, int(i)});
    }
}

void Launchpad::PaintTile(Canvas& c, const Theme& t, int index, D2D1_RECT_F cell)
{
    const CatalogApp& app = Shown()[size_t(index)];
    const float icon = grid_.iconDip, block = icon + kLabelGap + kLabelH;
    const float cx = (cell.left + cell.right) / 2, top = cell.top + (cell.bottom - cell.top - block) / 2;
    const D2D1_RECT_F ir = D2D1::RectF(cx - icon / 2, top, cx + icon / 2, top + icon);
    // Under the pointer or the keys: a soft plate behind the icon and its name.
    const float hv = Hover(c, index, hover_ == index || sel_ == index);
    if (hv > 0.001f) {
        const float half = std::min((cell.right - cell.left) / 2 - 4, icon * 0.5f + 22);
        const D2D1_RECT_F plate = D2D1::RectF(cx - half, top - 12, cx + half, top + block + 10);
        c.Fill(plate, 18, Alpha(sel_ == index && hover_ != index ? t.accent : t.hover, (sel_ == index && hover_ != index ? 0.28f : 1.f) * hv));
    }
    c.BeginGroup(1, pressed_ == index ? 0.92f : 1 + 0.03f * hv, cx, top + icon / 2);
    if (app.id == kFolderId)
        PaintFolder(c, t, ir);
    else
        PaintIcon(c, t, app.id, ir);
    c.EndGroup();
    c.Text(app.name, D2D1::RectF(cell.left + 6, ir.bottom + kLabelGap, cell.right - 6, ir.bottom + kLabelGap + kLabelH), kLabel, t.text,
           DWRITE_FONT_WEIGHT_NORMAL, 1);
}

void Launchpad::PaintIcon(Canvas& c, const Theme& t, const std::wstring& id, D2D1_RECT_F r)
{
    // Read at the tiles' size (the folder's small ones too, drawn smaller), so each is read once.
    const int px = int(std::lround(grid_.iconDip * c.scale));
    const std::wstring key = L"lp|" + id + L"|" + std::to_wstring(px);
    visibleIcons_.emplace_back(id, px);
    if (c.Cached(key))
        c.Image(key, IconImage{}, r);
    else if (IconImage image = catalog_.Icon(id, px); !image.Empty())
        c.Image(key, image, r);
    else
        c.Fill(r, (r.right - r.left) * 0.22f, Alpha(t.hover, 1.4f));
}

// A folder as on macOS: a plate of the glass's wash holding its first nine apps, three by three.
void Launchpad::PaintFolder(Canvas& c, const Theme& t, D2D1_RECT_F r)
{
    const float s = r.right - r.left, pad = s * 0.13f, gap = s * 0.05f, mini = (s - 2 * pad - 2 * gap) / 3;
    c.Fill(r, s * 0.24f, Alpha(t.hover, 1.6f));
    for (size_t i = 0; i < system_.size() && i < 9; ++i) {
        const int col = int(i % 3), row = int(i / 3);
        const float x = r.left + pad + float(col) * (mini + gap), y = r.top + pad + float(row) * (mini + gap);
        PaintIcon(c, t, system_[i].id, D2D1::RectF(x, y, x + mini, y + mini));
    }
}

// While the icons are read: an arc turning once a second in the middle, shown only if it takes a moment.
void Launchpad::PaintSpinner(Canvas& c, const Theme& t, float shown)
{
    const float fade = std::clamp(float((c.now - openedMs_ - 150) / 200), 0.f, 1.f) * shown;
    if (fade <= 0)
        return;
    const float from = float(std::fmod(c.now * 0.36, 360.0));
    c.Arc(w_ / 2, h_ / 2, 13, from, from + 270, Alpha(t.subtext, fade), 2.5f);
}

void Launchpad::PaintDots(Canvas& c, const Theme& t)
{
    const int pages = Pages();
    if (pages <= 1)
        return;
    const float y = h_ - kDotsFromBottom, x0 = w_ / 2 - float(pages - 1) * kDotStep / 2;
    for (int p = 0; p < pages; ++p) {
        const float x = x0 + float(p) * kDotStep;
        const float lit = std::max(0.f, 1 - std::abs(pageShown_ - float(p))); // lit as its page comes
        c.Circle(x, y, kDot, Alpha(t.text, 0.28f + 0.62f * lit));
        hits_.push_back({D2D1::RectF(x - kDotStep / 2, y - 12, x + kDotStep / 2, y + 12), -2 - p});
    }
}

} // namespace atrium
