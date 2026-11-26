// Pure helper logic for Stage Manager edge-reveal geometry and desktop-click vs. drag stowing.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace atrium {

struct StageEdgeResult {
    bool atEdge = false;
    ULONGLONG dwellMs = 40;
};

inline StageEdgeResult EvaluateStageEdge(RECT monitorRect, RECT workRect, POINT cursor, bool onRight, bool adjacentAtPointerY, float scale, bool dragging,
                                         bool hasDragWindow, ULONGLONG outerDwellMs = 40, ULONGLONG seamDwellMs = 100, float seamDip = 14.0f)
{
    if ((dragging && !hasDragWindow) || !PtInRect(&monitorRect, cursor) || cursor.y >= workRect.bottom)
        return {false, outerDwellMs};
    if (adjacentAtPointerY) {
        const int bandPx = std::max(2, static_cast<int>(std::lround(seamDip * scale)));
        const bool hit = onRight ? (cursor.x >= monitorRect.right - bandPx) : (cursor.x < monitorRect.left + bandPx);
        return {hit, seamDwellMs};
    }
    const bool hit = onRight ? (cursor.x >= monitorRect.right - 2) : (cursor.x <= monitorRect.left + 1);
    return {hit, outerDwellMs};
}

struct StageDesktopStowState {
    ULONGLONG desktopSince = 0;
    bool onDesktop = false;
    bool buttonDown = false;
    bool dragMoved = false;
    bool suppressUntilNewClick = false;
    POINT pressPos{};
};

inline bool StepStageDesktopStow(StageDesktopStowState& state, bool atDesktop, bool buttonDownNow, POINT cursorPos, int dragThresholdX, int dragThresholdY,
                                 ULONGLONG now, ULONGLONG desktopMs)
{
    if (buttonDownNow && !state.buttonDown) {
        state.pressPos = cursorPos;
        state.dragMoved = false;
    } else if (buttonDownNow) {
        if (std::abs(cursorPos.x - state.pressPos.x) > dragThresholdX || std::abs(cursorPos.y - state.pressPos.y) > dragThresholdY)
            state.dragMoved = true;
    }
    const bool released = state.buttonDown && !buttonDownNow;
    state.buttonDown = buttonDownNow;

    if (!atDesktop) {
        state.desktopSince = 0;
        state.onDesktop = false;
        state.suppressUntilNewClick = false;
        return false;
    }
    if (buttonDownNow) {
        state.desktopSince = 0;
        if (state.dragMoved)
            state.suppressUntilNewClick = true;
        return false;
    }
    if (released) {
        if (state.dragMoved) {
            state.suppressUntilNewClick = true;
            state.desktopSince = 0;
            return false;
        }
        state.suppressUntilNewClick = false;
        state.onDesktop = false;
        state.desktopSince = now;
    }
    if (state.suppressUntilNewClick) {
        state.desktopSince = 0;
        return false;
    }
    if (state.desktopSince == 0)
        state.desktopSince = now;
    const bool desktop = now - state.desktopSince >= desktopMs;
    const bool trigger = desktop && !state.onDesktop;
    state.onDesktop = desktop;
    return trigger;
}

} // namespace atrium
