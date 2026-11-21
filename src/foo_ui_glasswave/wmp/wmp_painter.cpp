#include "pch.h"
#include "wmp/wmp_painter.h"
#include "ui/text.h"
#include "ui/i18n.h"
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
    const D2D1_RECT_F rc{Snap(dc, r.left), Snap(dc, r.top), Snap(dc, r.right), Snap(dc, r.bottom)};
    // WMP 12 draws Win7's list selection: aero.msstyles Explorer::ListView item (premultiplied,
    // 7x74, sizing 3): 602 hot, 601 selected, 603 selected + hot, 604 selected without focus
    // (grey). Matches the owner's capture: border #83ACDD, #E8F2FE to #CEE4FE inside.
    {
        const int id = state == RowState::Hot           ? 602
                       : !focused                        ? 604
                       : state == RowState::SelectedHot ? 603
                                                         : 601;
        if (win7::DrawNineGrid(dc, win7::Module::Aero, id, {0, 0, 7, 74}, {3, 3, 3, 3}, rc)) return;
    }
    const float L = Hairline(dc);
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

void ScrollBar(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float thumbTop, float thumbHeight,
               const float* partBlends) {
    const float L = Hairline(dc);
    const float W = size.width, H = size.height;
    const float arrow = std::min(17.0f, H / 2);
    D2D1_MATRIX_3X2_F transform{};
    dc->GetTransform(&transform);
    const float scale = std::max(0.1f, transform._11);
    using win7::Module;
    auto& art = win7::Resources::Get();
    const bool aero = art.Has(Module::Aero, 728);
    auto blendOf = [&](ScrollBarPart part) {
        return partBlends ? std::clamp(partBlends[static_cast<int>(part)], 0.0f, 2.0f) : 0.0f;
    };
    auto drawTrack = [&](int state, float opacity) {
        const D2D1_RECT_F trackRect{0, arrow, W, H - arrow};
        if (aero) {
            // IMAGE 728 is 14x1 per state with fixed 10 px left / 3 px right margins; the
            // theme nine-grids it to the 17/21/26 px scrollbar width.
            win7::DrawNineGrid(dc, Module::Aero, 728,
                               {0.0f, static_cast<float>(state), 14.0f, static_cast<float>(state + 1)},
                               {10.0f, 0.0f, 3.0f, 0.0f}, trackRect, opacity);
        } else {
            dc->FillRectangle({0, 0, W, H}, Solid(dc, Hex(state == 2 ? 0xE0E2E3 : 0xF2F6FA, opacity)).get());
            dc->FillRectangle({0, 0, L, H}, Solid(dc, Hex(0xC9D5E1, opacity)).get());
            dc->FillRectangle({L, 0, W, H},
                              Vertical(dc, 0, H,
                                       {{0.0f, Hex(state == 2 ? 0xE0E2E3 : 0xF7FAFC, opacity)},
                                        {1.0f, Hex(state == 2 ? 0xD8DADC : 0xE5EDF5, opacity)}})
                                  .get());
        }
    };
    drawTrack(0, 1.0f);
    const float trackBlend = blendOf(ScrollBarPart::Track);
    if (trackBlend > 0.0f) drawTrack(1, std::min(trackBlend, 1.0f));
    if (trackBlend > 1.0f) drawTrack(2, trackBlend - 1.0f);

    const int glyphDpi = scale <= 1.0f ? 96 : scale <= 1.25f ? 120 : 144;
    const int glyphSize = glyphDpi == 96 ? 13 : glyphDpi == 120 ? 16 : 20;
    const int glyphId = glyphDpi == 96 ? 718 : glyphDpi == 120 ? 719 : 720;
    auto drawPlate = [&](bool down, int state, float opacity) {
        const D2D1_RECT_F r{0, down ? H - arrow : 0, W, down ? H : arrow};
        if (aero) {
            // IMAGE 717: 20 states of 17x17; states 1-4 up, 5-8 down (normal/hot/pressed/
            // disabled). The 8 px border is fixed, as the theme nine-grids it.
            const int index = (down ? 4 : 0) + state;
            win7::DrawNineGrid(dc, Module::Aero, 717,
                               {0.0f, static_cast<float>(index * 17), 17.0f,
                                static_cast<float>((index + 1) * 17)},
                               {8.0f, 8.0f, 8.0f, 8.0f}, r, opacity);
        } else {
            const bool press = state == 2;
            const bool hotPart = state == 1;
            dc->FillRectangle(r, Vertical(dc, r.top, r.bottom,
                                          {{0.0f, Hex(press ? 0xD9ECFB : hotPart ? 0xF2FAFF : 0xFBFDFF, opacity)},
                                           {1.0f, Hex(press ? 0xBFDCF2 : hotPart ? 0xD3E9FA : 0xE6EEF6, opacity)}})
                                      .get());
            dc->DrawRectangle({r.left + L / 2, r.top + L / 2, r.right - L / 2, r.bottom - L / 2},
                              Solid(dc, Hex(press ? 0x5E8CB5 : hotPart ? 0x7FA8CB : 0xC0CCD8, opacity)).get(), L);
        }
        const int index = (down ? 4 : 0) + state;
        const float dstSize = glyphSize / scale;
        const D2D1_RECT_F glyph{(W - dstSize) / 2, (r.top + r.bottom) / 2 - dstSize / 2,
                                (W + dstSize) / 2, (r.top + r.bottom) / 2 + dstSize / 2};
        if (!art.Has(Module::Aero, glyphId) ||
            !win7::DrawPartTo(dc, Module::Aero, glyphId,
                              {0, static_cast<UINT>(index * glyphSize), static_cast<UINT>(glyphSize),
                               static_cast<UINT>((index + 1) * glyphSize)},
                              glyph, opacity)) {
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            const bool up = !down;
            const float d = 3.5f;
            dc->FillGeometry(Polygon(dc, {{cx - d, cy + (up ? 1.5f : -1.5f)},
                                          {cx + d, cy + (up ? 1.5f : -1.5f)},
                                          {cx, cy + (up ? -2.0f : 2.0f)}})
                                     .get(),
                             Solid(dc, Hex(state ? 0x1E395B : 0x4A5563, opacity)).get());
        }
    };
    for (bool down : {false, true}) {
        drawPlate(down, 0, 1.0f);
        const float b = blendOf(down ? ScrollBarPart::ArrowDown : ScrollBarPart::ArrowUp);
        if (b > 0.0f) drawPlate(down, 1, std::min(b, 1.0f));
        if (b > 1.0f) drawPlate(down, 2, b - 1.0f);
    }
    if (thumbHeight <= 0) return;

    const D2D1_RECT_F t{0, std::round(thumbTop), W, std::round(thumbTop + thumbHeight)};
    const int gripDpi = scale <= 1.0f ? 96 : scale <= 1.25f ? 120 : 144;
    const int gripW = gripDpi == 96 ? 9 : gripDpi == 120 ? 11 : 14;
    const int gripH = gripDpi == 96 ? 10 : gripDpi == 120 ? 12 : 16;
    const int gripId = gripDpi == 96 ? 724 : gripDpi == 120 ? 725 : 726;
    auto drawThumb = [&](int state, float opacity) {
        if (aero) {
            // IMAGE 733: 5 states of 17x22; 8 px side borders, 5 px top/bottom caps.
            const float sy = static_cast<float>(state * 22);
            if (thumbHeight <= 10) {
                win7::DrawPartTo(dc, Module::Aero, 733,
                                 {0, static_cast<UINT>(sy), 17, static_cast<UINT>(sy + 22)}, t, opacity);
            } else {
                win7::DrawNineGrid(dc, Module::Aero, 733, {0.0f, sy, 17.0f, sy + 22.0f},
                                   {8.0f, 5.0f, 8.0f, 5.0f}, t, opacity);
            }
        } else {
            const D2D1_RECT_F f{2, t.top, W - 2, t.bottom};
            const D2D1_ROUNDED_RECT rr{Inset(f, L / 2), 2.5f, 2.5f};
            dc->FillRoundedRectangle(
                rr, Vertical(dc, f.top, f.bottom,
                             {{0.0f, Hex(state == 2 ? 0xD2EDFC : state == 1 ? 0xEAF8FF : 0xF7FBFE, opacity)},
                              {1.0f, Hex(state == 2 ? 0x9FD2EF : state == 1 ? 0xBEE4FA : 0xDDEAF6, opacity)}})
                        .get());
            dc->DrawRoundedRectangle(rr, Solid(dc, Hex(state == 2 ? 0x2D6A96
                                                         : state == 1 ? 0x3C7FB1
                                                                       : 0x9DB4CA,
                                                     opacity))
                                             .get(), L);
        }
        if (thumbHeight > 22) {
            const float gw = gripW / scale, gh = gripH / scale;
            const D2D1_RECT_F grip{(W - gw) / 2, (t.top + t.bottom) / 2 - gh / 2,
                                   (W + gw) / 2, (t.top + t.bottom) / 2 + gh / 2};
            if (!art.Has(Module::Aero, gripId) ||
                !win7::DrawPartTo(dc, Module::Aero, gripId,
                                  {0, static_cast<UINT>(state * gripH), static_cast<UINT>(gripW),
                                   static_cast<UINT>((state + 1) * gripH)},
                                  grip, opacity)) {
                const float cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
                for (int i = -1; i <= 1; ++i) {
                    const float y = Snap(dc, cy + i * 3.0f);
                    dc->FillRectangle({cx - 3.5f, y, cx + 3.5f, y + L},
                                      Solid(dc, Hex(state == 2 ? 0x2B5A85 : 0x5A7E9F,
                                                    0.8f * opacity)).get());
                }
            }
        }
    };
    drawThumb(0, 1.0f);
    const float thumbBlend = blendOf(ScrollBarPart::Thumb);
    if (thumbBlend > 0.0f) drawThumb(1, std::min(thumbBlend, 1.0f));
    if (thumbBlend > 1.0f) drawThumb(2, thumbBlend - 1.0f);
}

