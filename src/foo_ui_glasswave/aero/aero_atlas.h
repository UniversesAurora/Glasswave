#pragma once
#include "aero/aero_painter.h"

// Windows 7 DWM artwork from the user's aero.msstyles (ADR 0007).
//
// Win7's uDWM draws caption buttons, their hover glow, the top-left glare and the glass
// reflection streaks from one atlas, STREAM resource 971 of aero.msstyles (1357x780).
// The rectangles below are the DWMWINDOW class TMT_ATLASRECT values read from the
// theme's VARIANT data, with their sizing (nine-grid) and content margins. Glyphs come
// in 96 / 120 / 144 / 192 DPI sizes, so caption glyphs are drawn 1:1 at those scales.

namespace glasswave::aero::atlas {

bool Available();

enum class Button { Minimize, Maximize, Close };

// Caption button background and glyph. `inactive` selects the inactive-window images.
void CaptionButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Button button, paint::CaptionGlyph glyph,
                   paint::Layer layer);
// Hover glow around a caption button; `size` is the button plus GlowOutset().
void CaptionGlow(ID2D1DeviceContext* dc, D2D1_SIZE_F size, bool close);
D2D1_RECT_F GlowOutset(bool close);  // left, top, right, bottom (DIP)

// Top-left glare of the glass frame (228x175 DIP).
inline constexpr D2D1_SIZE_F kGlareSize{228, 175};
void Glare(ID2D1DeviceContext* dc, bool active);
// Reflection streak texture stretched over `size` (the monitor).
void Streaks(ID2D1DeviceContext* dc, D2D1_SIZE_F size);

}  // namespace glasswave::aero::atlas
