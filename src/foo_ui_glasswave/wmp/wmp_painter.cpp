#include "pch.h"
#include "wmp/wmp_painter.h"
#include "ui/text.h"
#include "win7/win7_resources.h"

namespace glasswave::wmp::paint {

namespace {

using winrt::com_ptr;

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

com_ptr<ID2D1SolidColorBrush> Solid(ID2D1DeviceContext* dc, D2D1_COLOR_F c) {
    com_ptr<ID2D1SolidColorBrush> b;
    winrt::check_hresult(dc->CreateSolidColorBrush(c, b.put()));
    return b;
}

com_ptr<ID2D1LinearGradientBrush> Vertical(ID2D1DeviceContext* dc, float y0, float y1,
                                           std::initializer_list<std::pair<float, D2D1_COLOR_F>> stops) {
    std::vector<D2D1_GRADIENT_STOP> v;
    for (auto& [p, c] : stops) v.push_back({p, c});
    com_ptr<ID2D1GradientStopCollection> coll;
    winrt::check_hresult(dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), coll.put()));
    com_ptr<ID2D1LinearGradientBrush> b;
    winrt::check_hresult(dc->CreateLinearGradientBrush({{0, y0}, {0, y1}}, coll.get(), b.put()));
    return b;
}

com_ptr<ID2D1LinearGradientBrush> Horizontal(ID2D1DeviceContext* dc, float x0, float x1,
                                             std::initializer_list<std::pair<float, D2D1_COLOR_F>> stops) {
    std::vector<D2D1_GRADIENT_STOP> v;
    for (auto& [p, c] : stops) v.push_back({p, c});
    com_ptr<ID2D1GradientStopCollection> coll;
    winrt::check_hresult(dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), coll.put()));
    com_ptr<ID2D1LinearGradientBrush> b;
    winrt::check_hresult(dc->CreateLinearGradientBrush({{x0, 0}, {x1, 0}}, coll.get(), b.put()));
    return b;
}

float Hairline(ID2D1DeviceContext* dc) {
    float dx = 96, dy = 96;
    dc->GetDpi(&dx, &dy);
    const float s = dx / 96.0f;
    return std::max(1.0f, std::floor(s)) / s;
}

float Snap(ID2D1DeviceContext* dc, float v) {
    float dx = 96, dy = 96;
    dc->GetDpi(&dx, &dy);
    const float s = dx / 96.0f;
    return std::round(v * s) / s;
}

D2D1_RECT_F Inset(D2D1_RECT_F r, float d) { return {r.left + d, r.top + d, r.right - d, r.bottom - d}; }

com_ptr<ID2D1PathGeometry> Polygon(ID2D1DeviceContext* dc, std::initializer_list<D2D1_POINT_2F> pts) {
    com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    com_ptr<ID2D1PathGeometry> g;
    winrt::check_hresult(f->CreatePathGeometry(g.put()));
    com_ptr<ID2D1GeometrySink> s;
    winrt::check_hresult(g->Open(s.put()));
    auto it = pts.begin();
    s->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
    for (++it; it != pts.end(); ++it) s->AddLine(*it);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    winrt::check_hresult(s->Close());
    return g;
}

}  // namespace

