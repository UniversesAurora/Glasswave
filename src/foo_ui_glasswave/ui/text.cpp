#include "pch.h"
#include "ui/text.h"
#include "gfx/composition_host.h"

namespace glasswave::ui {

winrt::com_ptr<IDWriteTextLayout> MakeText(std::wstring_view text, float size, DWRITE_FONT_WEIGHT weight,
                                           float maxW, float maxH, DWRITE_TEXT_ALIGNMENT align,
                                           const wchar_t* family) {
    auto* dw = gfx::CompositionHost::Get().DWrite();
    winrt::com_ptr<IDWriteTextFormat> fmt;
    winrt::check_hresult(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", fmt.put()));
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.get(), ellipsis.put())))
        fmt->SetTrimming(&trimming, ellipsis.get());
    winrt::com_ptr<IDWriteTextLayout> layout;
    winrt::check_hresult(dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt.get(),
                                              std::max(maxW, 1.0f), std::max(maxH, 1.0f), layout.put()));
    return layout;
}

std::wstring Wide(const char* utf8) { return std::wstring(pfc::stringcvt::string_wide_from_utf8(utf8 ? utf8 : "")); }

winrt::com_ptr<IDWriteTextLayout> MakeWrappedText(std::wstring_view text, float size, DWRITE_FONT_WEIGHT weight,
                                                  float maxW, float maxH, DWRITE_TEXT_ALIGNMENT align,
                                                  const wchar_t* family) {
    auto* dw = gfx::CompositionHost::Get().DWrite();
    winrt::com_ptr<IDWriteTextFormat> fmt;
    winrt::check_hresult(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", fmt.put()));
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.get(), ellipsis.put())))
        fmt->SetTrimming(&trimming, ellipsis.get());
    winrt::com_ptr<IDWriteTextLayout> layout;
    winrt::check_hresult(dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt.get(),
                                              std::max(maxW, 1.0f), std::max(maxH, 1.0f), layout.put()));
    return layout;
}
std::string Utf8(std::wstring_view wide) {
    return std::string(pfc::stringcvt::string_utf8_from_wide(std::wstring(wide).c_str()));
}

std::wstring FormatTime(double seconds) {
    if (!(seconds >= 0)) seconds = 0;
    const long long s = static_cast<long long>(seconds);
    wchar_t buf[32];
    if (s >= 3600)
        swprintf_s(buf, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else
        swprintf_s(buf, L"%lld:%02lld", s / 60, s % 60);
    return buf;
}

}  // namespace glasswave::ui
