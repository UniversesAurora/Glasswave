#pragma once

namespace glasswave::ui {

// DirectWrite layout with Segoe UI defaults. Locale zh-cn so system font fallback
// picks Microsoft YaHei UI for Han characters in titles and tags.
winrt::com_ptr<IDWriteTextLayout> MakeText(std::wstring_view text, float size,
                                           DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                           float maxW = 4096.0f, float maxH = 64.0f,
                                           DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                                           const wchar_t* family = L"Segoe UI");

// Multi-line text block: wraps at word boundaries and trims the last visible line.
winrt::com_ptr<IDWriteTextLayout> MakeWrappedText(std::wstring_view text, float size,
                                                  DWRITE_FONT_WEIGHT weight, float maxW, float maxH,
                                                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                                                  const wchar_t* family = L"Segoe UI");

// Width DrawTag() would take for `text` at `x`, or 0 when it does not fit before `right`.
float MeasureTagWidth(std::wstring_view text, float x, float right, float size, float scale);

// A tag drawn by a name (a song's 原唱 before its artist): its text in `ink` inside a thin
// rounded outline that hugs the glyphs' own ink with an even narrow margin, centred on `cy`
// whatever the font's line metrics (the line box put Han glyphs low and to the right). Returns
// its width; 0 when it does not fit before `right`. `scale` snaps it to device pixels.
float DrawTag(ID2D1DeviceContext* dc, std::wstring_view text, float x, float cy, float right, D2D1_COLOR_F ink,
              float size, float scale);

// "m:ss" or "h:mm:ss"
std::wstring FormatTime(double seconds);

}  // namespace glasswave::ui
