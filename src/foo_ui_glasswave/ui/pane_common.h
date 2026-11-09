#pragma once

// Shared look of the now-playing pane (lyrics, queue, header, bottom bar): white text in the
// Apple Music style on the window's glass. Values: docs/design/lyrics-view.md.

namespace glasswave::ui::pane {

inline constexpr float kHeaderHeight = 84;  // DIP: cover, title and artist
inline constexpr float kCover = 52;         // DIP: header cover
inline constexpr float kBottomBar = 64;     // DIP: the lyrics / queue switch
inline constexpr float kSideMargin = 12;    // DIP: hover plates' inset from the pane edge
inline constexpr float kPaneTint = 0.30f;   // black over the glass so white text reads

// Segoe UI Variable Display (else Segoe UI) in the user's locale; wrapping text.
winrt::com_ptr<IDWriteTextFormat> Format(float size, DWRITE_FONT_WEIGHT weight);
winrt::com_ptr<ID2D1SolidColorBrush> White(ID2D1DeviceContext* dc, float alpha);
// One line of text with an ellipsis where it does not fit.
void DrawTextLine(ID2D1DeviceContext* dc, const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight,
                  D2D1_POINT_2F at, float width, ID2D1Brush* brush,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
// A glyph of Windows' icon font (Segoe Fluent Icons, else Segoe MDL2 Assets) centred on `c`.
namespace icon {
inline constexpr wchar_t kShuffle = 0xE8B1, kRepeatAll = 0xE8EE, kRepeatOne = 0xE8ED, kList = 0xE8FD;
}
void DrawIcon(ID2D1DeviceContext* dc, wchar_t glyph, float size, D2D1_POINT_2F c, ID2D1Brush* brush,
              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
// Continuous-curvature capsule measured from the owner's iOS references.
void DrawCapsule(ID2D1DeviceContext* dc, D2D1_RECT_F bounds, ID2D1Brush* brush);
// A cover cropped to fill `box` with rounded corners and a soft shadow below.
void DrawCover(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap, D2D1_RECT_F box, float radius);
// The current song's header: cover, title, artist (lyrics and queue share it).
void DrawHeader(ID2D1DeviceContext* dc, float width, ID2D1Bitmap* cover, const std::wstring& title,
                const std::wstring& artist);

}  // namespace glasswave::ui::pane