void RowPlate(ID2D1DeviceContext* dc, D2D1_RECT_F r, RowState state, bool focused) {
    if (state == RowState::Normal) return;
    const float L = Hairline(dc);
    const D2D1_RECT_F rc{Snap(dc, r.left), Snap(dc, r.top), Snap(dc, r.right), Snap(dc, r.bottom)};
    const D2D1_ROUNDED_RECT outer{Inset(rc, L / 2), 2.5f, 2.5f};
    D2D1_COLOR_F top, bottom, border, inner;
    switch (state) {
    case RowState::Hot:  // Win7 Explorer hot-track
        top = Hex(0xFAFBFD), bottom = Hex(0xEBF3FD), border = Hex(0xB8D6FB), inner = Hex(0xFFFFFF, 0.6f);
        break;
    case RowState::SelectedHot:
        top = Hex(0xDCEBFC), bottom = Hex(0xC1DBFC), border = Hex(0x7DA2CE), inner = Hex(0xEBF4FE, 0.7f);
        break;
    default:  // Selected
        top = focused ? Hex(0xDCEBFC) : Hex(0xF8F8F8), bottom = focused ? Hex(0xC1DBFC) : Hex(0xE5E5E5);
        border = focused ? Hex(0x7DA2CE) : Hex(0xD9D9D9), inner = Hex(0xEBF4FE, focused ? 0.7f : 0.0f);
        break;
    }
    dc->FillRoundedRectangle(outer, Vertical(dc, rc.top, rc.bottom, {{0.0f, top}, {1.0f, bottom}}).get());
    dc->DrawRoundedRectangle({Inset(rc, L * 1.5f), 1.5f, 1.5f}, Solid(dc, inner).get(), L);
    dc->DrawRoundedRectangle(outer, Solid(dc, border).get(), L);
}

void HeaderBackground(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const float L = Hairline(dc);
    dc->FillRectangle({0, 0, size.width, size.height},
                      Vertical(dc, 0, size.height, {{0.0f, Hex(0xFFFFFF)}, {1.0f, Hex(0xF4F7FB)}}).get());
    dc->FillRectangle({0, size.height - L, size.width, size.height}, Solid(dc, Hex(0xD5DFE9)).get());
}

void HeaderSeparator(ID2D1DeviceContext* dc, float x, float height) {
    const float L = Hairline(dc);
    x = Snap(dc, x);
    dc->FillRectangle({x - L, 3, x, height - 3},
                      Vertical(dc, 0, height, {{0.0f, Hex(0xE3E8EE, 0.0f)}, {0.5f, Hex(0xDCE2E9)},
                                               {1.0f, Hex(0xE3E8EE, 0.0f)}})
                          .get());
}

void ScrollBar(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float thumbTop, float thumbHeight, bool hot) {
    const float L = Hairline(dc);
    dc->FillRectangle({0, 0, size.width, size.height}, Solid(dc, Hex(0xF4F7FB)).get());
    dc->FillRectangle({0, 0, L, size.height}, Solid(dc, Hex(0xE3E9F0)).get());
    if (thumbHeight <= 0) return;
    const D2D1_RECT_F t{Snap(dc, 3), Snap(dc, thumbTop), Snap(dc, size.width - 2), Snap(dc, thumbTop + thumbHeight)};
    const D2D1_ROUNDED_RECT rr{Inset(t, L / 2), 2.5f, 2.5f};
    dc->FillRoundedRectangle(rr, Vertical(dc, t.top, t.bottom, {{0.0f, hot ? Hex(0xE3F4FC) : Hex(0xF3F5F8)},
                                                                {1.0f, hot ? Hex(0xC4E3F7) : Hex(0xDDE3EA)}})
                                     .get());
    dc->DrawRoundedRectangle(rr, Solid(dc, hot ? Hex(0x3C7FB1) : Hex(0x9AA6B3)).get(), L);
    // Grip
    const float cy = (t.top + t.bottom) / 2, cx0 = t.left + 3, cx1 = t.right - 3;
    if (t.bottom - t.top > 16)
        for (int i = -1; i <= 1; ++i) {
            const float y = Snap(dc, cy + i * 2.5f);
            dc->FillRectangle({cx0, y, cx1, y + L}, Solid(dc, hot ? Hex(0x3C7FB1, 0.6f) : Hex(0x8A97A6, 0.6f)).get());
        }
}

void CommandButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, std::wstring_view text, bool dropDown) {
    const float L = Hairline(dc);
    const D2D1_RECT_F rc{0, 0, Snap(dc, size.width), Snap(dc, size.height)};
    using win7::Module;
    const int state = layer == Layer::Pressed ? 2 : layer == Layer::Hover ? 1 : 0;
    // Original WMP 12 art: WMP_TOOLBARBUTTON plates (empty / hot / pressed, 17x22, nine-grid)
    // for every button and both halves of split buttons; the arrow alone is the first cell
    // of WW_SPLIT (14x20), drawn 1:1 so it never scales with the plate.
    const bool art = win7::Resources::Get().Has(Module::Wmploc, L"WMP_TOOLBARBUTTON.PNG");
    if (art && state > 0) {
        win7::DrawNineGrid(dc, Module::Wmploc, L"WMP_TOOLBARBUTTON.PNG", {17.0f * state, 0, 17.0f * state + 17, 22},
                           {3, 3, 3, 3}, rc);
    }
    if (art && text.empty() && dropDown) {
        const float ax = Snap(dc, size.width / 2 - 7), ay = Snap(dc, size.height / 2 - 10);
        if (win7::DrawPartTo(dc, Module::Wmploc, L"WW_SPLIT.PNG", D2D1::RectU(0, 0, 14, 20), {ax, ay, ax + 14, ay + 20}))
            return;
    }
    if (art) {
    } else if (layer == Layer::Hover || layer == Layer::Pressed) {
        const bool pressed = layer == Layer::Pressed;
        const D2D1_ROUNDED_RECT rr{Inset(rc, L / 2), 2.5f, 2.5f};
        dc->FillRoundedRectangle(rr, Vertical(dc, rc.top, rc.bottom,
                                              pressed ? std::initializer_list<std::pair<float, D2D1_COLOR_F>>{
                                                            {0.0f, Hex(0xC9DCF0)}, {1.0f, Hex(0xB4CDE8)}}
                                                      : std::initializer_list<std::pair<float, D2D1_COLOR_F>>{
                                                            {0.0f, Hex(0xFBFDFF)}, {0.5f, Hex(0xEBF3FC)},
                                                            {0.5f, Hex(0xDDEBFA)}, {1.0f, Hex(0xE9F3FE)}})
                                         .get());
        dc->DrawRoundedRectangle({Inset(rc, L * 1.5f), 1.5f, 1.5f}, Solid(dc, Hex(0xFFFFFF, pressed ? 0.3f : 0.8f)).get(), L);
        dc->DrawRoundedRectangle(rr, Solid(dc, Hex(0x8EB0D3)).get(), L);
    }
    const float arrowW = dropDown ? 12.0f : 0.0f;
    auto layout = ui::MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, size.width - 12 - arrowW, size.height);
    dc->DrawTextLayout({6, 0}, layout.get(), Solid(dc, Hex(0x1E395B)).get());
    if (dropDown) {  // after the text, or centered on the arrow part of a split button
        const float cx = text.empty() ? std::round(size.width / 2) : size.width - 9, cy = size.height / 2 + 0.5f;
        dc->FillGeometry(Polygon(dc, {{cx - 3.5f, cy - 1.5f}, {cx + 3.5f, cy - 1.5f}, {cx, cy + 2.0f}}).get(),
                         Solid(dc, Hex(0x1E395B)).get());
    }
}

