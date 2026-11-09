#include "pch.h"

#include "ui/pane_common.h"

#include "gfx/composition_host.h"

namespace glasswave::ui::pane {

using winrt::com_ptr;

namespace {

std::wstring DefaultLocale() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) return name;
    return L"en-us";
}

std::wstring PickFamily(IDWriteFactory* f) {
    com_ptr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(f->GetSystemFontCollection(fonts.put()))) {
        for (const wchar_t* family : {L"Segoe UI Variable Display", L"Segoe UI"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) return family;
        }
    }
    return L"Segoe UI";
}

}  // namespace

com_ptr<IDWriteTextFormat> Format(float size, DWRITE_FONT_WEIGHT weight) {
    auto* f = gfx::CompositionHost::Get().DWrite();
    static const std::wstring family = PickFamily(f);
    static const std::wstring locale = DefaultLocale();
    com_ptr<IDWriteTextFormat> format;
    winrt::check_hresult(f->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, size, locale.c_str(), format.put()));
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return format;
}

com_ptr<ID2D1SolidColorBrush> White(ID2D1DeviceContext* dc, float alpha) {
    com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, alpha), b.put());
    return b;
}

void DrawTextLine(ID2D1DeviceContext* dc, const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight,
                  D2D1_POINT_2F at, float width, ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align) {
    if (text.empty() || width <= 1) return;
    auto* f = gfx::CompositionHost::Get().DWrite();
    auto format = Format(size, weight);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(align);
    com_ptr<IDWriteTextLayout> layout;
    if (FAILED(f->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), width, size * 1.5f,
                                   layout.put())))
        return;
    com_ptr<IDWriteInlineObject> ellipsis;
    f->CreateEllipsisTrimmingSign(layout.get(), ellipsis.put());
    const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trim, ellipsis.get());
    dc->DrawTextLayout(at, layout.get(), brush);
}

void DrawCover(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap, D2D1_RECT_F box, float radius) {
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    com_ptr<ID2D1SolidColorBrush> shade;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.18f), shade.put());
    dc->FillRoundedRectangle(D2D1::RoundedRect({box.left, box.top + 2, box.right, box.bottom + 2}, radius + 1, radius + 1),
                             shade.get());
    if (!bitmap) {
        // No cover: Apple Music's grey placeholder.
        dc->FillRoundedRectangle(D2D1::RoundedRect(box, radius, radius), White(dc, 0.12f).get());
        return;
    }
    com_ptr<ID2D1RoundedRectangleGeometry> clip;
    factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box, radius, radius), clip.put());
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get()), nullptr);
    const D2D1_SIZE_F s = bitmap->GetSize();
    const float bw = box.right - box.left, bh = box.bottom - box.top;
    const float k = std::max(bw / std::max(1.0f, s.width), bh / std::max(1.0f, s.height));
    const float dw = s.width * k, dh = s.height * k;
    const D2D1_RECT_F dst{box.left + (bw - dw) / 2, box.top + (bh - dh) / 2, box.left + (bw + dw) / 2,
                          box.top + (bh + dh) / 2};
    dc->DrawBitmap(bitmap, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    dc->PopLayer();
}

void DrawHeader(ID2D1DeviceContext* dc, float width, ID2D1Bitmap* cover, const std::wstring& title,
                const std::wstring& artist) {
    if (title.empty()) return;
    const float x0 = kSideMargin + 8, y0 = (kHeaderHeight - kCover) / 2;
    DrawCover(dc, cover, {x0, y0, x0 + kCover, y0 + kCover}, 6);
    const float textX = x0 + kCover + 14;
    const float textW = std::max(10.0f, width - textX - kSideMargin - 8);
    const float mid = kHeaderHeight / 2;
    DrawTextLine(dc, title, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, {textX, mid - 22}, textW, White(dc, 0.95f).get());
    DrawTextLine(dc, artist, 14, DWRITE_FONT_WEIGHT_NORMAL, {textX, mid + 1}, textW, White(dc, 0.6f).get());
}

}  // namespace glasswave::ui::pane
