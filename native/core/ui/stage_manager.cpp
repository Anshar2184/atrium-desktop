#include "ui/stage_manager.h"

#include "clock.h"
#include "log.h"
#include "settings.h"

#include <windowsx.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

// DIP. A card: the pictures, then the app's icon and name; cards one above the other, kEdge from
// the screen's edge, in the middle of what the bar leaves of the screen's height.
constexpr float kEdge = 16, kCardW = 168, kPad = 8, kImageH = 100, kTitleH = 26, kCardGap = 14, kCardR = 18;
constexpr float kCardH = kPad + kImageH + kTitleH + 4;
constexpr float kStackOff = 5;   // each window behind the front one shows this much at its top and right
constexpr size_t kStack = 3;     // windows shown per app
constexpr float kBarRoom = 90;   // the bar floats over the bottom of the work area
constexpr int kMaxEntries = 6;
constexpr UINT_PTR kWatchTimer = 0x5C01, kFadeTimer = 0x5C02, kRaiseTimer = 0x5C03, kHideTimer = 0x5C04;
constexpr UINT kWatchMs = 120, kFadeMs = 160, kRaiseMs = 220;
constexpr UINT WM_STAGE_THUMB = WM_APP + 0x5C; // a picture's source size is known
constexpr ULONGLONG kEdgeMs = 140, kPeekLeaveMs = 450, kPendingMs = 1200, kSettleMs = 2000;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

std::wstring StatePath()
{
    std::wstring path = SettingsPath();
    return path.substr(0, path.find_last_of(L'\\')) + L"\\stage.json";
}

// Back without taking the foreground. Never a synchronous call into another app's window: one that
// hangs must not hang the bar.
void ShowQuietly(HWND h) { ShowWindowAsync(h, SW_SHOWNOACTIVATE); }

bool OnMonitor(HWND h, HMONITOR m) { return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == m; }

bool Has(const std::vector<std::wstring>& set, const std::wstring& key) { return std::find(set.begin(), set.end(), key) != set.end(); }

} // namespace

StageManager::StageManager(HINSTANCE instance, TaskModel& tasks)
    : tasks_(tasks), surface_(instance, L"Atrium Stage Manager", false), thumbs_(surface_.Hwnd(), WM_STAGE_THUMB)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onReady = [this] {
        // The glass is out: the pictures fade in over it.
        fadeStart_ = ClockMs();
        SetTimer(surface_.Hwnd(), kFadeTimer, 15, nullptr);
    };
    surface_.SetRadius(kCardR);
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(desktops_.put()))))
        desktops_ = nullptr;
    Load(); // windows a previous run put away (it may not have ended well)
}

StageManager::~StageManager() { DropThumbs(); }

// ---- the stage ---------------------------------------------------------------------------------

void StageManager::SetEnabled(bool on)
{
    if (on == enabled_ && applied_)
        return;
    applied_ = true;
    enabled_ = on;
    if (on) {
        stage_.clear();
        pending_.clear();
        SetTimer(surface_.Hwnd(), kWatchTimer, kWatchMs, nullptr);
        Log(L"stage: on (%s), %zu window(s) waiting from before", onRight_ ? L"right" : L"left", parked_.size());
        Update(tasks_.Snapshot(true));
    } else {
        KillTimer(surface_.Hwnd(), kWatchTimer);
        size_t n = parked_.size();
        HWND front = GetForegroundWindow();
        RestoreAll();
        // The windows coming back must not cover the one in front.
        if (n && front) {
            raise_ = front;
            SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
        }
        Log(L"stage: off, %zu window(s) back", n);
    }
    if (onReserve)
        onReserve();
}

void StageManager::SetOnRight(bool onRight)
{
    if (onRight == onRight_)
        return;
    onRight_ = onRight;
    if (surface_.Visible())
        Hide(); // it comes out again on the other side
    Rebuild();
    if (onReserve)
        onReserve();
}

void StageManager::SetMonitor(HMONITOR monitor)
{
    if (monitor == monitor_)
        return;
    monitor_ = monitor;
    Rebuild();
}

float StageManager::Reserve() const { return enabled_ ? kEdge + kCardW + 8 : 0; }