void NavArrow(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, bool back, bool enabled) {
    const float L = Hairline(dc);
    const float r = std::min(size.width, size.height) / 2 - 0.5f;
    const D2D1_POINT_2F c{size.width / 2, size.height / 2};
    // Win7 Explorer: blue glossy orb when enabled, pale glass when disabled.
    D2D1_COLOR_F top, mid, bottom, rim;
    if (!enabled) {
        top = Hex(0xF4F7FB), mid = Hex(0xE2E9F2), bottom = Hex(0xEEF3F9), rim = Hex(0xA9B5C3);
    } else if (layer == Layer::Hover) {
        top = Hex(0x9ED3FB), mid = Hex(0x2E86D8), bottom = Hex(0x6BC8F8), rim = Hex(0x1B4F86);
    } else if (layer == Layer::Pressed) {
        top = Hex(0x6E9CC8), mid = Hex(0x174F94), bottom = Hex(0x2E7DC0), rim = Hex(0x103561);
    } else {
        top = Hex(0x7DB3E8), mid = Hex(0x1E64B4), bottom = Hex(0x4AA2E6), rim = Hex(0x1A4777);
    }
    dc->FillEllipse({c, r, r}, Vertical(dc, c.y - r, c.y + r, {{0.0f, top}, {0.5f, mid}, {1.0f, bottom}}).get());
    dc->FillEllipse({{c.x, c.y - r * 0.42f}, r * 0.72f, r * 0.48f},
                    Vertical(dc, c.y - r, c.y, {{0.0f, Hex(0xFFFFFF, enabled ? 0.65f : 0.8f)},
                                                {1.0f, Hex(0xFFFFFF, 0.05f)}})
                        .get());
    dc->DrawEllipse({c, r - L / 2, r - L / 2}, Solid(dc, rim).get(), L);
    // Arrow
    const float d = back ? -1.0f : 1.0f, a = r * 0.45f;
    auto arrow = Polygon(dc, {{c.x + d * a, c.y}, {c.x, c.y - a}, {c.x, c.y - a * 0.4f}, {c.x - d * a, c.y - a * 0.4f},
                              {c.x - d * a, c.y + a * 0.4f}, {c.x, c.y + a * 0.4f}, {c.x, c.y + a}});
    dc->FillGeometry(arrow.get(), Solid(dc, enabled ? Hex(0xFFFFFF) : Hex(0xB9C4D1)).get());
}

void SearchFrame(ID2D1DeviceContext* dc, D2D1_SIZE_F size, bool focused) {
    const float L = Hairline(dc);
    const D2D1_RECT_F rc{0, 0, Snap(dc, size.width), Snap(dc, size.height)};
    dc->FillRectangle(Inset(rc, L), Solid(dc, Hex(0xFFFFFF)).get());
    dc->DrawRectangle(Inset(rc, L / 2), Vertical(dc, 0, size.height, {{0.0f, focused ? Hex(0x3D7BAD) : Hex(0xABADB3)},
                                                                      {1.0f, focused ? Hex(0xA4C9E3) : Hex(0xE3E9EF)}})
                                            .get(),
                      L);
    // Magnifier at the right
    const D2D1_POINT_2F c{size.width - 12, size.height / 2 - 1};
    auto ink = Solid(dc, Hex(0x2F5F95));
    dc->DrawEllipse({c, 3.6f, 3.6f}, ink.get(), 1.4f);
    dc->DrawLine({c.x - 2.5f, c.y + 2.5f}, {c.x - 6.0f, c.y + 6.0f}, ink.get(), 1.8f);
}

void SeekBar(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float progress, bool hot, bool enabled, float ruleY) {
    // WMP 12 (owner capture at 150 %, bitmap pixels = DIP here): the transport band's top
    // rule is the bar's top edge; under it three rows. Unplayed: background, background,
    // #BEC3CB. Played: #7FC9FA / #014CB6 / #619FF2, and over the last 16 px the head
    // brightens to #C6EDFD / #5CB8F4 / #A0C6EB, then stops sharply.
    const float y = Snap(dc, ruleY) + 1;  // first row under the rule
    const float x0 = 0, x1 = size.width;
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float px = Snap(dc, x0 + (x1 - x0) * progress);
    dc->FillRectangle({x0, y + 2, x1, y + 3}, Solid(dc, Hex(0xBEC3CB)).get());
    if (enabled && progress > 0) {
        struct Row {
            uint32_t body, head;
        };
        const Row rows[] = {{0x7FC9FA, 0xC6EDFD}, {0x014CB6, 0x5CB8F4}, {0x619FF2, 0xA0C6EB}};
        const float h0 = std::max(x0, px - 16);
        for (int r = 0; r < 3; ++r) {
            const D2D1_RECT_F row{x0, y + r, px, y + r + 1};
            dc->FillRectangle(row, Solid(dc, Hex(rows[r].body)).get());
            dc->FillRectangle({h0, row.top, px, row.bottom},
                              Horizontal(dc, h0, px, {{0.0f, Hex(rows[r].body)}, {1.0f, Hex(rows[r].head)}}).get());
        }
    }
    if (enabled && hot) {
        const float L = Hairline(dc);
        const float r = 5.5f;
        const D2D1_POINT_2F c{std::clamp(px, x0 + r, x1 - r), y + 1.5f};
        dc->FillEllipse({c, r, r}, Vertical(dc, y - r, y + r, {{0.0f, Hex(0x6F7F9A)}, {1.0f, Hex(0x3F7486)}}).get());
        dc->FillEllipse({c, r - L, r - L},
                        Vertical(dc, y - r, y + r, {{0.0f, Hex(0xE2EAF6)}, {0.45f, Hex(0xA6C4E6)},
                                                    {0.5f, Hex(0x1C3A80)}, {1.0f, Hex(0x5ED8F5)}})
                            .get());
    }
}

