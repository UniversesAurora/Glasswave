#pragma once

namespace glasswave::ui {

// DirectWrite layout with Segoe UI defaults. Locale zh-cn so system font fallback
// picks Microsoft YaHei UI for Han characters in titles and tags.
winrt::com_ptr<IDWriteTextLayout> MakeText(std::wstring_view text, float size,
                                           DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                           float maxW = 4096.0f, float maxH = 64.0f,
                                           DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                                           const wchar_t* family = L"Segoe UI");

std::wstring Wide(const char* utf8);
std::string Utf8(std::wstring_view wide);

// "m:ss" or "h:mm:ss"
std::wstring FormatTime(double seconds);

}  // namespace glasswave::ui
