#pragma once
#include "aero/aero_painter.h"
#include "media/list_sources.h"

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
// Vertical scrollbar with the original Aero arrow plates/glyphs and thumb when aero.msstyles
// is available (fallbacks are drawn otherwise). thumbTop/thumbHeight are DIP within `size`;
// 0 height hides the thumb. partBlends[part] (ScrollBarPart 1..4) is 0 normal, 1 hot,
// 2 pressed; intermediate values cross-fade, as Win7 does.
enum class ScrollBarPart { None, ArrowUp, ArrowDown, Track, Thumb };
void ScrollBar(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float thumbTop, float thumbHeight,
               const float* partBlends);

// Toolbar text button ("Organize", "Create playlist") with optional drop-down arrow.
void CommandButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, std::wstring_view text, bool dropDown);

// Plates and arrow of a WMP 12 split button (main part [0, splitX), arrow part after it);
// part is the half under the mouse (0 main, 1 arrow). False without the WMP art.
bool SplitButtonPlate(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, float splitX, int part, bool menuOpen);
// View options icon (original strip 331) for the current view style, centred in size.
void ViewOptionsGlyph(ID2D1DeviceContext* dc, D2D1_SIZE_F size, media::ViewStyle style);

// Explorer-style round back/forward button.
void NavArrow(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, bool back, bool enabled);

// A green play triangle (16 DIP box), the band's Play all: rounded corners, a white rim.
void GreenPlay(ID2D1DeviceContext* dc, D2D1_RECT_F box);

// Search box frame (white field, border, magnifier; a cross while it holds a search).
// The cross is WMP 12's clear button (wmploc WW_CLEAR, measured at 144 DPI): `clearHot` and
// `clearPressed` (0..1) fade in its hot and pressed plates.
void SearchFrame(ID2D1DeviceContext* dc, D2D1_SIZE_F size, bool focused, bool clear = false, float clearHot = 0,
                 float clearPressed = 0);

// Seek bar: track, elapsed fill, and (when hot) the thumb.
// WMP 12 seek bar: three rows under the transport band's top rule at `ruleY` (DIP).
void SeekBar(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float progress, bool hot, bool enabled, float ruleY);

// Small glyphs used in lists and the navigation pane.
enum class NavIcon { None, Library, Playlist, Music, Artist, Album, Genre, NowPlaying, Disc };
void NavGlyph(ID2D1DeviceContext* dc, D2D1_POINT_2F center, NavIcon icon);
// Expand / collapse triangle.
void Disclosure(ID2D1DeviceContext* dc, D2D1_POINT_2F center, bool expanded, bool hot = false);
// Little "playing" speaker marker for the current track.
void PlayingMarker(ID2D1DeviceContext* dc, D2D1_POINT_2F center, bool paused);

}  // namespace glasswave::wmp::paint
