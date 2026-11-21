#pragma once

#include <optional>

// The colour a cover lends the glass (the window's tint when it follows the cover, the
// lyrics pane's background): the picture's most prominent vivid hue, brought into the range
// of Windows 7's own colour swatches; a grey for a picture without one.
namespace glasswave::ui {

std::optional<uint32_t> CoverTint(IWICBitmapSource* art); // 0xRRGGBB; none without a picture

}  // namespace glasswave::ui