// Small black drop-down triangle, drawn as a vector so it stays sharp at any DPI (the 14x20
// WW_SPLIT bitmap went soft when scaled).
void DropArrow(ID2D1DeviceContext* dc, float cx, float cy) {
    cx = Snap(dc, cx);
    dc->FillGeometry(Polygon(dc, {{cx - 3.5f, cy - 1.5f}, {cx + 3.5f, cy - 1.5f}, {cx, cy + 2.0f}}).get(),
                     Solid(dc, Hex(0x1E395B)).get());
}

void CommandButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, std::wstring_view text, bool dropDown) {
    const float L = Hairline(dc);
    const D2D1_RECT_F rc{0, 0, Snap(dc, size.width), Snap(dc, size.height)};
    using win7::Module;
    const int state = layer == Layer::Pressed ? 2 : layer == Layer::Hover ? 1 : 0;
    // WMP 12's command bar buttons are Win7 toolbar buttons, neutral and glossy, not Explorer's
    // bluer CommandModule: aero.msstyles Toolbar BUTTON 849 (premultiplied; 8 states of 7x26:
    // normal, hot, pressed, disabled, checked, hot checked, near hot, other side hot). Matches
    // the owner's capture: a #A1A8B0 border, a white highlight on top, a darker lower half.
    // The arrow is a vector (DropArrow).
    const bool art = win7::Resources::Get().Has(Module::Aero, 849);
    if (art && state > 0)
        win7::DrawNineGrid(dc, Module::Aero, 849, {0, 26.0f * state, 7, 26.0f * state + 26}, {3, 3, 3, 3}, rc);
    if (text.empty() && dropDown) {
        DropArrow(dc, size.width / 2, size.height / 2 + 0.5f);
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
    auto layout = ui::MakeText(ui::Tr(std::wstring(text)), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, size.width - 12 - arrowW, size.height);
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
    case NavIcon::Disc: // an audio CD: a silver disc with its clear hub
        dc->FillEllipse({c, 6.5f, 6.5f}, light.get());
        dc->DrawEllipse({c, 6.0f, 6.0f}, blue.get(), 1.0f);
        dc->DrawEllipse({c, 2.5f, 2.5f}, blue.get(), 1.0f);
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

void Disclosure(ID2D1DeviceContext* dc, D2D1_POINT_2F c, bool expanded, bool hot) {
    // Win7 tree view glyph as vectors: expanded is a filled corner triangle with a darker edge
    // than its centre, collapsed an open triangle; hovering turns both cyan-blue.
    const float px = Hairline(dc);
    auto shape = expanded ? Polygon(dc, {{c.x + 3, c.y - 3}, {c.x + 3, c.y + 3}, {c.x - 3, c.y + 3}})
                          : Polygon(dc, {{c.x - 2, c.y - 4}, {c.x + 2, c.y}, {c.x - 2, c.y + 4}});
    const uint32_t fill = expanded ? (hot ? 0x82DFFB : 0x595959) : 0xFFFFFF;
    const uint32_t edge = hot ? 0x1CC4F7 : expanded ? 0x262626 : 0xA6A6A6;
    dc->FillGeometry(shape.get(), Solid(dc, Hex(fill)).get());
    dc->DrawGeometry(shape.get(), Solid(dc, Hex(edge)).get(), px);
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

bool SplitButtonPlate(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, float splitX, int part, bool menuOpen) {
    // Win7 toolbar split button as in WMP 12: aero.msstyles Toolbar SPLITBUTTON (852, left half)
    // and SPLITBUTTONDROPDOWN (853, right half) join into one button with a single divider;
    // 8 states of 26 px like BUTTON (849). The half under the mouse is hot (1), the other
    // "other side hot" (7); pressing shows the pressed half (2); with the menu open the arrow
    // is pressed and the main half state 7.
    using win7::Module;
    if (!win7::Resources::Get().Has(Module::Aero, 852) || !win7::Resources::Get().Has(Module::Aero, 853)) return false;
    int mainState = 0, splitState = 0;
    if (layer == Layer::Hover) {
        mainState = part == 0 ? 1 : 7;
        splitState = part == 0 ? 7 : 1;
    } else if (layer == Layer::Pressed) {
        const bool arrow = menuOpen || part == 1;
        mainState = arrow ? 7 : 2;
        splitState = arrow ? 2 : 7;
    }
    const float h = Snap(dc, size.height), x = Snap(dc, splitX), w = Snap(dc, size.width);
    if (mainState)
        win7::DrawNineGrid(dc, Module::Aero, 852, {0, 26.0f * mainState, 6, 26.0f * mainState + 26}, {2, 3, 2, 3},
                           {0, 0, x, h});
    if (splitState)
        win7::DrawNineGrid(dc, Module::Aero, 853, {0, 26.0f * splitState, 5, 26.0f * splitState + 26}, {1, 3, 2, 3},
                           {x, 0, w, h});
    DropArrow(dc, (x + w) / 2, h / 2 + 0.5f);  // centred in the right part
    return true;
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
