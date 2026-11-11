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

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

namespace {

using glasswave::gfx::PaintCoalescer;
using glasswave::gfx::SurfaceReusePolicy;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool SameSizeReusesSurface() {
    SurfaceReusePolicy policy;
    uint64_t creates = 0, resizes = 0, draws = 0;
    const auto paint = [&](bool hasSurface, int width, int height) {
        switch (policy.Plan(hasSurface, width, height)) {
        case SurfaceReusePolicy::Update::Create:
            ++creates;
            break;
        case SurfaceReusePolicy::Update::Resize:
            ++resizes;
            break;
        case SurfaceReusePolicy::Update::Reuse:
            break;
        }
        ++draws;
    };

    paint(false, 360, 50);
    for (int i = 0; i < 100; ++i)
        paint(true, 360, 50);
    return Check(creates == 1, "same-size updates recreated the surface") &&
           Check(resizes == 0, "same-size updates resized the surface") &&
           Check(draws == 101, "content was not redrawn");
}

bool SizeAndScaleChangesRemainBounded() {
    SurfaceReusePolicy policy;
    uint64_t creates = 0, resizes = 0;
    const auto plan = [&](int width, int height) {
        const bool hasSurface = policy.WidthPx() != 0;
        switch (policy.Plan(hasSurface, width, height)) {
        case SurfaceReusePolicy::Update::Create:
            ++creates;
            break;
        case SurfaceReusePolicy::Update::Resize:
            ++resizes;
            break;
        case SurfaceReusePolicy::Update::Reuse:
            break;
        }
    };

    plan(360, 50);  // create at 100% DPI
    plan(360, 50);  // redraw after a DPI-only change
    plan(720, 100); // 200% DPI changes the backing size
    plan(720, 100); // content-only redraw at the new size
    return Check(creates == 1, "resize replaced the drawing surface") &&
           Check(resizes == 1, "size change was not applied once");
}

bool HiddenRequestsCoalesce() {
    PaintCoalescer paint;
    uint64_t draws = 0;
    for (int i = 0; i < 100; ++i) {
        paint.Request();
        if (paint.Take(false)) ++draws;
    }
    if (!Check(draws == 0, "hidden updates painted")) return false;
    if (!Check(paint.Pending(), "hidden request was discarded instead of coalesced")) return false;
    if (!Check(paint.Take(true), "show did not release the coalesced paint")) return false;
    ++draws;
    return Check(!paint.Take(true), "coalesced request painted more than once") &&
           Check(draws == 1, "recovery paint count was wrong");
}

bool CleanupDropsCachedIdentity() {
    SurfaceReusePolicy policy;
    policy.Plan(false, 1280, 40);
    policy.Clear();
    const bool reset = policy.WidthPx() == 0 && policy.HeightPx() == 0;
    const auto next = policy.Plan(false, 1280, 40);
    return Check(reset, "cleanup retained the old surface dimensions") &&
           Check(next == SurfaceReusePolicy::Update::Create, "cleanup retained a live surface identity");
}

} // namespace

int main() {
    const bool reuse = SameSizeReusesSurface();
    const bool resize = SizeAndScaleChangesRemainBounded();
    const bool hidden = HiddenRequestsCoalesce();
    const bool cleanup = CleanupDropsCachedIdentity();
    std::printf("surfaceReuse=%s sizeAndDpi=%s hiddenCoalesce=%s cleanup=%s\n", reuse ? "pass" : "fail",
                resize ? "pass" : "fail", hidden ? "pass" : "fail", cleanup ? "pass" : "fail");
    return reuse && resize && hidden && cleanup ? 0 : 1;
}
