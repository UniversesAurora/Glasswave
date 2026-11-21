#pragma once

#include <compare>

// Motion shared by every animated visual: the curves Glasswave moves along (on the compositor
// and, for per-frame views, on the CPU) and the fades built on them.

namespace glasswave::gfx {

// A unit cubic-bezier curve (CSS cubic-bezier(x1, y1, x2, y2)).
struct Curve {
    float x1, y1, x2, y2;
    auto operator<=>(const Curve&) const = default;
};

namespace curve {
inline constexpr Curve kEase{0.25f, 0.1f, 0.25f, 1.0f};   // CSS "ease": a control's own fades
inline constexpr Curve kFastOut{0.1f, 0.9f, 0.2f, 1.0f};  // Windows 8.1's fast-out: things arriving
inline constexpr Curve kPlace{0.16f, 0.9f, 0.2f, 1.0f};   // the player window's layout moving
inline constexpr Curve kSlideIn{0.16f, 1.0f, 0.3f, 1.0f}; // panes and bars sliding in
inline constexpr Curve kExit{0.4f, 0.0f, 0.2f, 1.0f};     // things fading away
inline constexpr Curve kStandard{0.4f, 0.0f, 0.6f, 1.0f}; // symmetric: a slide out, a rise
inline constexpr Curve kInOut{0.45f, 0.0f, 0.55f, 1.0f};  // pulses
} // namespace curve

// The curve's value at `t` (0-1), for views that animate frame by frame.
float Ease(const Curve& c, float t);

// The compositor's easing function for `c`, made once.
winrt::Windows::UI::Composition::CubicBezierEasingFunction Easing(const Curve& c);

// A visual's opacity, from where it is to `to`.
void FadeTo(const winrt::Windows::UI::Composition::Visual& v, float to, int ms,
            const Curve& c = curve::kEase);
// A visual appearing: transparent until `delayMs` have passed, then opaque over `ms`, along `c`
// or, without one, at an even pace.
void FadeIn(const winrt::Windows::UI::Composition::Visual& v, int ms, int delayMs, const Curve* c = nullptr);

} // namespace glasswave::gfx
