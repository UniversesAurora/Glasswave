#include "pch.h"
#include "ui/list_view_parts.h"

// The list view's painting: the status band, the header, rows, cells, badges and groups, and
// the scroll bar.
namespace glasswave::ui {

using namespace listview;

namespace {

constexpr uint32_t kNoteInk = 0x8C8C8C; // a title's note: lighter than the secondary columns

constexpr uint32_t kBadgeInk = 0xE5484D; // a title's tag: NetEase's 原唱 red (owner capture, 2026-11-21)

void PlaceVisual(const wuc::Visual& v, float x, float y, float w, float h) {
    v.Offset(float3{x, y, 0});
    v.Size(float2{std::max(w, 0.0f), std::max(h, 0.0f)});
}

// A song card's play button: a triangle in Aero's blue (after wmploc's 16 px play arrow, 5346),
// rounded corners, a white rim and a soft shadow; brighter under the mouse.
// A song's download mark after Apple Music's: a 16 DIP disc with a white arrow of round strokes
// (a stem and an open head). `hot` (0..1, fading) is the mouse on it. Downloaded: Apple's grey
// disc, darker under the mouse. The offer to download (a hovered row) is a glass ball in the
// colours of WMP 12's play orb (wmploc 4153 at rest, 4155 lit, sampled down its middle): a deep
// navy body from a sharp horizon (#0C3E90) brightening to a cyan foot (#2BAAEA, #44CAFA), a pale
// gloss over the upper part, a navy rim. Lit it glows: the body turns azure (#0060B1 at the
// horizon), the foot nearly white cyan (#7AF0FC), the rim blue (#0372C0) and a blue halo
// (#23A4EF) shines round it, as the orb's does.
void DownloadMark(ID2D1DeviceContext* dc, D2D1_POINT_2F c, bool offer, float hot, float opacity) {
    if (opacity <= 0) return;
    const float r = 8;
    const auto colour = [&](uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a * opacity); };
    const auto gradient = [&](D2D1_POINT_2F from, D2D1_POINT_2F to,
                              std::initializer_list<std::pair<float, D2D1_COLOR_F>> stops) {
        std::vector<D2D1_GRADIENT_STOP> list;
        for (const auto& [at, value] : stops)
            list.push_back({at, value});
        winrt::com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(list.data(), static_cast<UINT32>(list.size()), collection.put());
        winrt::com_ptr<ID2D1LinearGradientBrush> brush;
        dc->CreateLinearGradientBrush({from, to}, collection.get(), brush.put());
        return brush;
    };
    const auto mix = [](uint32_t a, uint32_t b, float t) {
        const auto ch = [&](int shift) {
            const float x = ((a >> shift) & 0xFF) * (1 - t) + ((b >> shift) & 0xFF) * t;
            return static_cast<uint32_t>(std::lround(x)) << shift;
        };
        return ch(16) | ch(8) | ch(0);
    };
    const D2D1_ELLIPSE disc{c, r, r};
    if (offer) {
        // The halo of the lit orb, round the disc.
        if (hot > 0) {
            D2D1_GRADIENT_STOP haloStops[] = {{0.0f, colour(0x23A4EF, 0.0f)},
                                              {0.72f, colour(0x23A4EF, 0.55f * hot)},
                                              {1.0f, colour(0x23A4EF, 0.0f)}};
            winrt::com_ptr<ID2D1GradientStopCollection> haloList;
            dc->CreateGradientStopCollection(haloStops, 3, haloList.put());
            winrt::com_ptr<ID2D1RadialGradientBrush> halo;
            dc->CreateRadialGradientBrush({c, {0, 0}, r + 3, r + 3}, haloList.get(), halo.put());
            dc->FillEllipse({c, r + 3, r + 3}, halo.get());
        }
        // The body, top to foot.
        dc->FillEllipse(disc, gradient({c.x, c.y - r}, {c.x, c.y + r},
                                       {{0.0f, colour(mix(0x1A4E9E, 0x2A7CC4, hot))},
                                        {0.45f, colour(mix(0x0C3E90, 0x0060B1, hot))},
                                        {0.62f, colour(mix(0x0C55A3, 0x0A7ACA, hot))},
                                        {0.8f, colour(mix(0x0C85D8, 0x38ADE6, hot))},
                                        {0.9f, colour(mix(0x2BAAEA, 0x5FD1F7, hot))},
                                        {1.0f, colour(mix(0x44CAFA, 0x7AF0FC, hot))}})
                                  .get());
        // The foot's glow, near white when lit.
        D2D1_GRADIENT_STOP glowStops[] = {{0.0f, colour(mix(0x5FD6FF, 0xC8FFFF, hot), 0.55f + 0.35f * hot)},
                                          {1.0f, colour(0x5FD6FF, 0)}};
        winrt::com_ptr<ID2D1GradientStopCollection> glowList;
        dc->CreateGradientStopCollection(glowStops, 2, glowList.put());
        winrt::com_ptr<ID2D1RadialGradientBrush> glow;
        dc->CreateRadialGradientBrush({{c.x, c.y + r * 0.85f}, {0, 0}, r * 0.75f, r * 0.45f}, glowList.get(),
                                      glow.put());
        dc->FillEllipse(disc, glow.get());
        // The gloss: pale over the upper part, ending in a sharp horizon.
        dc->FillEllipse({{c.x, c.y - r * 0.5f}, r * 0.8f, r * 0.46f},
                        gradient({c.x, c.y - r * 0.96f}, {c.x, c.y - r * 0.04f},
                                 {{0.0f, colour(0xFFFFFF, 0.78f)}, {1.0f, colour(0xFFFFFF, 0.28f)}})
                            .get());
        winrt::com_ptr<ID2D1SolidColorBrush> rim;
        dc->CreateSolidColorBrush(colour(mix(0x0A3478, 0x0372C0, hot), 0.95f), rim.put());
        dc->DrawEllipse({c, r - 0.4f, r - 0.4f}, rim.get(), 0.8f);
    } else {
        winrt::com_ptr<ID2D1SolidColorBrush> grey;
        dc->CreateSolidColorBrush(colour(mix(0xC4C4C4, 0xA0A0A0, hot)), grey.put());
        dc->FillEllipse(disc, grey.get());
    }
    // The arrow: round strokes, a stem and an open head.
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, round.put());
    winrt::com_ptr<ID2D1PathGeometry> arrow;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(arrow.put());
    arrow->Open(sink.put());
    sink->BeginFigure({c.x, c.y - 4.2f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({c.x, c.y + 3.6f});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({c.x - 3.3f, c.y + 0.4f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({c.x, c.y + 3.7f});
    sink->AddLine({c.x + 3.3f, c.y + 0.4f});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    if (offer) { // a soft shadow lifts it off the glass
        D2D1_MATRIX_3X2_F current;
        dc->GetTransform(&current);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0, 0.6f) * current);
        winrt::com_ptr<ID2D1SolidColorBrush> shadow;
        dc->CreateSolidColorBrush(colour(0x061E4A, 0.45f), shadow.put());
        dc->DrawGeometry(arrow.get(), shadow.get(), 1.7f, round.get());
        dc->SetTransform(current);
    }
    winrt::com_ptr<ID2D1SolidColorBrush> white;
    dc->CreateSolidColorBrush(colour(0xFFFFFF), white.put());
    dc->DrawGeometry(arrow.get(), white.get(), 1.7f, round.get());
}

// A download in progress: a ring (16 DIP) filling clockwise from the top in Windows 7's progress
// colours (green, yellow paused, red failed) over a grey track; waiting: the track alone.
void ProgressRing(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float share, media::DownloadCell::Look state) {
    const float r = 6.5f, width = 3.0f;
    dc->DrawEllipse({c, r, r}, Brush(dc, 0xDADADA).get(), width);
    share = std::clamp(share, 0.0f, 1.0f);
    if (share <= 0) return;
    using Look = media::DownloadCell::Look;
    const uint32_t light = state == Look::Failed ? 0xF08080 : state == Look::Paused ? 0xFFE27A : 0x7EEB86;
    const uint32_t dark = state == Look::Failed ? 0xC81E1E : state == Look::Paused ? 0xD49B00 : 0x0AB526;
    D2D1_GRADIENT_STOP gradient[2] = {{0.0f, D2D1::ColorF(light)}, {1.0f, D2D1::ColorF(dark)}};
   winrt::com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(gradient, 2, collection.put());
   winrt::com_ptr<ID2D1LinearGradientBrush> ink;
    dc->CreateLinearGradientBrush({{c.x - r, c.y - r}, {c.x + r, c.y + r}}, collection.get(), ink.put());
    if (share >= 0.999f) {
        dc->DrawEllipse({c, r, r}, ink.get(), width);
        return;
    }
   winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
   winrt::com_ptr<ID2D1PathGeometry> arc;
   winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(arc.put());
    arc->Open(sink.put());
    const float angle = share * 6.2831853f;
    sink->BeginFigure({c.x, c.y - r}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc({{c.x + r * std::sin(angle), c.y - r * std::cos(angle)},
                  {r, r},
                  0,
                  D2D1_SWEEP_DIRECTION_CLOCKWISE,
                  share > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
   winrt::com_ptr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                               round.put());
    dc->DrawGeometry(arc.get(), ink.get(), width, round.get());
}

} // namespace

void ListView::PaintStatus() {
    const int h = StatusHeightPx();
    if (h <= 0) {
        m_status.IsVisible(false);
        return;
    }
    m_status.IsVisible(true);
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1);
    const float s = m_scale;
    host.Repaint(m_status, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        wp::HeaderBackground(dc, size);
        Text(dc, m_statusText, {kCellPad, 0, size.width - kCellPad, size.height}, kSecondaryInk,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    });
    PlaceVisual(m_status, 0, static_cast<float>(HeaderHeightPx()), static_cast<float>(w),
                static_cast<float>(h));
}

void ListView::PaintHeader() {
    if (m_mode == Mode::Tiles) {  // icon views have no column header
        m_header.IsVisible(false);
        return;
    }
    m_header.IsVisible(true);
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = HeaderHeightPx();
    const float s = m_scale;
    host.Repaint(m_header, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        // WMP 12 header cells (wmploc LISTVIEWHEADER: normal / hot / pressed, 23 px each);
        // drawn per cell, so the cell edges form the separators.
        static const std::wstring kHeader = L"LISTVIEWHEADER.PNG";
        const bool art = win7::Resources::Get().Has(win7::Module::Wmploc, kHeader);
        auto cell = [&](float x0, float x1, int state) {
            return win7::DrawNineGrid(dc, win7::Module::Wmploc, kHeader, {0, 23.0f * state, 6, 23.0f * state + 23},
                                      {1, 1, 1, 1}, {x0, 0, x1, size.height});
        };
        if (!art || !cell(0, size.width, 0)) wp::HeaderBackground(dc, size);
        if (!m_source) return;
        const auto& srcCols = m_source->Columns();
        const auto cols = Columns(RowsWidth());
        // Cells in content coordinates, shifted by the horizontal scroll.
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-ScrollX(), 0) * base);
        const float dragX = (m_dragX - m_bounds.left) / s + ScrollX();
        // One header cell (background, title, sort chevron) with its left edge at `x`.
        auto drawCell = [&](const Col& c, float x, int state) {
            if (art) cell(x, x + c.w, state);
            else if (state) wp::RowPlate(dc, {x, 1, x + c.w, size.height - 1}, wp::RowState::Hot, true);
            const auto& col = srcCols[c.src];
            if (KindOf(col) != CellKind::Art)  // the cover column has no title, only covers
                Text(dc, Tr(col.title), {x + kCellPad, 0, x + c.w - kCellPad, size.height}, 0x4C607A,
                     col.rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            if (!art) wp::HeaderSeparator(dc, x + c.w, size.height);
            if (c.src == m_source->SortColumn()) {  // sort chevron at the top center, as in Win7
                winrt::com_ptr<ID2D1Factory> factory;
                dc->GetFactory(factory.put());
                winrt::com_ptr<ID2D1PathGeometry> tri;
                winrt::com_ptr<ID2D1GeometrySink> sink;
                factory->CreatePathGeometry(tri.put());
                tri->Open(sink.put());
                const float cx = std::round((x + c.w / 2) * s) / s, top = 1.5f, bottom = 5.0f;
                const bool down = m_source->SortDescending();
                sink->BeginFigure({cx - 3.5f, down ? top : bottom}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({cx + 3.5f, down ? top : bottom});
                sink->AddLine({cx, down ? bottom : top});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(tri.get(), Brush(dc, 0x7A96B8).get());
            }
        };
        // Pressed (and dragged) cells use the sunken state, the hovered one the hot state;
        // both fill the whole cell like WMP's.
        for (size_t d = 0; d < cols.size(); ++d) {
            const bool pressed = (m_drag == Drag::PendingMove || m_drag == Drag::Move) && d == m_dragCol;
            drawCell(cols[d], cols[d].x, pressed ? 2 : (d == m_hotHeader && m_drag == Drag::None) ? 1 : 0);
        }
        if (m_drag == Drag::Move && m_dragCol < cols.size()) {
            // Insertion mark where the column would land.
            float mark = cols.empty() ? kGutter : cols.back().x + cols.back().w;
            for (const Col& c : cols)
                if (dragX < c.x + c.w / 2) {
                    mark = c.x;
                    break;
                }
            const float line = gfx::Hairline(s);
            dc->FillRectangle({mark - line, 0, mark + line, size.height}, Brush(dc, 0x3C7FB1).get());
            // A translucent copy of the dragged header follows the cursor, as in WMP.
            const Col& c = cols[m_dragCol];
            const float x = std::round((c.x + (m_dragX - m_dragStartX) / s) * s) / s;
            winrt::com_ptr<ID2D1Layer> layer;
            dc->CreateLayer(nullptr, layer.put());
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                D2D1::IdentityMatrix(), 0.6f),
                          layer.get());
            drawCell(c, x, 0);
            dc->PopLayer();
        }
        dc->SetTransform(base);
    });
    PlaceVisual(m_header, 0, 0, static_cast<float>(w), static_cast<float>(h));
}

