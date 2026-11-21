#pragma once
#include "aero/aero_atlas.h"
#include "ui/widget.h"

// Windows 7's caption buttons, the same in the library and player windows: DWM's own images
// and hover glow when aero.msstyles' atlas is there, else drawn.

namespace glasswave::ui {

using CaptionButton = aero::atlas::Button;

// `maximized` (Maximize only, optional): while it says so the button shows Restore.
Widget::Painter CaptionPainter(CaptionButton button, bool atlasArt, std::function<bool()> maximized = {});
// The window-inactive look, and the atlas' glow around the button.
Widget::Options CaptionOptions(CaptionButton button, bool atlasArt);

} // namespace glasswave::ui
