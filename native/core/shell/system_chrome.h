// The parts of Windows' own shell that Atrium can stand in for, hidden on the
// user's request (both settings are off by default): the system taskbar
// (Shell_TrayWnd and the secondary ones are hidden while Atrium runs, and set
// to auto-hide so they leave no gap) and the system's notification banners
// (Do Not Disturb through the shell's QuietHoursSettings service: the
// notifications still reach the notification centre, where the listener reads
// them for Atrium's cards). Everything is put back when the setting is turned
// off or Atrium exits; what was changed is kept in system.json, so a run that
// did not end cleanly is undone by the next start.
#pragma once

#include <windows.h>

#include <string>

namespace atrium {

class SystemChrome {
public:
    SystemChrome();
    ~SystemChrome(); // puts everything back

    void SetTaskbarHidden(bool hidden);
    void SetBannersHidden(bool hidden);
    // Hides the taskbars again if Explorer has shown them (a restart of
    // Explorer, a display change); follows the user turning Do Not Disturb off.
    void Enforce();
    // Do Not Disturb is on because Atrium turned it on (Atrium's own cards
    // still pop up then).
    bool QuietIsOurs() const { return quietOurs_; }
    void LogState() const;
    // From a crash handler: shows the taskbars (no allocation, no COM).
    static void EmergencyRestore();

private:
    void HideTaskbars();
    void Watch();
    void Save() const;
    static void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    bool taskbarHidden_ = false;
    bool autoHideOurs_ = false; // Atrium switched the taskbar to auto-hide
    bool quietOurs_ = false;
    HWINEVENTHOOK hook_ = nullptr;
    DWORD hookedPid_ = 0;
};

// Do Not Disturb: the user's selected profile, e.g.
// "Microsoft.QuietHoursProfile.Unrestricted"; empty if the service is unavailable.
std::wstring QuietProfile();
bool SetQuietProfile(const wchar_t* profile);

} // namespace atrium
