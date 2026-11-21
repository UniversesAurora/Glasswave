#include "pch.h"
#include "gfx/motion.h"
#include "gfx/composition_host.h"

namespace glasswave::gfx {

namespace wuc = winrt::Windows::UI::Composition;

float Ease(const Curve& c, float t) {
    // x(u) = t solved by halving, then y(u).
    const auto bez = [](float a, float b, float u) {
        return 3 * a * u * (1 - u) * (1 - u) + 3 * b * u * u * (1 - u) + u * u * u;
    };
    float lo = 0, hi = 1, u = t;
    for (int i = 0; i < 24; ++i) {
        u = (lo + hi) / 2;
        (bez(c.x1, c.x2, u) < t ? lo : hi) = u;
    }
    return bez(c.y1, c.y2, u);
}

wuc::CubicBezierEasingFunction Easing(const Curve& c) { return CompositionHost::Get().Easing(c); }

void FadeTo(const wuc::Visual& v, float to, int ms, const Curve& c) {
    auto fade = v.Compositor().CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, to, Easing(c));
    fade.Duration(std::chrono::milliseconds(ms));
    v.StartAnimation(L"Opacity", fade);
}

void FadeIn(const wuc::Visual& v, int ms, int delayMs, const Curve* c) {
    auto fade = v.Compositor().CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    if (c)
        fade.InsertKeyFrame(1.0f, 1.0f, Easing(*c));
    else
        fade.InsertKeyFrame(1.0f, 1.0f);
    fade.Duration(std::chrono::milliseconds(ms));
    fade.DelayTime(std::chrono::milliseconds(delayMs));
    fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    v.StartAnimation(L"Opacity", fade);
}

} // namespace glasswave::gfx
