// Atrium's context menus (D41): a short list on a piece of the bar's glass,
// in place of the system's menus. One at a time; it pops out of the point it
// was opened at, closes on a pick, Esc or a click elsewhere, and reports the
// pick afterwards. Rows: text, an optional icon or check mark, disabled
// rows, a header (an app's name) and separators.
#pragma once

#include "motion.h"
#include "ui/surface.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace atrium {

struct MenuEntry {
    std::wstring text; // empty: a separator
    int id = 0;
    bool checked = false;
    bool disabled = false;
    bool header = false; // a title row (not a choice)
    IconImage icon;
};

// Where the menu goes relative to the point it opens at (screen px): below
// and right of it (a right click on a widget or in a panel), above it (on the
// bar), or centred above it (above an entry of the bar). It stays on the work area.
enum class MenuPlace { Below, Above, AboveCentered };

class GlassMenu {
public:
    explicit GlassMenu(HINSTANCE instance);

    // onPick runs after the menu closed, with the chosen entry's id; onClosed
    // runs when it closes either way (after onPick).
    void Open(std::vector<MenuEntry> entries, POINT at, MenuPlace place, std::function<void(int)> onPick,
              std::function<void()> onClosed = nullptr);
    void Close();
    bool Visible() const { return surface_.Visible(); }
    HWND Hwnd() const { return surface_.Hwnd(); }

private:
    void Paint(Canvas& c);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    int RowAt(POINT client) const; // entry index, -1 for none
    void Move(int step);           // keyboard selection
    void Pick(int index);
    D2D1_SIZE_F Size(float scale) const;

    Surface surface_;
    std::vector<MenuEntry> entries_;
    std::vector<D2D1_RECT_F> rows_; // DIP, by entry
    std::function<void(int)> onPick_;
    std::function<void()> onClosed_;
    int hover_ = -1, pressed_ = -1;
    float scroll_ = 0, contentH_ = 0, viewH_ = 0; // DIP: a menu taller than the work area scrolls
    bool tracking_ = false, marks_ = false;
    std::map<int, motion::Follow> hoverAnim_;
};

} // namespace atrium
