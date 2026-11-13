// What each desktop widget draws, after macOS's widgets: clear glass with its
// ink and a few of macOS's colours. desktop_widgets owns the windows, the grid,
// the input and the saved layout.
#pragma once

#include "shell/bluetooth_batteries.h"
#include "shell/lunar.h"
#include "shell/network_monitor.h"
#include "shell/system_status.h"
#include "shell/weather.h"
#include "shell/task_model.h"
#include "ui/surface.h"

#include <memory>
#include <string>
#include <vector>

namespace atrium {

// FileBox is appended last: main's WM_ATRIUM_ADD_WIDGET casts the settings app's number straight
// to a WidgetKind, so the order of this list is the settings app's order.
enum class WidgetKind { Clock, Calendar, Battery, Digital, System, Notes, Reminders, Photos, Weather, Bluetooth, Network, FileBox };
enum class WidgetSize { Small, Medium, Large };
// Glass: the bar's clear glass, its ink adapting to what shows through. Dusk: the same glass
// dimmed by a dark veil, everything on it white (macOS's dimmed desktop widgets). Color: each
// widget's own opaque face, white or (weather) its sky, as macOS's full-colour widgets.
enum class WidgetMaterial { Glass, Dusk, Color };

// macOS's grid: a small widget is a square, a medium one two squares wide, a
// large one two by two (DIP).
constexpr float kWidgetCell = 164, kWidgetGap = 16, kWidgetRadius = 22;
inline int WidgetCols(WidgetSize s) { return s == WidgetSize::Small ? 1 : 2; }
inline int WidgetRows(WidgetSize s) { return s == WidgetSize::Large ? 2 : 1; }
inline float WidgetWidth(WidgetSize s) { return float(WidgetCols(s)) * kWidgetCell + float(WidgetCols(s) - 1) * kWidgetGap; }
inline float WidgetHeight(WidgetSize s) { return float(WidgetRows(s)) * kWidgetCell + float(WidgetRows(s) - 1) * kWidgetGap; }
bool WidgetSupports(WidgetKind kind, WidgetSize size);

// The cities a world clock can show, each with its Windows time zone.
struct WorldCity {
    const wchar_t* name;
    const wchar_t* zone;
};
const std::vector<WorldCity>& WorldCities();
// The world clocks of a widget (the medium Clock shows four, the medium Digital three): its own
// choice where it made one, else the defaults.
bool HasWorldClocks(WidgetKind kind, WidgetSize size);
std::vector<std::wstring> WidgetCities(WidgetKind kind, const std::vector<std::wstring>& chosen);

struct Reminder {
    std::wstring text;
    bool done = false;
};

// A file box shows what one folder holds: a file or a subfolder of it, its icon read on a worker
// thread and cached under `iconKey` in the canvas.
struct FileEntry {
    std::wstring name; // its name, without the folder
    std::wstring path; // where it is (what a click opens)
    bool folder = false;
    std::shared_ptr<const IconImage> icon;
    std::wstring iconKey;
};

// A widget's own content and its editing state.
struct WidgetContent {
    std::wstring text;           // notes
    std::vector<Reminder> items; // reminders
    std::wstring folder;         // photos: the folder (empty: Pictures)
    std::vector<std::wstring> cities; // world clocks: their cities (WorldCities' names; empty: the defaults)
    std::wstring photo;          // the photo shown
    std::shared_ptr<const IconImage> image; // it, cropped and scaled to the widget
    std::wstring imageKey;       // the canvas's cache key of `image`
    // File box: the folder it shows (empty: the desktop), what that folder holds now, the extensions
    // it keeps on its own when `autoFile` is on, and where its grid is scrolled to.
    std::vector<FileEntry> files;
    std::vector<std::wstring> rules;
    bool autoFile = false;
    float scrollRow = 0; // not saved
    bool dropHover = false; // files are being dragged over it: not saved
    bool editing = false;
    int item = -1;     // reminders: the item being edited
    size_t caret = 0;  // in the text being edited
    bool caretOn = true; // blinking
    float scroll = 0;   // reminders: viewport offset, not saved
};

// What every widget may show.
struct WidgetWorld {
    SYSTEMTIME now{};
    std::wstring city; // the local time zone's first city
    const ChineseCalendar* lunar = nullptr;
    SystemStatus status;
    float cpu = 0, memory = 0, disk = 0; // in use, 0..1
    double memoryGb = 0, memoryTotalGb = 0, diskFreeGb = 0;
    Weather weather; // the forecast (weather.ok false: none yet)
    std::vector<BluetoothBattery> bluetooth; // the connected devices' batteries
    bool bluetoothRead = false;              // false until the first reading
    const NetworkMonitor* network = nullptr; // the link's rates and their minute of history
};

// Places in a widget that respond to the pointer (content DIP): "toggle:<i>"
// and "item:<i>" (a reminder's circle and text), "add" (a new reminder),
// "text" (the note). "caret" is where the caret is (the input method's window).
struct WidgetHit {
    D2D1_RECT_F rect;
    std::wstring id;
};

constexpr float kReminderTop = 42, kReminderRow = 27, kReminderBottom = 14;
float ReminderScrollMax(const WidgetContent& content, WidgetSize size);

// A file box's grid: icons across and down for a size (a small box holds four, a medium eight, a
// large sixteen).
int FileBoxCols(WidgetSize size);
int FileBoxRows(WidgetSize size);
inline int FileBoxCapacity(WidgetSize size) { return FileBoxCols(size) * FileBoxRows(size); }

// Paints the content over the widget's glass (the face and the ink of `material` included).
void PaintWidget(Canvas& c, WidgetKind kind, WidgetSize size, WidgetMaterial material, const WidgetContent& content, const WidgetWorld& world, std::vector<WidgetHit>& hits);

} // namespace atrium