void StageManager::Update(const std::vector<TaskItem>& items)
{
    items_ = items;
    if (!enabled_)
        return;
    Prune();
    if (!pending_.empty() && GetTickCount64() > pendingUntil_)
        pending_.clear();
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    auto active = std::find_if(items_.begin(), items_.end(), [](const TaskItem& t) { return t.active; });
    // A window in front, on this screen, of an app not on the stage: its set takes the stage. Not
    // while a set picked in the strip is on its way to the front.
    if (active != items_.end() && root && !IsIconic(root) && OnMonitor(root, monitor_)) {
        if (pending_.empty() || Has(pending_, active->key)) {
            pending_.clear();
            if (!Has(stage_, active->key))
                Stage(SetOf(active->key), false, root, false);
        }
    }
    Rebuild();
}

StageManager::Set StageManager::SetOf(const std::wstring& key) const
{
    for (const Set& set : recent_)
        if (Has(set, key))
            return set;
    return Set{key};
}

// `keys` take the stage (or join it: add). Their windows that were put away come back, `front` in
// front; then every other app's windows on this screen go to the strip: the apps that were on the
// stage as one set, at the top.
void StageManager::Stage(const Set& keys, bool add, HWND front, bool activate)
{
    Unpark(keys, front, activate);
    if (!add) {
        Set left; // the stage's apps that had windows up
        std::vector<Set> others;
        for (const TaskItem& t : items_) {
            if (Has(keys, t.key))
                continue;
            bool any = false;
            // Least recent first: the most recent window gets the latest number and shows in front.
            for (auto h = t.windows.rbegin(); h != t.windows.rend(); ++h)
                if (IsWindowVisible(*h) && !IsIconic(*h) && OnMonitor(*h, monitor_)) {
                    Park(*h, t.key);
                    any = true;
                }
            if (!any)
                continue;
            if (Has(stage_, t.key))
                left.push_back(t.key);
            else
                others.push_back(Set{t.key});
        }
        if (!left.empty())
            others.insert(others.begin(), left);
        // An app is in one set only: the newest.
        for (const Set& set : others)
            for (const std::wstring& k : set)
                for (Set& old : recent_)
                    std::erase(old, k);
        std::erase_if(recent_, [](const Set& set) { return set.empty(); });
        recent_.insert(recent_.begin(), others.begin(), others.end());
        stage_.clear();
        if (!others.empty())
            Log(L"stage: %s on the stage, %zu set(s) to the strip", keys.front().c_str(), others.size());
    } else {
        Log(L"stage: %s joins the stage", keys.front().c_str());
    }
    for (const std::wstring& k : keys) {
        if (!Has(stage_, k))
            stage_.push_back(k);
        for (Set& old : recent_)
            std::erase(old, k);
    }
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    Save();
}

void StageManager::Park(HWND window, const std::wstring& key)
{
    Parked p;
    p.key = key;
    p.seq = ++seq_;
    p.since = GetTickCount64();
    // The picture without the invisible resize borders: measured now, while the window is up.
    RECT win{}, frame{};
    if (GetWindowRect(window, &win) && SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
        frame.right > frame.left && frame.bottom > frame.top)
        p.source = RECT{frame.left - win.left, frame.top - win.top, frame.right - win.left, frame.bottom - win.top};
    parked_[window] = p;
    ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
}

// The windows of `keys` that were put away come back, least recent first; `front` (by default the
// most recent of them) last, and in front, activated if asked (a pick in the strip).
void StageManager::Unpark(const Set& keys, HWND front, bool activate)
{
    std::vector<std::pair<uint64_t, HWND>> mine;
    for (const auto& [h, p] : parked_)
        if (Has(keys, p.key))
            mine.emplace_back(p.seq, h);
    std::sort(mine.begin(), mine.end());
    if (!front && !mine.empty())
        front = mine.back().second;
    for (const auto& [seq, h] : mine) {
        parked_.erase(h);
        if (h != front && IsWindow(h))
            ShowQuietly(h);
    }
    if (!front)
        return;
    parked_.erase(front);
    if (activate) {
        // Restoring waits for the app to answer (and for its animation): not on the UI thread,
        // whose strip is gliding meanwhile. The click on the strip lets this process hand on the
        // foreground from any of its threads.
        std::thread([front] { TaskModel::ActivateWindow(front); }).detach();
    } else if (IsIconic(front))
        ShowQuietly(front);
    if (!mine.empty()) {
        // The others come back asynchronously: `front` goes on top again once they have.
        raise_ = front;
        SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
    }
}

