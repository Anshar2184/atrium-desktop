// Atrium's own notifications: cards in a window above the bell on the bar.
// New notices pop up without taking the focus (collapsed stack: the newest
// card in full, the edges of older ones behind it) and time out; the bell
// opens the list with unread marks, dismiss and clear. Senders are internal
// (tests, Atrium itself) or, experimentally, the Windows notifications read
// through UserNotificationListener.
#pragma once

#include "ui/surface.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

struct Notice {
    uint64_t id = 0;
    std::wstring source;     // "test", "atrium", "windows"
    std::wstring app;        // sender's display name
    std::wstring appId;      // sender's AUMID, used to activate it
    std::wstring title, body;
    std::wstring action;     // "uri:<target>", "app:<aumid>" or empty
    uint32_t externalId = 0; // the listener's id (de-duplication)
    ULONGLONG time = 0;      // GetTickCount64 of the latest update
    int count = 1;           // merged notices
    bool unread = true;
};

class Notifications {
public:
    explicit Notifications(HINSTANCE instance);

    std::function<RECT()> bellRect; // screen px
    std::function<RECT()> barRect;
    std::function<void(int unread)> onUnread;
    std::function<bool(const Notice&)> onActivate; // runs the notice's action

    void Post(Notice n);
    void ToggleCenter();
    void Clear();
    void SetOptions(double timeoutSeconds, bool hideBody);
    // Corner radius (DIP), the bar's; the cards are concentric with it.
    void SetRadius(float dip);
    int Unread() const;
    // Tests: pretend SHQueryUserNotificationState returned `state` (0: use the real one).
    void SetQuietOverride(int state) { quietOverride_ = state; }
    void LogState() const;

private:
    enum class Mode { Popup, Center };
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    // A card's displayed place; it eases towards where the layout wants it,
    // so arrivals, expanding, collapsing and closing all glide.
    struct CardAnim {
        float y = 0, inset = 0, alpha = 0;
        bool placed = false;
    };
    struct View {
        std::unique_ptr<Surface> surface;
        std::vector<Hit> hits;
        std::wstring hover, pressed;
        bool tracking = false;
        float scroll = 0, contentH = 0, viewH = 0;
        std::map<uint64_t, CardAnim> cards;
        std::map<std::wstring, float> glow; // eased hover per element
        double lastPaint = 0;
        std::wstring rippleId;
        float rippleX = 0, rippleY = 0;
        double rippleMs = 0;
    };

    void Paint(Mode mode, Canvas& c);
    void PaintCard(Canvas& c, const Notice& n, D2D1_RECT_F r, int index, View& v, bool full, float alpha);
    float Ease(View& v, Canvas& c, const std::wstring& id, bool on, float dt);
    void Sweep(); // drops the cards whose closing animation has finished
    bool Message(Mode mode, UINT m, WPARAM w, LPARAM l, LRESULT& result);
    void Click(Mode mode, const std::wstring& id);
    void Activate(uint64_t id);
    void Remove(uint64_t id);
    void Changed();       // unread count, layout of visible windows
    void Layout(Mode mode);
    float CardHeight() const;
    std::vector<const Notice*> Shown(Mode mode) const;
    int QuietState() const;
    bool BodyHidden() const;
    void HidePopup(const wchar_t* why);
    View& ViewOf(Mode mode) { return mode == Mode::Popup ? popup_ : center_; }
    const View& ViewOf(Mode mode) const { return mode == Mode::Popup ? popup_ : center_; }

    View popup_, center_;
    std::vector<Notice> notices_;  // newest first
    std::vector<uint64_t> popupIds_; // shown by the popup, newest first
    bool expanded_ = false;
    uint64_t nextId_ = 1;
    double timeout_ = 6;
    bool hideBody_ = false;
    int quietOverride_ = 0;
    ULONGLONG centerClosed_ = 0;
    float radius_ = 20;
    std::map<uint64_t, double> leaving_; // closing cards: ClockMs() when closed
};

} // namespace atrium