void NavGlyph(ID2D1DeviceContext* dc, D2D1_POINT_2F c, NavIcon icon) {
    auto blue = Solid(dc, Hex(0x3A78C3));
    auto light = Solid(dc, Hex(0xDCE9F8));
    switch (icon) {
    case NavIcon::Library:
        dc->FillRectangle({c.x - 6, c.y - 5, c.x - 3, c.y + 6}, blue.get());
        dc->FillRectangle({c.x - 2, c.y - 6, c.x + 1, c.y + 6}, blue.get());
        dc->FillGeometry(Polygon(dc, {{c.x + 2, c.y - 4}, {c.x + 4.5f, c.y - 5}, {c.x + 7, c.y + 5.5f},
                                      {c.x + 4.5f, c.y + 6.5f}}).get(), blue.get());
        break;
    case NavIcon::Playlist:
    case NavIcon::NowPlaying:
        dc->FillRectangle({c.x - 6, c.y - 6, c.x + 6, c.y + 6}, light.get());
        dc->DrawRectangle({c.x - 5.5f, c.y - 5.5f, c.x + 5.5f, c.y + 5.5f}, blue.get(), 1.0f);
        dc->FillGeometry(Polygon(dc, {{c.x - 2, c.y - 3.5f}, {c.x + 3.5f, c.y}, {c.x - 2, c.y + 3.5f}}).get(),
                         blue.get());
        break;
    case NavIcon::Music:
        dc->FillEllipse({{c.x - 3, c.y + 4}, 2.6f, 2.1f}, blue.get());
        dc->FillRectangle({c.x - 1.0f, c.y - 6, c.x + 0.4f, c.y + 4}, blue.get());
        dc->FillGeometry(Polygon(dc, {{c.x - 1, c.y - 6}, {c.x + 5, c.y - 4}, {c.x + 5, c.y - 1.5f},
                                      {c.x - 1, c.y - 3.5f}}).get(), blue.get());
        break;
    case NavIcon::Artist:
        dc->FillEllipse({{c.x, c.y - 2.5f}, 3.2f, 3.2f}, blue.get());
        dc->FillEllipse({{c.x, c.y + 6}, 6.0f, 4.0f}, blue.get());
        break;
    case NavIcon::Album:
        dc->FillEllipse({c, 6.0f, 6.0f}, blue.get());
        dc->FillEllipse({c, 2.0f, 2.0f}, light.get());
        break;
    case NavIcon::Genre:
        dc->FillRectangle({c.x - 6, c.y - 5, c.x + 6, c.y + 5}, blue.get());
        dc->FillRectangle({c.x - 4, c.y - 3, c.x + 4, c.y - 1.5f}, light.get());
        dc->FillRectangle({c.x - 4, c.y + 0.0f, c.x + 2, c.y + 1.5f}, light.get());
        break;
    default:
        break;
    }
}