void StageManager::Pick(const Set& keys, bool add)
{
    pending_ = keys;
    pendingUntil_ = GetTickCount64() + kPendingMs;
    hover_ = pressed_ = -1;
    Stage(keys, add, nullptr, true);
    Rebuild();
}

void StageManager::RestoreAll()
{
    std::vector<std::pair<uint64_t, HWND>> all;
    for (const auto& [h, p] : parked_)
        all.emplace_back(p.seq, h);
    std::sort(all.begin(), all.end());
    for (const auto& [seq, h] : all)
        if (IsWindow(h) && IsIconic(h))
            ShowQuietly(h);
    parked_.clear();
    recent_.clear();
    stage_.clear();
    pending_.clear();
    Save();
    Rebuild();
}

// Windows that went (closed, hidden to the tray) or came back some other way are no longer waiting.
void StageManager::Prune()
{
    const ULONGLONG now = GetTickCount64();
    std::erase_if(parked_, [&](const auto& kv) {
        HWND h = kv.first;
        if (!IsWindow(h) || !IsWindowVisible(h))
            return true;
        // Minimizing is asynchronous: a window just put away may not be down yet.
        return !IsIconic(h) && now - kv.second.since > kSettleMs;
    });
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) {
            return std::none_of(parked_.begin(), parked_.end(), [&](const auto& kv) { return kv.second.key == k; });
        });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    Save();
}

void StageManager::Save()
{
    json::JsonArray list;
    std::vector<std::pair<uint64_t, HWND>> all;
    for (const auto& [h, p] : parked_)
        all.emplace_back(p.seq, h);
    std::sort(all.begin(), all.end());
    for (const auto& [seq, h] : all) {
        json::JsonObject o;
        o.SetNamedValue(L"hwnd", json::JsonValue::CreateNumberValue(double(reinterpret_cast<uintptr_t>(h))));
        o.SetNamedValue(L"key", json::JsonValue::CreateStringValue(parked_[h].key));
        list.Append(o);
    }
    json::JsonArray sets;
    for (const Set& set : recent_) {
        json::JsonArray keys;
        for (const std::wstring& k : set)
            keys.Append(json::JsonValue::CreateStringValue(k));
        sets.Append(keys);
    }
    json::JsonObject root;
    root.SetNamedValue(L"parked", list);
    root.SetNamedValue(L"sets", sets);
    std::wstring text(root.Stringify());
    if (text == saved_)
        return;
    saved_ = text;
    if (!WriteTextFile(StatePath(), text))
        Log(L"stage: could not write %s", StatePath().c_str());
}

void StageManager::Load()
{
    json::JsonObject root{nullptr};
    std::wstring text = ReadTextFile(StatePath());
    if (text.empty() || !json::JsonObject::TryParse(text, root))
        return;
    saved_ = text;
    if (root.HasKey(L"parked") && root.GetNamedValue(L"parked").ValueType() == json::JsonValueType::Array)
        for (const auto& v : root.GetNamedArray(L"parked")) {
            if (v.ValueType() != json::JsonValueType::Object)
                continue;
            auto o = v.GetObject();
            if (!o.HasKey(L"hwnd") || !o.HasKey(L"key") || o.GetNamedValue(L"hwnd").ValueType() != json::JsonValueType::Number ||
                o.GetNamedValue(L"key").ValueType() != json::JsonValueType::String)
                continue;
            HWND h = reinterpret_cast<HWND>(uintptr_t(o.GetNamedNumber(L"hwnd")));
            // A window handle is only reused once its window is gone; one still down is the same.
            if (!IsWindow(h) || !IsWindowVisible(h) || !IsIconic(h))
                continue;
            Parked p;
            p.key = std::wstring(o.GetNamedString(L"key"));
            p.seq = ++seq_;
            parked_[h] = p;
        }
    if (root.HasKey(L"sets") && root.GetNamedValue(L"sets").ValueType() == json::JsonValueType::Array)
        for (const auto& v : root.GetNamedArray(L"sets")) {
            if (v.ValueType() != json::JsonValueType::Array)
                continue;
            Set set;
            for (const auto& k : v.GetArray())
                if (k.ValueType() == json::JsonValueType::String && !Has(set, std::wstring(k.GetString())))
                    set.push_back(std::wstring(k.GetString()));
            if (!set.empty())
                recent_.push_back(std::move(set));
        }
    // Apps only known from their windows: each a set of its own.
    for (const auto& [h, p] : parked_)
        if (std::none_of(recent_.begin(), recent_.end(), [&](const Set& set) { return Has(set, p.key); }))
            recent_.push_back(Set{p.key});
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) {
            return std::none_of(parked_.begin(), parked_.end(), [&](const auto& kv) { return kv.second.key == k; });
        });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
}

