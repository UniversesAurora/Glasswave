#include "pch.h"
#include "ui/text.h"
#include "gfx/composition_host.h"

namespace glasswave::ui {

float DrawTag(ID2D1DeviceContext* dc, std::wstring_view text, float x, float cy, float right, D2D1_COLOR_F ink,
              float size, float scale) {
    constexpr float kPadX = 2.5f, kPadY = 1.5f; // DIP between the ink and the outline
    auto layout = MakeText(text, size, DWRITE_FONT_WEIGHT_NORMAL, 400, 40);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    // The overhangs are measured against the layout box: make it the text's own size.
    layout->SetMaxWidth(std::max(m.widthIncludingTrailingWhitespace, 1.0f));
    layout->SetMaxHeight(std::max(m.height, 1.0f));
    DWRITE_OVERHANG_METRICS o{};
    layout->GetOverhangMetrics(&o);
    const float inkLeft = -o.left, inkTop = -o.top;
    const float inkWidth = std::max(m.widthIncludingTrailingWhitespace + o.left + o.right, 1.0f);
    const float inkHeight = std::max(m.height + o.top + o.bottom, 1.0f);
    const auto snap = [scale](float v) { return std::round(v * scale) / scale; };
    const float w = snap(inkWidth + 2 * kPadX), h = snap(inkHeight + 2 * kPadY);
    const float left = snap(x), top = snap(cy - h / 2);
    if (left + w > right) return 0;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(ink, brush.put());
    const float px = 1.0f / scale;
    dc->DrawRoundedRectangle({{left + px / 2, top + px / 2, left + w - px / 2, top + h - px / 2}, 2.0f, 2.0f},
                             brush.get(), px);
    dc->DrawTextLayout({left + kPadX - inkLeft, top + kPadY - inkTop}, layout.get(), brush.get());
    return w;
}

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
