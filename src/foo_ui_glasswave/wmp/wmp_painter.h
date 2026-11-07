#pragma once
#include "aero/aero_painter.h"

// Direct2D painters for the WMP 12 client chrome (lists, navigation pane, command bar,
// search box, seek bar). Win7 Explorer / WMP 12 styling; values provisional until the
// owner's Win7 reference captures are measured (docs/design/visual-spec.md).

namespace glasswave::wmp::paint {

using aero::paint::Layer;

enum class RowState { Normal, Hot, Selected, SelectedHot };

// Win7 list hot-track / selection plate (rounded, gradient, 1-px border).
void RowPlate(ID2D1DeviceContext* dc, D2D1_RECT_F r, RowState state, bool focused);

// Column header strip background and separator lines.
void HeaderBackground(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
void HeaderSeparator(ID2D1DeviceContext* dc, float x, float height);

// Slim Win7-style scrollbar: track + thumb (no arrow buttons).
void ScrollBar(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float thumbTop, float thumbHeight, bool hot);

// Toolbar text button ("Organize", "Create playlist") with optional drop-down arrow.
void CommandButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, std::wstring_view text, bool dropDown);

// Explorer-style round back/forward button.
void NavArrow(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, bool back, bool enabled);

// Search box frame (white field, border, magnifier).
void SearchFrame(ID2D1DeviceContext* dc, D2D1_SIZE_F size, bool focused);

// Seek bar: track, elapsed fill, and (when hot) the thumb.
void SeekBar(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float progress, bool hot, bool enabled);

// Small glyphs used in lists and the navigation pane.
enum class NavIcon { None, Library, Playlist, Music, Artist, Album, Genre, NowPlaying };
void NavGlyph(ID2D1DeviceContext* dc, D2D1_POINT_2F center, NavIcon icon);
// Expand / collapse triangle.
void Disclosure(ID2D1DeviceContext* dc, D2D1_POINT_2F center, bool expanded);
// Little "playing" speaker marker for the current track.
void PlayingMarker(ID2D1DeviceContext* dc, D2D1_POINT_2F center, bool paused);

}  // namespace glasswave::wmp::paint
