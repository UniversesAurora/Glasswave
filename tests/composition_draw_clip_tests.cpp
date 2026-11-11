#include <Windows.h>

#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/base.h>

#include "gfx/composition_host.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

using glasswave::gfx::SurfaceDrawClip;
using glasswave::gfx::detail::ScopeExit;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool Same(float a, float b) {
    return std::abs(a - b) < 0.001f;
}

bool DrawClipTracksOffsetAndScale() {
    const auto clip = SurfaceDrawClip(POINT{300, 120}, 360, 50, 1.5f);
    return Check(Same(clip.left, 200.0f), "clip left ignored update offset") &&
           Check(Same(clip.top, 80.0f), "clip top ignored update offset") &&
           Check(Same(clip.right, 440.0f), "clip right ignored logical width") &&
           Check(Same(clip.bottom, 113.333f), "clip bottom ignored logical height");
}

bool DrawClipKeepsIdentityAtOneHundredPercent() {
    const auto clip = SurfaceDrawClip(POINT{0, 0}, 1280, 40, 1.0f);
    return Check(Same(clip.left, 0.0f) && Same(clip.top, 0.0f) && Same(clip.right, 1280.0f) &&
                     Same(clip.bottom, 40.0f),
                 "100 percent logical clip bounds were not exact");
}

bool CleanupRunsInReverseOrder() {
    std::vector<int> order;
    bool clipActive = true, drawActive = true, targetActive = true, surfaceActive = true;
    try {
        {
            auto endSurface = ScopeExit([&] {
                order.push_back(4);
                surfaceActive = false;
            });
            auto unbindTarget = ScopeExit([&] {
                order.push_back(3);
                targetActive = false;
            });
            auto endDraw = ScopeExit([&] {
                order.push_back(2);
                drawActive = false;
            });
            auto popClip = ScopeExit([&] {
                order.push_back(1);
                clipActive = false;
            });
            throw std::runtime_error("paint");
        }
    } catch (const std::runtime_error&) {
    }

    const std::vector<int> expected{1, 2, 3, 4};
    return Check(order == expected, "drawing cleanup order was not clip/draw/target/surface") &&
           Check(!clipActive && !drawActive && !targetActive && !surfaceActive,
                 "drawing cleanup left an active resource");
}

bool DismissedCleanupDoesNotRun() {
    bool ran = false;
    {
        auto cleanup = ScopeExit([&] { ran = true; });
        cleanup.Dismiss();
    }
    return Check(!ran, "dismissed cleanup still ran");
}

}  // namespace

int main() {
    const bool offset = DrawClipTracksOffsetAndScale();
    const bool identity = DrawClipKeepsIdentityAtOneHundredPercent();
    const bool unwind = CleanupRunsInReverseOrder();
    const bool dismiss = DismissedCleanupDoesNotRun();
    std::printf("offsetScale=%s identity=%s unwind=%s dismiss=%s\n", offset ? "pass" : "fail",
                identity ? "pass" : "fail", unwind ? "pass" : "fail", dismiss ? "pass" : "fail");
    return offset && identity && unwind && dismiss ? 0 : 1;
}