// ---- the strip ---------------------------------------------------------------------------------

RECT StageManager::WorkArea() const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcWork;
}

int StageManager::Capacity() const
{
    RECT wa = WorkArea();
    float avail = float(wa.bottom - wa.top) / PopupScale(wa) - 2 * kEdge - kBarRoom;
    return std::clamp(int((avail + kCardGap) / (kCardH + kCardGap)), 1, kMaxEntries);
}

void StageManager::DropThumbs(Entry& e)
{
    for (uint64_t id : e.thumbs)
        thumbs_.Unregister(id);
    e.thumbs.clear();
    e.shown.clear();
}

RECT StageManager::SourceOf(HWND window, SIZE size) const
{
    // Without the invisible resize borders, as measured when the window went (if it still fits).
    RECT full{0, 0, size.cx, size.cy};
    auto p = parked_.find(window);
    if (p == parked_.end())
        return full;
    const RECT& r = p->second.source;
    if (r.right <= r.left || r.bottom <= r.top || r.right > size.cx + 1 || r.bottom > size.cy + 1)
        return full;
    return RECT{r.left, r.top, std::min(r.right, full.right), std::min(r.bottom, full.bottom)};
}

void StageManager::DropThumbs()
{
    for (Entry& e : entries_)
        DropThumbs(e);
    for (Entry& e : leaving_)
        DropThumbs(e);
    leaving_.clear();
}

void StageManager::Rebuild()
{
    std::vector<Entry> next;
    if (enabled_) {
        const int cap = Capacity();
        for (const Set& set : recent_) {
            if (int(next.size()) >= cap)
                break;
            std::vector<std::pair<uint64_t, HWND>> mine;
            for (const auto& [h, p] : parked_) {
                if (!Has(set, p.key) || !IsWindow(h))
                    continue;
                BOOL here = TRUE; // windows put away on another virtual desktop wait there
                if (desktops_ && SUCCEEDED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) && !here)
                    continue;
                mine.emplace_back(p.seq, h);
            }
            if (mine.empty())
                continue;
            std::sort(mine.rbegin(), mine.rend()); // most recent first
            Entry e;
            e.keys = set;
            // The apps' names, the most recent first.
            for (const auto& [seq, h] : mine) {
                const std::wstring& key = parked_[h].key;
                auto item = std::find_if(items_.begin(), items_.end(), [&](const TaskItem& t) { return t.key == key; });
                if (item == items_.end() || item->name.empty() || e.name.find(item->name) != std::wstring::npos)
                    continue;
                e.name += (e.name.empty() ? L"" : L"、") + item->name;
            }
            std::stable_sort(e.keys.begin(), e.keys.end(), [&](const std::wstring& a, const std::wstring& b) {
                auto newest = [&](const std::wstring& k) {
                    uint64_t n = 0;
                    for (const auto& [seq, h] : mine)
                        if (parked_[h].key == k)
                            n = std::max(n, seq);
                    return n;
                };
                return newest(a) > newest(b);
            });
            for (size_t i = 0; i < mine.size() && i < kStack; ++i)
                e.windows.push_back(mine[i].second);
            next.push_back(std::move(e));
        }
    }
    const bool visible = surface_.Visible();
    // A set keeps its card (where it is, its pictures if its windows are the same); DWM draws the
    // pictures in the order they were registered, so changed ones register again, back first.
    for (Entry& e : next) {
        auto old = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& o) { return o.keys == e.keys; });
        if (old != entries_.end()) {
            e.hover = old->hover;
            e.y = old->y;
            e.vy = old->vy;
            e.appear = old->appear;
            e.va = old->va;
            if (old->windows == e.windows) {
                e.thumbs = std::move(old->thumbs);
                e.shown = std::move(old->shown);
                old->thumbs.clear();
            }
            old->keys.clear(); // taken
        } else {
            e.appear = visible ? 0.f : 1.f; // a new card fades in; a strip coming out pops as a whole
        }
        if (e.thumbs.empty()) {
            // Back first: DWM stacks the pictures in the order they were registered.
            e.thumbs.assign(e.windows.size(), 0);
            for (size_t j = e.windows.size(); j-- > 0;)
                e.thumbs[j] = thumbs_.Register(surface_.Hwnd(), e.windows[j]);
        }
    }
    // Cards that went fade out where they were (their pictures with them).
    for (Entry& old : entries_) {
        if (old.keys.empty()) {
            DropThumbs(old);
            continue;
        }
        if (!visible || old.appear < 0.02f) {
            DropThumbs(old);
            continue;
        }
        old.targetAppear = 0;
        old.hover = motion::Follow{};
        leaving_.push_back(std::move(old));
    }
    if (!visible)
        DropThumbs(), leaving_.clear();
    const bool changed = next.size() != entries_.size() ||
                         !std::equal(next.begin(), next.end(), entries_.begin(), [](const Entry& a, const Entry& b) { return a.keys == b.keys; }) ||
                         !leaving_.empty();
    entries_ = std::move(next);
    if (hover_ >= int(entries_.size()))
        hover_ = -1;
    if (pressed_ >= int(entries_.size()))
        pressed_ = -1;
    Layout();
    if (entries_.empty() && leaving_.empty()) {
        Hide();
        return;
    }
    if (surface_.Visible()) {
        surface_.Move(rect_);
        surface_.Invalidate();
    }
    if (changed)
        Watch();
}

