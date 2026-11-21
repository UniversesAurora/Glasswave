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

// The sixteen colours of Win7's "Window Color and Appearance" page, in its order, read from
// the swatch bitmaps in Win7's themecpl.dll (resources 1-8 and 21-28; names: its .mui strings
// 101-116). A swatch sets the colour only: the intensity slider stays where it is.
struct Win7Tint {
    const wchar_t* name; // English, translated where shown
    uint32_t rgb;
};
inline constexpr std::array<Win7Tint, 16> kWin7Tints{{{L"Sky", 0x74B8FC},
                                                       {L"Twilight", 0x0046AD},
                                                       {L"Sea", 0x32CDCD},
                                                       {L"Leaf", 0x14A600},
                                                       {L"Lime", 0x97D937},
                                                       {L"Sun", 0xFADC0E},
                                                       {L"Pumpkin", 0xFF9C00},
                                                       {L"Ruby", 0xCE0F0F},
                                                       {L"Fuchsia", 0xFF0099},
                                                       {L"Blush", 0xFCC7F8},
                                                       {L"Violet", 0x6E3BA1},
                                                       {L"Lavender", 0x8D5A94},
                                                       {L"Taupe", 0x98844C},
                                                       {L"Chocolate", 0x4F1B1B},
                                                       {L"Slate", 0x555555},
                                                       {L"Frost", 0xDEDEDE}}};

// Win7's "Color intensity" slider position (0..100) for the Sky theme's alpha 0x6B.
inline constexpr int kWin7SkyIntensity = 43;

// What Win7's themecpl.dll hands DWM (DwmpSetColorizationParameters) for a colour and an
// intensity slider position, transparency on. Read from its code:
//   alpha = (0.10 + 0.75 * slider / 100) * 255
//   v     = trunc((alpha / 255 - 0.10) / 0.75 * 100 + 10)              (10..110)
//   v < 50:       colour 5,          afterglow v - 5,  blur 100 - v
//   50 <= v < 95: afterglow 95 - v,  blur 50 - (v - 50) / 2,  colour the rest of 100
//   v >= 95:      colour v - 25,     afterglow 0,      blur 125 - v
// colour and afterglow both the chosen ARGB, reflection 50. Sky at its default gives
// 0x6B74B8FC, 8/43/49: the registry defaults above.
constexpr Colorization Win7Colorization(uint32_t rgb, int slider) {
    slider = slider < 0 ? 0 : slider > 100 ? 100 : slider;
    const auto alpha = static_cast<uint32_t>((0.10 + 0.75 * slider / 100.0) * 255.0);
    const int v = static_cast<int>((alpha / 255.0 - 0.10) / 0.75 * 100.0 + 10.0);
    int colour = 0, afterglow = 0, blur = 0;
    if (v < 50) {
        colour = 5;
        afterglow = v - 5;
        blur = 100 - v;
    } else if (v < 95) {
        afterglow = 95 - v;
        blur = 50 - (v - 50) / 2;
        colour = 100 - afterglow - blur;
    } else {
        colour = v - 25;
        blur = 125 - v;
    }
    const uint32_t argb = (alpha << 24) | (rgb & 0xFFFFFF);
    return {argb, argb, colour / 100.0f, afterglow / 100.0f, blur / 100.0f, 0.50f};
}
static_assert(Win7Colorization(0x74B8FC, kWin7SkyIntensity).color == kWin7Sky.color);
static_assert(Win7Colorization(0x74B8FC, kWin7SkyIntensity).afterglowBalance == kWin7Sky.afterglowBalance);

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
