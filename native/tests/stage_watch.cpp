#include "ui/stage_watch.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Require(bool ok, const char* message)
{
    if (ok)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void TestOuterEdgeVsInteriorSeamReveal()
{
    const RECT primary{1920, 0, 3840, 1080};
    const RECT primaryWork{1920, 0, 3840, 1032};

    const atrium::StageEdgeResult outerMiss =
        atrium::EvaluateStageEdge(primary, primaryWork, POINT{1925, 400}, false, false, 1.0f, false, false, 40, 100, 14.0f);
    Require(!outerMiss.atEdge, "outer left edge stays tight at 2px");

    const atrium::StageEdgeResult outerHit =
        atrium::EvaluateStageEdge(primary, primaryWork, POINT{1921, 400}, false, false, 1.0f, false, false, 40, 100, 14.0f);
    Require(outerHit.atEdge && outerHit.dwellMs == 40, "outer left edge triggers within 2px at 40ms dwell");

    const atrium::StageEdgeResult seamHit = atrium::EvaluateStageEdge(primary, primaryWork, POINT{1932, 400}, false, true, 1.0f, false, false, 40, 100, 14.0f);
    Require(seamHit.atEdge && seamHit.dwellMs == 100, "interior seam widens left trigger band and uses 100ms dwell");

    const atrium::StageEdgeResult seamRightHit =
        atrium::EvaluateStageEdge(primary, primaryWork, POINT{3825, 400}, true, true, 1.25f, false, false, 40, 100, 14.0f);
    Require(seamRightHit.atEdge && seamRightHit.dwellMs == 100, "scaled interior right seam widens proportionally");

    const atrium::StageEdgeResult staggeredTight =
        atrium::EvaluateStageEdge(primary, primaryWork, POINT{1932, 950}, false, false, 1.0f, false, false, 40, 100, 14.0f);
    Require(!staggeredTight.atEdge, "non-adjacent height on staggered dual display stays tight");

    const atrium::StageEdgeResult belowWork =
        atrium::EvaluateStageEdge(primary, primaryWork, POINT{1921, 1050}, false, true, 1.0f, false, false, 40, 100, 14.0f);
    Require(!belowWork.atEdge, "cursor below work area does not trigger Stage reveal");

    const atrium::StageEdgeResult fileDragBlocked =
        atrium::EvaluateStageEdge(primary, primaryWork, POINT{1921, 400}, false, false, 1.0f, true, false, 40, 100, 14.0f);
    Require(!fileDragBlocked.atEdge, "button drag without dragWindow does not auto-reveal edge");
}

void TestDesktopDragDoesNotStowAndSelfResets()
{
    atrium::StageDesktopStowState state{};
    const POINT start{500, 500};
    const POINT dragged{540, 500};

    Require(!atrium::StepStageDesktopStow(state, true, true, start, 4, 4, 100, 250), "press on desktop file must not stow");
    Require(!atrium::StepStageDesktopStow(state, true, true, dragged, 4, 4, 450, 250), "dragging desktop file past 250ms must not stow");
    Require(!atrium::StepStageDesktopStow(state, true, false, dragged, 4, 4, 480, 250), "releasing drag on desktop must not stow");
    Require(!atrium::StepStageDesktopStow(state, true, false, dragged, 4, 4, 900, 250), "remaining on desktop after drag drop/cancel must stay suppressed");

    // Next independent plain click on desktop must reset suppression cleanly and stow after 250ms.
    Require(!atrium::StepStageDesktopStow(state, true, true, dragged, 4, 4, 1000, 250), "plain click press starts fresh");
    Require(!atrium::StepStageDesktopStow(state, true, false, dragged, 4, 4, 1050, 250), "plain click release clears prior drag suppression");
    Require(!atrium::StepStageDesktopStow(state, true, false, dragged, 4, 4, 1200, 250), "plain click waits full 250ms before stowing");
    Require(atrium::StepStageDesktopStow(state, true, false, dragged, 4, 4, 1310, 250), "next plain click stows after 250ms");
    Require(!atrium::StepStageDesktopStow(state, true, false, dragged, 4, 4, 1500, 250), "same desktop stay fires stow only once");

    // Window minimize dropping foreground onto desktop without mouse click still stows after 250ms.
    atrium::StageDesktopStowState minState{};
    Require(!atrium::StepStageDesktopStow(minState, true, false, start, 4, 4, 2000, 250), "no-click desktop entry waits 250ms");
    Require(atrium::StepStageDesktopStow(minState, true, false, start, 4, 4, 2260, 250), "no-click desktop entry stows after 250ms");
}

} // namespace

int main()
{
    TestOuterEdgeVsInteriorSeamReveal();
    TestDesktopDragDoesNotStowAndSelfResets();
    if (g_failures > 0)
        return 1;
    std::printf("stage watch: all checks passed\n");
    return 0;
}