// The strip's window has room for Capacity() cards, in the middle of what the bar leaves of the
// screen's height; the cards stand in the middle of it, and glide when that changes.
void StageManager::Layout()
{
    RECT wa = WorkArea();
    const float s = PopupScale(wa);
    const int cap = Capacity();
    size_ = D2D1::SizeF(kCardW, float(cap) * kCardH + float(cap - 1) * kCardGap);
    const size_t n = entries_.size();
    const float used = n ? float(n) * kCardH + float(n - 1) * kCardGap : 0;
    const float top = (size_.height - used) / 2;
    const bool snap = !surface_.Visible();
    for (size_t i = 0; i < n; ++i) {
        Entry& e = entries_[i];
        e.targetY = top + float(i) * (kCardH + kCardGap);
        if (snap || e.appear == 0) {
            e.y = e.targetY; // a new card fades in at its place
            e.vy = 0;
        }
    }
    const LONG w = LONG(std::lround(size_.width * s)), h = LONG(std::lround(size_.height * s));
    const LONG edge = LONG(std::lround(kEdge * s)), room = LONG(std::lround(kBarRoom * s));
    LONG x = onRight_ ? wa.right - edge - w : wa.left + edge;
    LONG y = wa.top + std::max<LONG>(edge, (wa.bottom - wa.top - room - h) / 2);
    rect_ = RECT{x, y, x + w, y + h};
}

namespace {
constexpr motion::Spring kGlide{0.42f, 0.86f};
constexpr motion::Spring kFadeSpring{0.30f, 1.f};

D2D1_RECT_F ScaleRect(D2D1_RECT_F r, float k, float cx, float cy)
{
    return D2D1::RectF(cx + (r.left - cx) * k, cy + (r.top - cy) * k, cx + (r.right - cx) * k, cy + (r.bottom - cy) * k);
}
} // namespace

bool StageManager::Animate(double nowMs)
{
    const float dt = lastStep_ > 0 ? float(std::clamp((nowMs - lastStep_) / 1000.0, 0.0, 0.05)) : 0.f;
    lastStep_ = nowMs;
    bool moving = false;
    auto step = [&](Entry& e) {
        kGlide.Step(e.y, e.vy, e.targetY, dt);
        kFadeSpring.Step(e.appear, e.va, e.targetAppear, dt);
        if (motion::Spring::AtRest(e.y, e.vy, e.targetY, 0.2f))
            e.y = e.targetY, e.vy = 0;
        else
            moving = true;
        if (motion::Spring::AtRest(e.appear, e.va, e.targetAppear, 0.004f))
            e.appear = e.targetAppear, e.va = 0;
        else
            moving = true;
        e.appear = std::clamp(e.appear, 0.f, 1.f);
        e.rect = D2D1::RectF(0, e.y, kCardW, e.y + kCardH);
        e.image = D2D1::RectF(kPad, e.y + kPad, kCardW - kPad, e.y + kPad + kImageH);
    };
    for (Entry& e : entries_)
        step(e);
    for (Entry& e : leaving_)
        step(e);
    for (Entry& e : leaving_)
        if (e.appear <= 0.01f)
            DropThumbs(e);
    std::erase_if(leaving_, [](const Entry& e) { return e.appear <= 0.01f; });
    if (!moving)
        lastStep_ = 0;
    return moving;
}