void Disclosure(ID2D1DeviceContext* dc, D2D1_POINT_2F c, bool expanded) {
    auto ink = Solid(dc, Hex(0x595959));
    if (expanded)
        dc->FillGeometry(Polygon(dc, {{c.x + 3, c.y - 3}, {c.x + 3, c.y + 3}, {c.x - 3, c.y + 3}}).get(), ink.get());
    else
        dc->DrawGeometry(Polygon(dc, {{c.x - 2, c.y - 4}, {c.x + 2, c.y}, {c.x - 2, c.y + 4}}).get(),
                         Solid(dc, Hex(0xA6A6A6)).get(), 1.0f);
}

void PlayingMarker(ID2D1DeviceContext* dc, D2D1_POINT_2F c, bool paused) {
    auto blue = Solid(dc, Hex(0x1E64B4));
    if (paused) {
        dc->FillRectangle({c.x - 3.5f, c.y - 4, c.x - 1, c.y + 4}, blue.get());
        dc->FillRectangle({c.x + 1, c.y - 4, c.x + 3.5f, c.y + 4}, blue.get());
    } else {
        dc->FillGeometry(Polygon(dc, {{c.x - 3, c.y - 4.5f}, {c.x + 4, c.y}, {c.x - 3, c.y + 4.5f}}).get(),
                         blue.get());
    }
}

void ViewOptionsGlyph(ID2D1DeviceContext* dc, D2D1_SIZE_F size, media::ViewStyle style) {
    // Original icons: wmploc strip 331, 16x16 slots 55 details / 56 expanded tile / 57 icon.
    {
        const UINT32 slot = style == media::ViewStyle::Icons ? 57 : style == media::ViewStyle::Tiles ? 56 : 55;
        const float x = std::round((size.width - 16) / 2), y = std::round((size.height - 16) / 2);
        if (win7::DrawPartTo(dc, win7::Module::Wmploc, 331, D2D1::RectU(slot * 16, 0, slot * 16 + 16, 16),
                             {x, y, x + 16, y + 16}))
            return;
    }
    winrt::com_ptr<ID2D1SolidColorBrush> ink, light;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x4D6185), ink.put());
    dc->CreateSolidColorBrush(D2D1::ColorF(0x8FA3C0), light.put());
    float dpi = 96, dummy = 96;
    dc->GetDpi(&dpi, &dummy);
    const float px = 96.0f / dpi;  // one device pixel
    auto snap = [&](float v) { return std::round(v / px) * px; };
    const float x0 = snap(7), y0 = snap(size.height / 2 - 6);
    switch (style) {
    case media::ViewStyle::Icons:  // 2 x 2 tiles
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c)
                dc->FillRectangle({x0 + c * 7.0f, y0 + r * 7.0f, x0 + c * 7.0f + 5, y0 + r * 7.0f + 5}, ink.get());
        break;
    case media::ViewStyle::Tiles:  // tile with text lines beside it
        for (int r = 0; r < 2; ++r) {
            dc->FillRectangle({x0, y0 + r * 7.0f, x0 + 5, y0 + r * 7.0f + 5}, ink.get());
            dc->FillRectangle({x0 + 7, y0 + r * 7.0f + 1, x0 + 13, y0 + r * 7.0f + 2}, light.get());
            dc->FillRectangle({x0 + 7, y0 + r * 7.0f + 3, x0 + 11, y0 + r * 7.0f + 4}, light.get());
        }
        break;
    default:  // details: rows
        for (int r = 0; r < 4; ++r) {
            dc->FillRectangle({x0, y0 + r * 3.0f, x0 + 2, y0 + r * 3.0f + 2}, ink.get());
            dc->FillRectangle({x0 + 4, y0 + r * 3.0f, x0 + 13, y0 + r * 3.0f + 2}, light.get());
        }
        break;
    }
}
}  // namespace glasswave::wmp::paint
