#pragma once
// Windows 7 DWM glass colorization model (pure math, platform-independent, unit-testable).
//
// Win7's DWM blurs what is behind a window and mixes the glass color from three
// "balance" terms:
//
//   out = colorization.rgb * colorBalance                     (base: flat tint)
//       + luma(backdrop)   * afterglow.rgb * afterglowBalance  (afterglow: tint that follows brightness)
//       + backdrop.rgb     * blurBalance                      (the blurred backdrop itself)
//
// The balances are ColorizationColorBalance / ColorizationAfterglowBalance /
// ColorizationBlurBalance (0..100) under HKCU\Software\Microsoft\Windows\DWM.
// The recipe is linear, so it folds into one 5x4 color matrix that the
// Windows.UI.Composition ColorMatrix effect evaluates on the GPU every frame.
//
// Verified: with the Win7 "Sky" defaults over a white backdrop the formula yields
// #b8dbfd, exactly the glass color measured on a native Win7 screenshot.
//
// Reference: community analysis of the Win7 DWM shader (OpenGlass / DWMBlurGlass).
// Only the published math is used here; no code from those projects.

#include <array>
#include <cstdint>

namespace glasswave::aero {

struct Rgb {
    float r = 0, g = 0, b = 0;
};

constexpr Rgb RgbFromArgb(uint32_t argb) {
    return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f};
}

struct Colorization {
    uint32_t color = 0;             // ColorizationColor (ARGB; alpha is not used for mixing)
    uint32_t afterglow = 0;         // ColorizationAfterglow (ARGB)
    float colorBalance = 0;         // 0..1
    float afterglowBalance = 0;     // 0..1
    float blurBalance = 0;          // 0..1
    float reflectionIntensity = 0;  // 0..1, ColorizationGlassReflectionIntensity
};

// Windows 7 default "Sky" colorization (registry defaults 0x6B74B8FC, 8/43/49, reflection 50).
inline constexpr Colorization kWin7Sky{0x6B74B8FC, 0x6B74B8FC, 0.08f, 0.43f, 0.49f, 0.50f};

// Inactive windows: Win7 glass loses its colour and is clear (owner's capture: an inactive
// WMP next to an active Explorer). No base colour or afterglow; the blurred backdrop takes
// the whole share.
constexpr Colorization MakeInactive(const Colorization& active) {
    Colorization c = active;
    const float total = active.colorBalance + active.afterglowBalance + active.blurBalance;
    c.colorBalance = 0.0f;
    c.afterglowBalance = 0.0f;
    c.blurBalance = total;
    c.reflectionIntensity = active.reflectionIntensity * 0.8f;
    return c;
}

// Rec.601 luma weights (Win7-era DWM mixed in gamma space).
inline constexpr std::array<float, 3> kLuma{0.299f, 0.587f, 0.114f};

// Row-major D2D1_MATRIX_5X4_F layout: m[row][col]; rows 0..3 are the input R/G/B/A
// coefficients, row 4 the constant offset; columns are the output R/G/B/A.
inline std::array<float, 20> BuildGlassColorMatrix(const Colorization& c) {
    const Rgb base = RgbFromArgb(c.color);
    const Rgb glow = RgbFromArgb(c.afterglow);
    const float baseRgb[3] = {base.r, base.g, base.b};
    const float glowRgb[3] = {glow.r, glow.g, glow.b};

    std::array<float, 20> m{};
    for (int out = 0; out < 3; ++out) {
        for (int in = 0; in < 3; ++in) {
            float v = kLuma[in] * glowRgb[out] * c.afterglowBalance;
            if (in == out) v += c.blurBalance;
            m[in * 4 + out] = v;
        }
        m[4 * 4 + out] = baseRgb[out] * c.colorBalance;  // constant offset
    }
    m[3 * 4 + 3] = 0.0f;  // output alpha does not depend on the input
    m[4 * 4 + 3] = 1.0f;  // the glass itself is opaque; transparency comes from the backdrop
    return m;
}

}  // namespace glasswave::aero