// The cards' span and their shadows: the rest of the column lets clicks through. While cards move
// the region only grows (where they go is in it already); it is fitted again when they rest.
void StageManager::UpdateRegion(bool moving)
{
    float top = size_.height, bottom = 0;
    for (const auto* list : {&entries_, &leaving_})
        for (const Entry& e : *list) {
            top = std::min({top, e.rect.top, e.targetY});
            bottom = std::max({bottom, e.rect.bottom, e.targetY + kCardH});
        }
    RECT wr{};
    if (!GetWindowRect(surface_.Hwnd(), &wr))
        return;
    const float s = surface_.Scale();
    POINT a = surface_.ContentToScreen(POINT{0, LONG(std::floor(top * s))}), b = surface_.ContentToScreen(POINT{0, LONG(std::ceil(bottom * s))});
    const LONG inset = rect_.left - wr.left;
    RECT r{0, std::max<LONG>(0, a.y - wr.top - inset), wr.right - wr.left, std::min<LONG>(wr.bottom - wr.top, b.y - wr.top + inset)};
    if (bottom <= top)
        r = RECT{0, 0, 0, 0};
    if (moving && !IsRectEmpty(&region_))
        UnionRect(&r, &r, &region_);
    if (EqualRect(&r, &region_))
        return;
    region_ = r;
    HRGN rgn = CreateRectRgn(r.left, r.top, r.right, r.bottom);
    if (!SetWindowRgn(surface_.Hwnd(), rgn, FALSE))
        DeleteObject(rgn);
}

RECT StageManager::Zone() const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const LONG pad = LONG(std::lround(8 * PopupScale(rect_)));
    return onRight_ ? RECT{rect_.left - pad, rect_.top - pad, mi.rcMonitor.right, rect_.bottom + pad}
                    : RECT{mi.rcMonitor.left, rect_.top - pad, rect_.right + pad, rect_.bottom + pad};
}

bool StageManager::Covered() const
{
    const RECT zone = Zone();
    for (const TaskItem& t : items_)
        for (HWND h : t.windows) {
            if (parked_.contains(h) || !IsWindowVisible(h) || IsIconic(h) || !OnMonitor(h, monitor_))
                continue;
            RECT r{}, hit{};
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(h, &r))
                continue;
            BOOL cloaked = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                continue; // on another virtual desktop
            if (IntersectRect(&hit, &r, &zone))
                return true;
        }
    return false;
}