void ListView::PaintRows() {
    auto& host = gfx::CompositionHost::Get();
    if (!m_slideKey.empty() && ContentTopDip() != m_slideTop) { // scrolled meanwhile: the slide ends
        m_slideKey.clear();
        m_slide.StopAnimation(L"Offset");
        m_slideClip.IsVisible(false);
    }
    const RECT rr = RowsRect();
    const int w = std::max<int>(rr.right - rr.left, 1), viewPx = std::max<int>(rr.bottom - rr.top, 1);
    PlaceVisual(m_viewport, 0, static_cast<float>(ContentOffsetPx()), static_cast<float>(w),
                static_cast<float>(viewPx));
    // A jump elsewhere (a row brought into view, the thumb, new rows) ends a glide.
    if (m_glide.ms && (!Gliding() || std::abs(ContentTopDip() - m_glide.to) > 0.01f)) {
        m_glide.ms = 0;
        m_content.StopAnimation(L"Offset");
    }
    if (!Gliding()) {
        // The glide this thread counts as over may still run its last frame in the compositor
        // (or a jump left it): it stops here, at the place it was going to.
        m_glide.ms = 0;
        m_content.StopAnimation(L"Offset");
        m_paintTop = ContentTopDip();
        m_paintHeight = viewPx / m_scale;
        m_content.Offset(float3{0, -std::round(m_paintTop * m_scale), 0});
    }
    const int h = std::max(1, static_cast<int>(std::ceil(m_paintHeight * m_scale)));
    // Painted as if the view stood at the painted range's top and were as tall.
    const size_t realTop = m_top;
    const float realOffset = m_topOffsetDip;
    ScrollTopTo(m_paintTop);
    m_paintViewH = h / m_scale;
    // Two surfaces drawn in turn: the one on screen is never drawn into. Drawn in place, its new
    // rows (laid out for a new glide's start) could reach the screen a frame before the glide's
    // new offset, the content standing a screen off for that frame (owner, 2026-11-21: what is
    // further down flashed up over the home cards while scrolling up and down). The brush swap,
    // like the offset, is a property: both land in the same frame. A surface only grows (in
    // half views) while the view keeps its size: sized to each glide, every wheel step made one.
    RowsBuffer& back = m_rowsBuffers[m_rowsFront ^ 1];
    if (!back.surface || back.size.cx != w || back.size.cy < h || back.viewPx != viewPx) {
        const int half = std::max(1, viewPx / 2);
        const int surfaceH = (h + half - 1) / half * half;
        back.surface = host.CreateSurface(w, surfaceH);
        back.size = {w, surfaceH};
        back.viewPx = viewPx;
        back.brush = host.Compositor().CreateSurfaceBrush(back.surface);
        back.brush.Stretch(wuc::CompositionStretch::None);
        back.brush.HorizontalAlignmentRatio(0);
        back.brush.VerticalAlignmentRatio(0);
    }
    // The painted range stands at its own place in the (absolute) content.
    const float paintPx = std::round(m_paintTop * m_scale);
    PlaceVisual(m_rows, 0, paintPx, static_cast<float>(w), static_cast<float>(h));
    m_cards->SetBounds({0, static_cast<LONG>(paintPx), w, static_cast<LONG>(paintPx) + h}, m_scale);
    const float s = m_scale;
    m_glowSeen = false;
    const bool drawn = host.Draw(back.surface, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        if (!m_source) return;
        if (m_source->Count() == 0) {
            if (m_statusText.empty())
                Text(dc, Tr(m_source->EmptyText()), {20, 20, size.width - 20, 60}, kGreyInk,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-ScrollX(), 0) * base);
        switch (m_mode) {
        case Mode::Grouped: PaintGrouped(dc, size); break;
        case Mode::Tiles: PaintTiles(dc); break;
        default: PaintPlain(dc, size); break;
        }
        if (m_mode != Mode::Tiles) {
            // WMP 12 column lines, down to the bottom of the view (owner capture at 150 %): on
            // each column's last pixel a translucent blue (#75B5F4 at 15 %), a faint white
            // line just left of it.
            const float line = gfx::Hairline(s);
            auto blue = Brush(dc, 0x75B5F4, 0.15f), white = Brush(dc, 0xFFFFFF, 0.2f);
            for (const Col& c : Columns(size.width)) {
                const float x = std::round((c.x + c.w) * s) / s;
                dc->FillRectangle({x - line, 0, x, size.height}, blue.get());
                dc->FillRectangle({x - 2 * line, 0, x - line, size.height}, white.get());
            }
        }
        dc->SetTransform(base);
        if (m_drag == Drag::Marquee) {
            // Win7 rubber band: #3399FF border over a translucent fill.
            const float top = ContentTopDip();
            const float px = 1.0f / s;
            auto snap = [&](float v) { return std::round(v * s) / s; };
            const D2D1_RECT_F r{snap(std::min(m_marqueeFrom.x, m_marqueeTo.x) - ScrollX()),
                                snap(std::min(m_marqueeFrom.y, m_marqueeTo.y) - top),
                                snap(std::max(m_marqueeFrom.x, m_marqueeTo.x) - ScrollX()),
                                snap(std::max(m_marqueeFrom.y, m_marqueeTo.y) - top)};
            dc->FillRectangle(r, Brush(dc, 0x3399FF, 0.25f).get());
            dc->DrawRectangle({r.left + px / 2, r.top + px / 2, r.right - px / 2, r.bottom - px / 2},
                              Brush(dc, 0x3399FF).get(), px);
        }
    });
    if (drawn) { // shown with the offset set in this same message
        m_rows.Brush(back.brush);
        m_rowsFront ^= 1;
    }
    NoteGlow();
    LayoutCards();
    m_paintViewH = -1;
    m_top = realTop;
    m_topOffsetDip = realOffset;
    PlaceThumb();
}

