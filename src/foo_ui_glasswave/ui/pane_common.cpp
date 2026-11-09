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

void DrawIcon(ID2D1DeviceContext* dc, wchar_t glyph, float size, D2D1_POINT_2F c, ID2D1Brush* brush,
              DWRITE_FONT_WEIGHT weight) {
    auto* f = gfx::CompositionHost::Get().DWrite();
    static const std::wstring family = [f] {
        com_ptr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(f->GetSystemFontCollection(fonts.put()))) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) && exists)
                return std::wstring(L"Segoe Fluent Icons");
        }
        return std::wstring(L"Segoe MDL2 Assets");
    }();
    com_ptr<IDWriteTextFormat> format;
    if (FAILED(f->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, size, L"", format.put())))
        return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const wchar_t text[1] = {glyph};
    dc->DrawText(text, 1, format.get(), D2D1::RectF(c.x - size, c.y - size, c.x + size, c.y + size), brush);
}

void DrawCapsule(ID2D1DeviceContext* dc, D2D1_RECT_F bounds, ID2D1Brush* brush) {
    const float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return;
    // The iOS endcap eases out of the straight edge before meeting the circular side.
    // Its cubic has zero curvature at the straight edge and matches the circle's
    // tangent and curvature at 45 degrees. See docs/design/visual-spec.md.
    constexpr float extent = 1.3f;
    constexpr float first = 0.94974747f, second = 0.58578644f, diagonal = 0.29289322f;
    const float radius = std::min(height / 2, width / (2 * extent));
    const float reach = extent * radius;
    const float a = first * radius, b = second * radius, d = diagonal * radius;
    const float l = bounds.left, t = bounds.top, r = bounds.right, bottom = bounds.bottom;
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    com_ptr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(path.put()))) return;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;
    const auto arc = [&](float x, float y) {
        sink->AddArc({{x, y}, {radius, radius}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    };
    sink->BeginFigure({l + reach, t}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine({r - reach, t});
    sink->AddBezier({{r - a, t}, {r - b, t}, {r - d, t + d}});
    arc(r, t + radius);
    sink->AddLine({r, bottom - radius});
    arc(r - d, bottom - d);
    sink->AddBezier({{r - b, bottom}, {r - a, bottom}, {r - reach, bottom}});
    sink->AddLine({l + reach, bottom});
    sink->AddBezier({{l + a, bottom}, {l + b, bottom}, {l + d, bottom - d}});
    arc(l, bottom - radius);
    sink->AddLine({l, t + radius});
    arc(l + d, t + d);
    sink->AddBezier({{l + b, t}, {l + a, t}, {l + reach, t}});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;
    dc->FillGeometry(path.get(), brush);
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