// Every kWatchMs while on: the strip shows unless a window covers its place; then it comes out
// over the window while the pointer rests on that edge of the screen, and goes once it has left.
void StageManager::Watch()
{
    if (!enabled_ || entries_.empty()) {
        peek_ = false;
        if (leaving_.empty())
            Hide();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    POINT p{};
    GetCursorPos(&p);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT zone = Zone();
    const bool atEdge = (onRight_ ? p.x >= mi.rcMonitor.right - 2 : p.x <= mi.rcMonitor.left + 1) && p.y >= zone.top && p.y < zone.bottom;
    edgeSince_ = atEdge ? (edgeSince_ ? edgeSince_ : now) : 0;
    if (!Covered()) {
        peek_ = false;
        Show();
        return;
    }
    const bool over = surface_.Visible() && PtInRect(&zone, p);
    if (!peek_ && edgeSince_ && now - edgeSince_ >= kEdgeMs) {
        peek_ = true;
        leftAt_ = 0;
    }
    if (peek_) {
        if (over || atEdge || inside_)
            leftAt_ = 0;
        else if (!leftAt_)
            leftAt_ = now;
        else if (now - leftAt_ >= kPeekLeaveMs)
            peek_ = false;
    }
    if (peek_)
        Show();
    else
        Hide();
}

void StageManager::Show()
{
    if (surface_.Visible()) {
        surface_.Move(rect_);
        return;
    }
    thumbAlpha_ = 0;
    hover_ = pressed_ = -1;
    for (Entry& e : leaving_)
        DropThumbs(e);
    leaving_.clear();
    for (Entry& e : entries_) {
        e.y = e.targetY, e.vy = 0;
        e.appear = e.targetAppear = 1, e.va = 0;
        e.rect = D2D1::RectF(0, e.y, kCardW, e.y + kCardH);
        e.image = D2D1::RectF(kPad, e.y + kPad, kCardW - kPad, e.y + kPad + kImageH);
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    // Out of the screen's edge.
    POINT anchor{onRight_ ? mi.rcMonitor.right : mi.rcMonitor.left, (rect_.top + rect_.bottom) / 2};
    surface_.Show(rect_, false, anchor);
    UpdateRegion(false);
    UpdateThumbs();
}

void StageManager::Hide()
{
    if (!surface_.Visible())
        return;
    KillTimer(surface_.Hwnd(), kFadeTimer);
    thumbAlpha_ = 0;
    UpdateThumbs(); // the pictures go first: they cannot shrink with the glass
    inside_ = false;
    hover_ = pressed_ = -1;
    surface_.Hide();
}

void StageManager::UpdateThumbs()
{
    const float s = surface_.Scale();
    const bool shown = surface_.Visible() && thumbAlpha_ > 0.01f;
    for (auto* list : {&entries_, &leaving_})
        for (Entry& e : *list) {
            const size_t n = e.windows.size();
            const float off = kStackOff * float(n - 1);
            const float k = 0.94f + 0.06f * e.appear, cx = (e.rect.left + e.rect.right) / 2, cy = (e.rect.top + e.rect.bottom) / 2;
            // The front window's room; each one behind it a step up and to the right.
            const D2D1_RECT_F front = D2D1::RectF(e.image.left, e.image.top + off, e.image.right - off, e.image.bottom);
            for (size_t j = 0; j < n && j < e.thumbs.size(); ++j) {
                const SIZE size = thumbs_.SourceSize(e.thumbs[j]);
                if (size.cx <= 0 || size.cy <= 0)
                    continue; // not there yet (or no picture)
                const RECT src = SourceOf(e.windows[j], size);
                float sw = float(std::max<LONG>(1, src.right - src.left)), sh = float(std::max<LONG>(1, src.bottom - src.top));
                float bw = front.right - front.left, bh = front.bottom - front.top;
                float f = std::min(bw / sw, bh / sh), dw = sw * f, dh = sh * f;
                float l = front.left + (bw - dw) / 2 + kStackOff * float(j), t = front.top + (bh - dh) / 2 - kStackOff * float(j);
                D2D1_RECT_F d = ScaleRect(D2D1::RectF(l, t, l + dw, t + dh), k, cx, cy);
                POINT tl = surface_.ContentToScreen(POINT{LONG(std::lround(d.left * s)), LONG(std::lround(d.top * s))});
                ScreenToClient(surface_.Hwnd(), &tl);
                DWM_THUMBNAIL_PROPERTIES p{};
                p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
                p.rcDestination = RECT{tl.x, tl.y, tl.x + LONG(std::lround((d.right - d.left) * s)), tl.y + LONG(std::lround((d.bottom - d.top) * s))};
                p.rcSource = src;
                const float alpha = thumbAlpha_ * e.appear * (j == 0 ? 1.f : 0.72f);
                p.fVisible = shown && alpha > 0.01f;
                p.opacity = BYTE(std::lround(255 * std::clamp(alpha, 0.f, 1.f)));
                p.fSourceClientAreaOnly = FALSE;
                // A call into DWM is only made for a change: at rest a frame costs it nothing.
                auto& last = e.shown;
                if (last.size() != e.thumbs.size())
                    last.assign(e.thumbs.size(), DWM_THUMBNAIL_PROPERTIES{});
                if (memcmp(&last[j], &p, sizeof(p)) == 0)
                    continue;
                last[j] = p;
                thumbs_.Update(e.thumbs[j], p);
            }
        }
}

void StageManager::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const int iconPx = int(std::lround(20 * c.scale));
    const bool moving = Animate(c.now);
    std::vector<ShapeDip> shapes;
    auto card = [&](Entry& e, bool live, int i) {
        const float k = 0.94f + 0.06f * e.appear, cx = (e.rect.left + e.rect.right) / 2, cy = (e.rect.top + e.rect.bottom) / 2;
        ShapeDip shape{ScaleRect(e.rect, k, cx, cy), kCardR * k};
        shape.fade = 1 - e.appear;
        shapes.push_back(shape);
        c.BeginGroup(e.appear, k, cx, cy);
        if (c.panel)
            c.Fill(e.rect, kCardR, t.panel);
        float hv = e.hover.Step(live && i == hover_, c.now);
        if (e.hover.moving)
            c.Continue();
        if (hv > 0.001f)
            c.Fill(e.rect, kCardR, Alpha(t.hover, hv));
        if (live && i == pressed_)
            c.Fill(e.rect, kCardR, t.press);
        IconImage icon = tasks_.Icon(e.keys.front(), iconPx);
        // Under the pictures: the app's icon, for a window DWM has no picture of.
        if (std::none_of(e.thumbs.begin(), e.thumbs.end(), [&](uint64_t id) { return thumbs_.SourceSize(id).cx > 0; })) {
            c.Fill(e.image, kCardR - kPad, t.card);
            float mx = (e.image.left + e.image.right) / 2, my = (e.image.top + e.image.bottom) / 2;
            if (!icon.empty())
                c.Image(L"stage|" + e.keys.front(), icon, D2D1::RectF(mx - 20, my - 20, mx + 20, my + 20), 0.9f);
        }
        // The title row: the apps' icons (three at most) and their names.
        float ty = e.image.bottom + 4, ry = ty + kTitleH / 2 - 1, x = e.rect.left + kPad + 2;
        for (size_t j = 0; j < e.keys.size() && j < 3; ++j) {
            IconImage img = j == 0 ? icon : tasks_.Icon(e.keys[j], iconPx);
            if (img.empty())
                continue;
            c.Image(L"stage|" + e.keys[j], img, D2D1::RectF(x, ry - 10, x + 20, ry + 10));
            x += 23;
        }
        x += 3;
        c.Text(e.name, D2D1::RectF(x, ty, e.rect.right - kPad - 2, ty + kTitleH - 2), 12.5f, t.text, DWRITE_FONT_WEIGHT_MEDIUM);
        c.EndGroup();
    };
    for (Entry& e : leaving_)
        card(e, false, -1);
    for (size_t i = 0; i < entries_.size(); ++i)
        card(entries_[i], true, int(i));
    surface_.SetShapes(shapes, moving);
    UpdateRegion(moving);
    UpdateThumbs();
    if (moving)
        c.Continue();
    else if (entries_.empty() && leaving_.empty())
        SetTimer(surface_.Hwnd(), kHideTimer, 1, nullptr); // the last card has gone: not from inside a paint
}

int StageManager::EntryAt(POINT pt) const
{
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const D2D1_RECT_F& r = entries_[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return int(i);
    }
    return -1;
}

bool StageManager::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    (void)result;
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_STAGE_THUMB:
        surface_.Invalidate(); // a picture can be placed now
        return true;
    case WM_TIMER:
        if (w == kWatchTimer) {
            Watch();
            return true;
        }
        if (w == kFadeTimer) {
            float k = float(std::clamp((ClockMs() - fadeStart_) / kFadeMs, 0.0, 1.0));
            thumbAlpha_ = motion::EaseOut(k);
            UpdateThumbs();
            if (k >= 1)
                KillTimer(surface_.Hwnd(), kFadeTimer);
            return true;
        }
        if (w == kHideTimer) {
            KillTimer(surface_.Hwnd(), kHideTimer);
            if (entries_.empty() && leaving_.empty())
                Hide();
            return true;
        }
        if (w == kRaiseTimer) {
            KillTimer(surface_.Hwnd(), kRaiseTimer);
            if (raise_ && IsWindow(raise_) && IsWindowVisible(raise_) && !IsIconic(raise_))
                SetWindowPos(raise_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            raise_ = nullptr;
            return true;
        }
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        inside_ = true;
        int i = EntryAt(pt);
        if (i != hover_) {
            hover_ = i;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        inside_ = false;
        hover_ = pressed_ = -1;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        pressed_ = EntryAt(pt);
        if (pressed_ >= 0)
            surface_.Ripple(float(pt.x) / surface_.Scale(), float(pt.y) / surface_.Scale());
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        int i = EntryAt(pt);
        bool same = i >= 0 && i == pressed_;
        pressed_ = -1;
        surface_.Invalidate();
        if (same) {
            Set keys = entries_[size_t(i)].keys;
            Log(L"stage: %s picked in the strip%s", keys.front().c_str(), (w & MK_SHIFT) ? L" (added)" : L"");
            Pick(keys, (w & MK_SHIFT) != 0);
        }
        return true;
    }
    }
    return false;
}

} // namespace atrium
