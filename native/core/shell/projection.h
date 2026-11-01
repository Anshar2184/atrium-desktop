// Casting for the control centre, after the Windows taskbar's cast panel
// (Win+K) and projection modes (Win+P): the wireless displays in range
// (Windows.Devices.Enumeration with ProjectionManager's selector),
// projecting to one (ProjectionManager.StartProjectingAsync; when Windows
// refuses, its own cast panel takes over) and the four projection modes
// (SetDisplayConfig topologies, as Win+P sets them).
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace atrium {

struct WirelessDisplay {
    std::wstring id, name;
};

// Watches for wireless displays while the cast page is open; posts `msg` to
// `hwnd` whenever the list changes (read it with Displays()).
class DisplayFinder {
public:
    DisplayFinder();
    ~DisplayFinder();
    void Start(HWND hwnd, UINT msg);
    void Stop();
    std::vector<WirelessDisplay> Displays() const;
    bool Searching() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

// Blocks (worker thread): projects the desktop to the display; false if Windows refused.
bool ProjectTo(const std::wstring& id);
bool StopProjecting();

enum class ProjectionMode { Unknown, Internal, Clone, Extend, External };
// The current topology and how many displays could take part (connected).
ProjectionMode CurrentProjectionMode(int* displays);
bool SetProjectionMode(ProjectionMode mode);

} // namespace atrium
