#include "displays.h"

#include <shellapi.h>

#include <algorithm>

namespace atrium {

std::vector<DisplayInfo> ConnectedDisplays()
{
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return TRUE;
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        std::wstring id = info.szDevice;
        if (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) && device.DeviceID[0])
            id = device.DeviceID;
        auto& list = *reinterpret_cast<std::vector<DisplayInfo>*>(data);
        list.push_back({monitor, std::move(id), info.rcMonitor, info.rcWork, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

HMONITOR ResolveDisplay(const std::vector<DisplayInfo>& displays, const std::wstring& id)
{
    auto selected = std::find_if(displays.begin(), displays.end(), [&](const DisplayInfo& d) { return !id.empty() && d.id == id; });
    if (selected != displays.end())
        return selected->monitor;
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

bool FilterTasksOnDisplay(const std::wstring& mode, bool primary)
{
    return mode == L"window" || (mode == L"primaryAndWindow" && !primary);
}

std::optional<bool> WindowCoverOnMonitor(HMONITOR windowMonitor, const RECT& frame, bool maximized,
    HMONITOR monitor, const RECT& bounds)
{
    if (windowMonitor != monitor || IsRectEmpty(&frame))
        return std::nullopt;
    return maximized || (frame.left <= bounds.left && frame.top <= bounds.top && frame.right >= bounds.right && frame.bottom >= bounds.bottom);
}

bool IsTile(const RECT& frame, const RECT& work, int slack)
{
    if (IsRectEmpty(&frame) || frame.left < work.left - slack || frame.top < work.top - slack ||
        frame.right > work.right + slack || frame.bottom > work.bottom + slack)
        return false;
    const int edges = (frame.left <= work.left + slack) + (frame.top <= work.top + slack) +
                      (frame.right >= work.right - slack) + (frame.bottom >= work.bottom - slack);
    return edges >= 2;
}

bool TilesFill(const std::vector<RECT>& tiles, const RECT& work, int slack)
{
    if (tiles.empty() || IsRectEmpty(&work))
        return false;
    // The work area cut along every tile edge: each cell is inside a tile or not at all.
    std::vector<RECT> grown;
    std::vector<LONG> xs{work.left, work.right}, ys{work.top, work.bottom};
    for (RECT t : tiles) {
        InflateRect(&t, slack, slack);
        grown.push_back(t);
        for (LONG x : {t.left, t.right})
            if (x > work.left && x < work.right)
                xs.push_back(x);
        for (LONG y : {t.top, t.bottom})
            if (y > work.top && y < work.bottom)
                ys.push_back(y);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    for (size_t i = 0; i + 1 < xs.size(); ++i)
        for (size_t j = 0; j + 1 < ys.size(); ++j) {
            if (xs[i] == xs[i + 1] || ys[j] == ys[j + 1])
                continue;
            const POINT mid{(xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2};
            if (std::none_of(grown.begin(), grown.end(), [&](const RECT& t) { return PtInRect(&t, mid) != FALSE; }))
                return false;
        }
    return true;
}

bool AtDisplayBottom(POINT pointer, const RECT& bounds)
{
    return PtInRect(&bounds, pointer) && pointer.y >= bounds.bottom - 2;
}

bool FullScreenInFront(HMONITOR monitor)
{
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    // No foreground for a moment (it is changing hands): the top app window on this screen.
    for (HWND w = root ? nullptr : GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        DWORD owner = 0;
        GetWindowThreadProcessId(w, &owner);
        if (IsWindowVisible(w) && !IsIconic(w) && owner != GetCurrentProcessId() && !(ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) &&
            !GetWindow(w, GW_OWNER) && MonitorFromWindow(w, MONITOR_DEFAULTTONULL) == monitor) {
            root = w;
            break;
        }
    }
    DWORD pid = 0;
    if (!root || !GetWindowThreadProcessId(root, &pid) || pid == GetCurrentProcessId() ||
        MonitorFromWindow(root, MONITOR_DEFAULTTONULL) != monitor)
        return false;
    wchar_t cls[64] = {};
    GetClassNameW(root, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN)
        return true;
    // Maximized fills only the work area (all of it when Windows' taskbar is hidden).
    MONITORINFO mi{sizeof(mi)};
    RECT r{};
    if (IsZoomed(root) || IsIconic(root) || !GetMonitorInfoW(monitor, &mi) || !GetWindowRect(root, &r))
        return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

std::vector<TaskItem> TaskItemsForDisplay(const std::vector<TaskItem>& items, const std::wstring& mode, bool primary,
    HMONITOR monitor, HWND foreground, const std::function<HMONITOR(HWND)>& monitorOf)
{
    auto result = items;
    if (!FilterTasksOnDisplay(mode, primary))
        return result;
    // Keep pins, but restrict running dots, previews and active state to this display.
    for (TaskItem& item : result) {
        std::erase_if(item.windows, [&](HWND window) { return monitorOf(window) != monitor; });
        item.active = std::find(item.windows.begin(), item.windows.end(), foreground) != item.windows.end();
        if (item.windows.empty())
            item.attention = false;
    }
    std::erase_if(result, [](const TaskItem& item) { return !item.pinned && item.windows.empty(); });
    return result;
}

} // namespace atrium