void ListView::PaintCells(ID2D1DeviceContext* dc, size_t row, float y, float rowH, const std::vector<Col>& cols) {
    const bool isPlaying = m_source->IsPlaying(row);
    const auto& srcCols = m_source->Columns();
    const bool skipAlbum = AlbumColumn();
    if (isPlaying) {
        // In the gutter: before the first column, or between the album column and the tracks;
        // the same distance from the row highlight's left edge in both layouts.
        const float x = skipAlbum && !cols.empty() ? cols[0].x + cols[0].w + kGutter / 2 - 1 : kGutter / 2 + 1;
        wp::PlayingMarker(dc, {x, y + rowH / 2 + 0.5f}, playback_control::get()->is_paused());
    }
    for (const Col& c : cols) {
        if (skipAlbum && c.src == 0) continue;
        const auto& col = srcCols[c.src];
        const CellKind kind = KindOf(col);
        if (kind == CellKind::Rating) {
            // WMP stars (STARS.PNG 13x16 cells: 0-4 empty, 5-9 gold, 15-19 hover gold,
            // 25-29 white on a selected row); hovering previews the rating under the mouse.
            const int rating = _wtoi(m_source->Cell(row, c.src).c_str());
            const bool hover = row == m_hot && m_hotStar >= 0, sel = m_source->IsSelected(row);
            const float sy = std::round((y + (rowH - 16) / 2) * m_scale) / m_scale;
            bool drawn = true;
            for (int i = 0; i < 5 && drawn; ++i) {
                const UINT32 cell = hover ? (i <= m_hotStar ? 15 + i : i) : (i < rating ? (sel ? 25 : 5) + i : i);
                const float sx = std::round((c.x + kCellPad + i * kStar) * m_scale) / m_scale;
                drawn = win7::DrawPartTo(dc, win7::Module::Wmploc, L"STARS.PNG",
                                         D2D1::RectU(cell * 13, 0, cell * 13 + 13, 16), {sx, sy, sx + kStar, sy + 16});
            }
            if (drawn) continue;
        } else if (kind == CellKind::Art) {
            // The song's front cover, as tall as the row allows, centred in the column.
            const float side = rowH - 4, ax = std::round((c.x + (c.w - side) / 2) * m_scale) / m_scale;
            if (const metadb_handle_ptr item = m_source->Item(row); item.is_valid()) {
                const metadb_handle_ptr art = m_source->ArtTrack(row);
                DrawArt(dc, m_source->ArtKey(row), art.is_valid() ? art : item, {ax, y + 2, ax + side, y + 2 + side});
            }
            continue;
        } else if (kind == CellKind::Favorite) {
            // No heart in WMP's art: a gold star (5530) when the song is in Favorites, its
            // outline (5531) on the hovered row, else the empty one (5533).
            const bool fav = m_source->IsFavorite(row);
            const int id = fav ? 5530 : row == m_hot ? 5531 : 5533;
            const float fx = std::round((c.x + (c.w - 14) / 2) * m_scale) / m_scale;
            const float fy = std::round((y + (rowH - 14) / 2) * m_scale) / m_scale;
            if (win7::Draw(dc, win7::Module::Wmploc, id, {fx, fy})) continue;
            if (fav) Text(dc, L"★", {c.x + kCellPad, y + 0.5f, c.x + c.w - kCellPad, y + 0.5f + rowH}, 0xD4A017,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
            continue;
        } else if (kind == CellKind::Popularity) {
            // NetEase's 热度: a short rounded bar, the score's share of the column, on a
            // track in the list's group-rule tint. No bar for an unscored song.
            const int score = std::clamp(_wtoi(m_source->Cell(row, c.src).c_str()), 0, 100);
            if (score == 0) continue;
            auto snap = [&](float v) { return gfx::Snap(v, m_scale); };
            const float left = snap(c.x + kCellPad), right = snap(c.x + c.w - kCellPad);
            const float h = 4, top = snap(y + (rowH - h) / 2);
            if (right - left < h * 2) continue;
            const float fill = std::max(h, snap((right - left) * score / 100.0f));
            dc->FillRoundedRectangle({{left, top, right, top + h}, h / 2, h / 2}, Brush(dc, kGroupRule).get());
            dc->FillRoundedRectangle({{left, top, left + fill, top + h}, h / 2, h / 2},
                                     Brush(dc, isPlaying ? kPlayingInk : 0x3C7FB1).get());
            continue;
        } else if (kind == CellKind::Download) {
            // Downloaded: the mark always; downloading: its ring; else the offer on the hovered row.
            const media::DownloadCell cell = m_source->Download(row);
            const D2D1_POINT_2F centre{std::round((c.x + c.w / 2) * m_scale) / m_scale,
                                       std::round((y + rowH / 2) * m_scale) / m_scale};
            switch (cell.look) {
            case media::DownloadCell::Look::None: break;
            case media::DownloadCell::Look::Offer:
                if (row == m_hot) DownloadMark(dc, centre, true, m_downloadHot, 1.0f); // at once with the row
                break;
            case media::DownloadCell::Look::Done:
                DownloadMark(dc, centre, false, row == m_hot ? m_downloadHot : 0.0f, 1.0f);
                break;
            default: ProgressRing(dc, centre, cell.progress, cell.look); break;
            }
            continue;
        } else if (kind == CellKind::Progress) {
            // Windows 7's progress bar at its own 14 DIP height: the grey trough, then the
            // fill's share of it, green (yellow paused, red failed), cut off at the share. A
            // running one ('r') has Windows 7's glow (aero 664, the move overlay) sweeping from
            // the bar's start to the fill's end: 1.7 s across, 0.7 s rest.
            const std::wstring cell = m_source->Cell(row, c.src);
            if (cell.empty()) continue;
            const float share = std::clamp(static_cast<float>(_wtoi(cell.c_str())) / 1000.0f, 0.0f, 1.0f);
            const wchar_t state = cell.back();
            auto snap = [&](float v) { return gfx::Snap(v, m_scale); };
            const float left = snap(c.x + kCellPad), right = snap(c.x + c.w - kCellPad);
            const float h = 14, top = snap(y + (rowH - h) / 2);
            if (right - left < 20) continue;
            const D2D1_RECT_F bar{left, top, right, top + h};
            win7::DrawNineGrid(dc, win7::Module::Aero, 648, {0, 0, 19, 14}, {3, 3, 3, 3}, bar);
            const float fill = snap((right - left) * share);
            if (fill > 0) {
                const int id = state == L'e' ? 657 : state == L'p' ? 658 : 656;
                dc->PushAxisAlignedClip({left, top, left + fill, top + h}, D2D1_ANTIALIAS_MODE_ALIASED);
                win7::DrawNineGrid(dc, win7::Module::Aero, id, {0, 0, 51, 14}, {7, 3, 7, 3},
                                   {left, top, left + std::max(fill, 14.0f), top + h});
                if (state == L'r') {
                    m_glowSeen = true;
                    constexpr ULONGLONG kSweep = 1700, kCycle = 2400;
                    const ULONGLONG t = GetTickCount64() % kCycle;
                    if (t < kSweep) {
                        const float glow = 127.0f * h / 18.0f; // the overlay at the bar's height
                        const float x = left - glow + (fill + glow) * static_cast<float>(t) / kSweep;
                        win7::DrawPartTo(dc, win7::Module::Aero, 664, {0, 0, 127, 18}, {x, top, x + glow, top + h});
                    }
                }
                dc->PopAxisAlignedClip();
            }
            continue;
        }
        const bool primary = col.flex;
        D2D1_RECT_F box{c.x + kCellPad, y + 0.5f, c.x + c.w - kCellPad, y + 0.5f + rowH};
        const std::wstring text = m_source->Cell(row, c.src);
        // Tags stand before the text, as NetEase puts 原唱 before the singer (owner, 2026-11-21).
        if (!col.rightAlign) box.left = PaintBadges(dc, m_source->CellBadges(row, c.src), box.left, y + rowH / 2, box.right);
        const DWRITE_FONT_WEIGHT weight =
            isPlaying && primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
        Text(dc, text, box, isPlaying ? kPlayingInk : (primary ? kInk : kSecondaryInk),
             col.rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING, weight);
        // A note follows the text in grey, "(note)", where the text leaves room for it.
        if (const std::wstring note = m_source->CellNote(row, c.src); !note.empty() && !col.rightAlign) {
            auto layout = MakeText(text, 12.0f, weight, box.right - box.left, rowH);
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            const float x = box.left + m.widthIncludingTrailingWhitespace + 4;
            if (x + 12 < box.right) Text(dc, L"(" + note + L")", {x, box.top, box.right, box.bottom}, kNoteInk);
        }
    }
}

// A tag, as NetEase draws 原唱: its text in red inside a thin red rounded outline, a little
// smaller than the row's text, centred on `cy`. Returns its width; 0 when it does not fit before
// `right`.
float ListView::PaintBadge(ID2D1DeviceContext* dc, const std::wstring& text, float x, float cy, float right) {
    return DrawTag(dc, text, x, cy, right, Hex(kBadgeInk), 10.0f, m_scale);
}

// Tags (untranslated keys) from `x` on; returns where the text after them starts.
float ListView::PaintBadges(ID2D1DeviceContext* dc, const std::vector<std::wstring>& badges, float x, float cy,
                            float right) {
    if (!settings::SongTagsShown()) return x; // Preferences: tags such as 原唱 off
    for (const auto& badge : badges) {
        const float w = PaintBadge(dc, Tr(badge), x, cy, right);
        if (w <= 0) break;
        x += w + 5;
    }
    return x;
}

void ListView::PaintPlain(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const size_t n = m_source->Count();
    const auto cols = Columns(size.width);
    const size_t focus = m_source->Focus();
    const size_t visible = VisibleLines() + 1;
    const float rowH = RowHeightPx() / m_scale;  // whole device pixels, matches hit testing
    const float right = ContentWidth();  // rows end with the last column
    for (size_t i = 0; i < visible && m_top + i < n; ++i) {
        const size_t row = m_top + i;
        const float y = i * rowH - m_topOffsetDip;
        const bool sel = m_source->IsSelected(row), hot = row == m_hot;
        const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                    : (hot ? wp::RowState::Hot : wp::RowState::Normal);
        wp::RowPlate(dc, {2, y + 1, right - 2, y + rowH}, st, m_focused);
        if (m_focused && row == focus && !sel)
            dc->DrawRoundedRectangle({{2.5f, y + 1.5f, right - 2.5f, y + rowH - 0.5f}, 2, 2},
                                     Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
        PaintCells(dc, row, y, rowH, cols);
    }
}

void ListView::PaintGrouped(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const auto& groups = *m_source->Groups();
    const auto cols = Columns(size.width);
    const bool art = AlbumColumn() && !cols.empty();
    const float albumL = art ? cols[0].x : kGutter, albumR = art ? cols[0].x + cols[0].w : 2;
    const size_t focus = m_source->Focus();
    const float right = ContentWidth();              // rows end with the last column
    const float viewRight = ScrollX() + size.width;  // group rules run to the view's edge
    // WMP 12: the group title starts a little inside the album column (7 px at 150 %).
    const float titleX = art ? albumL + 8 : kGutter + 2;

    std::vector<size_t> shownGroups;
    float y = -m_topOffsetDip;
    for (size_t i = 0; m_top + i < m_lines.size() && y < size.height; ++i) {
        const Line& line = m_lines[m_top + i];
        const float rowH = LineHeightDip(line);
        if (shownGroups.empty() || shownGroups.back() != line.group) shownGroups.push_back(line.group);
        if (line.kind == Line::GroupHeader) {
            // Album artist in blue, followed by a hairline to the right edge.
            const auto& g = groups[line.group];
            auto layout = MakeText(g.header, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, std::max(40.0f, viewRight - titleX - 20), rowH);
            dc->DrawTextLayout({titleX, y + 1}, layout.get(), Brush(dc, kGroupInk).get());
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const float lx = titleX + tm.widthIncludingTrailingWhitespace + 6;
            const float line1 = gfx::Hairline(m_scale);
            const float ly = std::round((y + rowH / 2 + 1) * m_scale) / m_scale;
            if (lx < viewRight - 6)
                dc->FillRectangle({lx, ly, viewRight - 6, ly + line1}, Brush(dc, kGroupRule).get());
            // Hovering the bar shows only its outline, as WMP does (no selection fill).
            if (line.group == m_hotGroupHeader) {
                const float px = gfx::Hairline(m_scale);
                dc->DrawRoundedRectangle({{2.5f, y + 1.5f, viewRight - 2.5f, y + rowH - 0.5f}, 2, 2},
                                         Brush(dc, 0x7DA2CE, 0.6f).get(), px);
            }
            y += rowH;
            continue;
        }
        if (line.kind != Line::Item) {
            y += rowH;
            continue;
        }
        const size_t row = line.row;
        const bool sel = m_source->IsSelected(row), hot = row == m_hot;
        const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                    : (hot ? wp::RowState::Hot : wp::RowState::Normal);
        wp::RowPlate(dc, {albumR, y + 1, right - 2, y + rowH}, st, m_focused);
        if (m_focused && row == focus && !sel)
            dc->DrawRoundedRectangle({{albumR + 0.5f, y + 1.5f, right - 2.5f, y + rowH - 0.5f}, 2, 2},
                                     Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
        PaintCells(dc, row, y, rowH, cols);
        y += rowH;
    }
    if (!art) return;

    // Album column: cover plus album / artist / genre / year beside it, once per group.
    const float artSize = ArtSize();
    const float albumRowH = RowHeightPx() / m_scale;  // album column only exists in uniform tiles
    for (size_t gi : shownGroups) {
        const auto& g = groups[gi];
        const float top = LineTopDip(m_groupLine[gi] + 1) - ContentTopDip();
        // Only the cover and its four lines belong to the album cell; below them (long
        // albums) the column is empty space, e.g. to start a rubber band from.
        const size_t lines = kGroupMinLines;
        bool allSelected = g.count > 0;
        for (size_t k = 0; k < g.count && allSelected; ++k) allSelected = m_source->IsSelected(g.first + k);
        if (allSelected || gi == m_hotGroup) {
            const wp::RowState st = allSelected ? (gi == m_hotGroup ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                                : wp::RowState::Hot;
            wp::RowPlate(dc, {albumL, top + 1, albumR - 2, top + lines * albumRowH}, st, m_focused);
        }
        DrawArt(dc, m_source->ArtKey(g.first), g.art, {albumL + 3, top + 4, albumL + 3 + artSize, top + 4 + artSize});
        const float tx = AlbumTextLeft(albumL);
        for (size_t k = 0; k < g.lines.size(); ++k) {
            const bool hotLink = gi == m_hotLinkGroup && k == m_hotLinkLine;
            Text(dc, g.lines[k], {tx, top + k * albumRowH + 0.5f, albumR - 4, top + (k + 1) * albumRowH + 0.5f},
                 hotLink ? kLinkInk : (k == 0 ? kInk : kSecondaryInk), DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_FONT_WEIGHT_NORMAL, hotLink);
        }
    }
}

// The bar measures in pixels (its arrows 17 DIP, its thumb at least 24), the content in DIP:
// the thumb is the view's share of the content, wherever the tall rows are.
ScrollBarModel::Geometry ListView::BarGeometry() const {
    const RECT rr = RowsRect();
    return {static_cast<float>(std::max<LONG>(rr.bottom - rr.top, 0)), kScrollArrow * m_scale, 24.0f * m_scale,
            ContentHeightDip(), RowsHeightDip(), ContentTopDip()};
}

ScrollBarModel::Part ListView::BarPart(Part part) {
    switch (part) {
    case Part::ArrowUp: return ScrollBarModel::kUp;
    case Part::ArrowDown: return ScrollBarModel::kDown;
    case Part::Track: return ScrollBarModel::kTrack;
    case Part::Thumb: return ScrollBarModel::kThumb;
    default: return ScrollBarModel::kNone;
    }
}

bool ListView::ThumbGeometry(float* top, float* height) const {
    return ScrollBarModel::Thumb(BarGeometry(), top, height);
}

float ListView::ScrollPage() const { return std::max(RowsHeightDip() - 24.0f, 24.0f); }

bool ListView::AnimateScrollBar() {
    bool changed = false;
    bool active = m_scrollBar.Animate(&changed);
    if (changed) PaintScrollBar();
    // Shelves slide to their page (ease-out cubic); play buttons fade.
    bool rows = false;
    const ULONGLONG now = GetTickCount64();
    if (!m_slideKey.empty()) {
        if (now >= m_slideEnd)
            FinishSlide();
        else
            active = true;
    }
    const auto step = [&](float& value, float target, float up, float down) {
        const float next = value < target ? std::min(target, value + up) : std::max(target, value - down);
        if (next == value) return;
        value = next;
        rows = true;
        if (value != target) active = true;
    };
    step(m_playBlend, m_playRow != SIZE_MAX ? 1.0f : 0.0f, 0.2f, 0.2f);
    step(m_playOldBlend, 0.0f, 0.15f, 0.15f);
    step(m_playHotBlend, m_playHot ? 1.0f : 0.0f, 0.25f, 0.12f);
    step(m_shelfPlate, m_hotShelf != SIZE_MAX ? 1.0f : 0.0f, 0.2f, 0.2f);
    step(m_downloadHot, m_hotDownload ? 1.0f : 0.0f, 0.15f, 0.08f);
    step(m_shelfPress,
         m_shelfPressedGroup != SIZE_MAX && m_shelfPressedGroup == m_hotShelf && m_shelfPressedSide == m_hotShelfSide
             ? 1.0f
             : 0.0f,
         0.35f, 0.15f);
    step(m_moreHot, m_moreGroup != SIZE_MAX ? 1.0f : 0.0f, 0.2f, 0.1f);
    step(m_moreOldHot, 0.0f, 0.1f, 0.1f);
    step(m_morePress, m_morePressedGroup != SIZE_MAX && m_morePressedGroup == m_moreGroup ? 1.0f : 0.0f, 0.35f, 0.15f);
    if (m_source && m_source->Groups())
        for (const auto& g : *m_source->Groups())
            if (g.carousel) step(m_shelfShown[g.key], g.key == m_shelfUnder ? 1.0f : 0.0f, 0.15f, 0.1f);
    if (m_playOldBlend == 0) m_playOldRow = SIZE_MAX;
    if (m_glowing) { // about 30 frames a second for the glow
        active = true;
        if (now - m_glowPainted >= 33) {
            m_glowPainted = now;
            rows = true;
        }
    }
    if (rows) PaintRows();
    return active;
}

bool ListView::OnScrollRepeat() {
    const float step = m_scrollBar.Repeat(BarGeometry(), kScrollStep, ScrollPage());
    if (step == 0) return false; // an end, or the thumb under the pointer
    ScrollBy(step);
    return true;
}

void ListView::PaintScrollBar() {
    auto& host = gfx::CompositionHost::Get();
    const RECT rr = RowsRect();
    const int w = ScrollWidthPx(), h = std::max<int>(rr.bottom - rr.top, 1);
    float top = 0, height = 0;
    const bool has = ThumbGeometry(&top, &height);
    const float s = m_scale;
    host.Repaint(m_scroll, w, h, s, [&](ID2D1DeviceContext* dc) {
        wp::ScrollBar(dc, {w / s, h / s}, 0, 0, m_scrollBar.Blend()); // the track and the arrows
    });
    PlaceVisual(m_scroll, static_cast<float>(rr.right - m_bounds.left), static_cast<float>(ContentOffsetPx()),
                static_cast<float>(w), static_cast<float>(h));
    m_thumb.IsVisible(has);
    if (has) {
        const int th = std::max(1, static_cast<int>(std::lround(height)));
        host.Repaint(m_thumb, w, th, s, [&](ID2D1DeviceContext* dc) {
            wp::ScrollBar(dc, {w / s, th / s}, 0, th / s, m_scrollBar.Blend(), true);
        });
        m_thumb.Size(float2{static_cast<float>(w), static_cast<float>(th)});
    }
    PlaceThumb();
}

void ListView::PlaceThumb() {
    float top = 0, height = 0;
    if (!ThumbGeometry(&top, &height)) return;
    const RECT rr = RowsRect();
    const float arrow = kScrollArrow * m_scale;
    const float trackH = static_cast<float>(std::max<LONG>(rr.bottom - rr.top, 0)) - arrow * 2;
    const float max = MaxScrollDip() * m_scale;
    const float lo = static_cast<float>(ContentOffsetPx()) + arrow;
    m_thumbPlace.InsertScalar(L"x", static_cast<float>(rr.right - m_bounds.left));
    m_thumbPlace.InsertScalar(L"a", lo);
    m_thumbPlace.InsertScalar(L"b", max > 0 ? (trackH - std::round(height)) / max : 0.0f);
    m_thumbPlace.InsertScalar(L"top", 0.0f); // content coordinates are absolute
    m_thumbPlace.InsertScalar(L"lo", lo);
    m_thumbPlace.InsertScalar(L"hi", lo + std::max(0.0f, trackH - std::round(height)));
}

} // namespace glasswave::ui
