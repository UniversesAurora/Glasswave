#include "pch.h"
#include "ui/list_view.h"
#include "gfx/composition_host.h"
#include "ui/art_cache.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "wmp/wmp_painter.h"

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
namespace wp = wmp::paint;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {
constexpr float kRowHeight = 22.0f;
constexpr float kHeaderHeight = 25.0f;
constexpr float kScrollWidth = 14.0f;
constexpr float kGutter = 18.0f;  // playing marker column
constexpr float kCellPad = 6.0f;
constexpr size_t kGroupMinLines = 4;  // the album column needs four lines (cover + 4 text lines)
constexpr float kTilePad = 8.0f;
constexpr D2D1_SIZE_F kAlbumTile{132, 160};
constexpr float kAlbumTileArt = 100;
constexpr D2D1_SIZE_F kArtistTile{240, 108};
constexpr float kArtistTileArt = 88;

constexpr uint32_t kInk = 0x1E1E1E, kSecondaryInk = 0x4A4A4A, kGreyInk = 0x7A7A7A, kPlayingInk = 0x1E5BA8;
constexpr uint32_t kGroupInk = 0x2E64B4, kGroupRule = 0xC9D7E8;

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

winrt::com_ptr<ID2D1SolidColorBrush> Brush(ID2D1DeviceContext* dc, uint32_t rgb, float a = 1.0f) {
    winrt::com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(Hex(rgb, a), b.put());
    return b;
}

void PlaceVisual(const wuc::Visual& v, float x, float y, float w, float h) {
    v.Offset(float3{x, y, 0});
    v.Size(float2{std::max(w, 0.0f), std::max(h, 0.0f)});
}

void Text(ID2D1DeviceContext* dc, const std::wstring& text, D2D1_RECT_F box, uint32_t ink,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
          DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
    if (text.empty() || box.right - box.left < 4) return;
    auto layout = MakeText(text, 12.0f, weight, box.right - box.left, box.bottom - box.top, align);
    dc->DrawTextLayout({box.left, box.top}, layout.get(), Brush(dc, ink).get());
}
}  // namespace

ListView::ListView(const wuc::ContainerVisual& parent) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    m_header = comp.CreateSpriteVisual();
    m_rows = comp.CreateSpriteVisual();
    m_scroll = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_rows);
    m_root.Children().InsertAtTop(m_header);
    m_root.Children().InsertAtTop(m_scroll);
    parent.Children().InsertAtTop(m_root);
    m_artToken = ArtCache::Get().Subscribe([this] {
        if (m_mode != Mode::Plain) PaintRows();
    });
}

ListView::~ListView() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
}

// ---- Geometry -------------------------------------------------------------------------------

int ListView::RowHeightPx() const { return static_cast<int>(std::lround(kRowHeight * m_scale)); }
int ListView::HeaderHeightPx() const { return static_cast<int>(std::lround(kHeaderHeight * m_scale)); }
int ListView::ScrollWidthPx() const { return static_cast<int>(std::lround(kScrollWidth * m_scale)); }

int ListView::LineHeightPx() const {
    if (m_mode == Mode::Tiles) return static_cast<int>(std::lround(TileSize().height * m_scale));
    return RowHeightPx();
}

D2D1_SIZE_F ListView::TileSize() const {
    return m_source && m_source->Tiles() == media::TileStyle::Artist ? kArtistTile : kAlbumTile;
}

size_t ListView::TileColumns() const {
    const RECT r = RowsRect();
    const float w = (r.right - r.left) / m_scale - 2 * kTilePad;
    return static_cast<size_t>(std::max(1.0f, std::floor(w / TileSize().width)));
}

RECT ListView::RowsRect() const {
    return {m_bounds.left, m_bounds.top + HeaderHeightPx(), m_bounds.right - ScrollWidthPx(), m_bounds.bottom};
}

size_t ListView::LineCount() const {
    const size_t n = m_source ? m_source->Count() : 0;
    switch (m_mode) {
    case Mode::Grouped: return m_lines.size();
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        return (n + cols - 1) / cols;
    }
    default: return n;
    }
}

size_t ListView::LineOfRow(size_t row) const {
    switch (m_mode) {
    case Mode::Grouped: return row < m_rowLine.size() ? m_rowLine[row] : 0;
    case Mode::Tiles: return row / TileColumns();
    default: return row;
    }
}

size_t ListView::VisibleLines() const {
    const RECT r = RowsRect();
    return static_cast<size_t>(std::max<LONG>(r.bottom - r.top, 0) / std::max(LineHeightPx(), 1));
}

size_t ListView::MaxTop() const {
    const size_t n = LineCount(), v = VisibleLines();
    return n > v ? n - v : 0;
}

std::vector<float> ListView::ColumnWidths(float total) const {
    std::vector<float> widths;
    if (!m_source) return widths;
    const auto& cols = m_source->Columns();
    float fixed = kGutter;
    for (const auto& c : cols)
        if (!c.flex) fixed += c.width;
    const float flex = std::max(120.0f, total - fixed);
    for (const auto& c : cols) widths.push_back(c.flex ? flex : c.width);
    return widths;
}

// ---- Data -----------------------------------------------------------------------------------

void ListView::BuildLines() {
    m_lines.clear();
    m_rowLine.clear();
    m_groupLine.clear();
    m_mode = Mode::Plain;
    if (!m_source) return;
    if (m_source->Tiles() != media::TileStyle::None) {
        m_mode = Mode::Tiles;
        return;
    }
    const auto* groups = m_source->Groups();
    if (!groups || groups->empty()) return;
    m_mode = Mode::Grouped;
    m_rowLine.assign(m_source->Count(), 0);
    for (size_t gi = 0; gi < groups->size(); ++gi) {
        const auto& g = (*groups)[gi];
        m_groupLine.push_back(m_lines.size());
        m_lines.push_back({Line::GroupHeader, SIZE_MAX, gi, 0});
        const size_t lines = std::max(g.count, kGroupMinLines);
        for (size_t k = 0; k < lines; ++k) {
            if (k < g.count) {
                m_rowLine[g.first + k] = m_lines.size();
                m_lines.push_back({Line::Item, g.first + k, gi, k});
            } else {
                m_lines.push_back({Line::Filler, SIZE_MAX, gi, k});
            }
        }
    }
}

void ListView::SetSource(media::ListSource* source, bool resetScroll) {
    m_source = source;
    m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    m_anchor = source ? source->Focus() : SIZE_MAX;
    if (resetScroll) m_top = 0;
    BuildLines();
    PaintHeader();
    Refresh();
}

void ListView::SetBounds(const RECT& px, float scale) {
    const bool scaleChanged = scale != m_scale;
    const bool sizeChanged = (px.right - px.left) != (m_bounds.right - m_bounds.left) ||
                             (px.bottom - px.top) != (m_bounds.bottom - m_bounds.top);
    m_bounds = px;
    m_scale = scale;
    const float w = static_cast<float>(px.right - px.left), h = static_cast<float>(px.bottom - px.top);
    m_root.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    m_root.Size(float2{w, h});
    if (sizeChanged || scaleChanged) {
        PaintHeader();
        Refresh();
    }
}

void ListView::SetFocused(bool focused) {
    if (m_focused == focused) return;
    m_focused = focused;
    PaintRows();
}

void ListView::Refresh() {
    BuildLines();  // rows or groups may have changed (filter, playlist edits)
    m_top = std::min(m_top, MaxTop());
    if (m_source && m_hot != SIZE_MAX && m_hot >= m_source->Count()) m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    PaintRows();
    PaintScrollBar();
}

void ListView::ScrollTo(size_t top) {
    top = std::min(top, MaxTop());
    if (top == m_top) return;
    m_top = top;
    PaintRows();
    PaintScrollBar();
}

void ListView::EnsureVisible(size_t row) {
    size_t line = LineOfRow(row);
    // Bring a group's header into view together with its first track.
    if (m_mode == Mode::Grouped && line > 0 && m_lines[line - 1].kind == Line::GroupHeader) --line;
    const size_t v = std::max<size_t>(VisibleLines(), 1);
    const size_t last = LineOfRow(row);
    if (line < m_top)
        ScrollTo(line);
    else if (last >= m_top + v)
        ScrollTo(last - v + 1);
}

// ---- Painting -------------------------------------------------------------------------------

void ListView::PaintHeader() {
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = HeaderHeightPx();
    const float s = m_scale;
    host.Repaint(m_header, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        wp::HeaderBackground(dc, size);
        if (!m_source) return;
        const auto widths = ColumnWidths(size.width - kScrollWidth);
        float x = kGutter;
        const auto& cols = m_source->Columns();
        for (size_t i = 0; i < cols.size(); ++i) {
            Text(dc, cols[i].title, {x + kCellPad, 0, x + widths[i] - kCellPad, size.height}, 0x4C607A,
                 cols[i].rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            x += widths[i];
            if (!cols[i].flex || !cols[i].title.empty()) wp::HeaderSeparator(dc, x, size.height);
        }
    });
    PlaceVisual(m_header, 0, 0, static_cast<float>(w), static_cast<float>(h));
}

void ListView::PaintRows() {
    auto& host = gfx::CompositionHost::Get();
    const RECT rr = RowsRect();
    const int w = std::max<int>(rr.right - rr.left, 1), h = std::max<int>(rr.bottom - rr.top, 1);
    if (!m_rowsSurface || m_rowsSurfaceSize.cx != w || m_rowsSurfaceSize.cy != h) {
        m_rowsSurface = host.CreateSurface(w, h);
        m_rowsSurfaceSize = {w, h};
        auto brush = host.Compositor().CreateSurfaceBrush(m_rowsSurface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        m_rows.Brush(brush);
    }
    PlaceVisual(m_rows, 0, static_cast<float>(HeaderHeightPx()), static_cast<float>(w), static_cast<float>(h));
    const float s = m_scale;
    host.Draw(m_rowsSurface, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        if (!m_source) return;
        if (m_source->Count() == 0) {
            Text(dc, m_source->EmptyText(), {20, 20, size.width - 20, 60}, kGreyInk, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        switch (m_mode) {
        case Mode::Grouped: PaintGrouped(dc, size); break;
        case Mode::Tiles: PaintTiles(dc, size); break;
        default: PaintPlain(dc, size); break;
        }
    });
}

void ListView::PaintCells(ID2D1DeviceContext* dc, size_t row, float y, float rowH, const std::vector<float>& widths,
                          size_t firstColumn) {
    const bool isPlaying = m_source->IsPlaying(row);
    const auto& cols = m_source->Columns();
    float x = kGutter;
    for (size_t c = 0; c < firstColumn && c < widths.size(); ++c) x += widths[c];
    if (isPlaying) {
        const bool paused = playback_control::get()->is_paused();
        // In the gutter, or at the left of the "#" column when the album column comes first.
        wp::PlayingMarker(dc, {firstColumn ? x + 5 : kGutter / 2 + 1, y + rowH / 2 + 0.5f}, paused);
    }
    for (size_t c = firstColumn; c < cols.size(); ++c) {
        const std::wstring text = m_source->Cell(row, c);
        const bool primary = cols[c].flex;
        Text(dc, text, {x + kCellPad, y + 0.5f, x + widths[c] - kCellPad, y + 0.5f + rowH},
             isPlaying ? kPlayingInk : (primary ? kInk : kSecondaryInk),
             cols[c].rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING,
             isPlaying && primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        x += widths[c];
    }
}

void ListView::PaintPlain(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const size_t n = m_source->Count();
    const auto widths = ColumnWidths(size.width);
    const size_t focus = m_source->Focus();
    const size_t visible = VisibleLines() + 1;
    const float rowH = RowHeightPx() / m_scale;  // whole device pixels, matches hit testing
    for (size_t i = 0; i < visible && m_top + i < n; ++i) {
        const size_t row = m_top + i;
        const float y = i * rowH;
        const bool sel = m_source->IsSelected(row), hot = row == m_hot;
        const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                    : (hot ? wp::RowState::Hot : wp::RowState::Normal);
        wp::RowPlate(dc, {2, y + 1, size.width - 2, y + rowH}, st, m_focused);
        if (m_focused && row == focus && !sel)
            dc->DrawRoundedRectangle({{2.5f, y + 1.5f, size.width - 2.5f, y + rowH - 0.5f}, 2, 2},
                                     Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
        PaintCells(dc, row, y, rowH, widths, 0);
    }
}

void ListView::PaintGrouped(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const auto& groups = *m_source->Groups();
    const auto widths = ColumnWidths(size.width);
    const float albumL = kGutter, albumR = kGutter + (widths.empty() ? 0 : widths[0]);
    const size_t focus = m_source->Focus();
    const size_t visible = VisibleLines() + 1;
    const float rowH = RowHeightPx() / m_scale;

    std::vector<size_t> shownGroups;
    for (size_t i = 0; i < visible && m_top + i < m_lines.size(); ++i) {
        const Line& line = m_lines[m_top + i];
        const float y = i * rowH;
        if (shownGroups.empty() || shownGroups.back() != line.group) shownGroups.push_back(line.group);
        if (line.kind == Line::GroupHeader) {
            // Album artist in blue, followed by a hairline to the right edge.
            const auto& g = groups[line.group];
            auto layout = MakeText(g.header, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, size.width - albumL - 20, rowH);
            dc->DrawTextLayout({albumL + 2, y + 1}, layout.get(), Brush(dc, kGroupInk).get());
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const float lx = albumL + 2 + tm.widthIncludingTrailingWhitespace + 6;
            const float line1 = 1.0f / m_scale * std::floor(m_scale);
            const float ly = std::round((y + rowH / 2 + 1) * m_scale) / m_scale;
            if (lx < size.width - 6)
                dc->FillRectangle({lx, ly, size.width - 6, ly + line1}, Brush(dc, kGroupRule).get());
            continue;
        }
        if (line.kind != Line::Item) continue;
        const size_t row = line.row;
        const bool sel = m_source->IsSelected(row), hot = row == m_hot;
        const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                    : (hot ? wp::RowState::Hot : wp::RowState::Normal);
        wp::RowPlate(dc, {albumR, y + 1, size.width - 2, y + rowH}, st, m_focused);
        if (m_focused && row == focus && !sel)
            dc->DrawRoundedRectangle({{albumR + 0.5f, y + 1.5f, size.width - 2.5f, y + rowH - 0.5f}, 2, 2},
                                     Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
        PaintCells(dc, row, y, rowH, widths, 1);
    }

    // Album column: cover plus album / artist / genre / year beside it, once per group.
    const float art = kGroupMinLines * rowH - 8;
    for (size_t gi : shownGroups) {
        const auto& g = groups[gi];
        const float top = (static_cast<float>(m_groupLine[gi]) + 1 - static_cast<float>(m_top)) * rowH;
        const size_t lines = std::max(g.count, kGroupMinLines);
        bool allSelected = g.count > 0;
        for (size_t k = 0; k < g.count && allSelected; ++k) allSelected = m_source->IsSelected(g.first + k);
        if (allSelected || gi == m_hotGroup) {
            const wp::RowState st = allSelected ? (gi == m_hotGroup ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                                : wp::RowState::Hot;
            wp::RowPlate(dc, {albumL, top + 1, albumR - 2, top + lines * rowH}, st, m_focused);
        }
        DrawArt(dc, g.art, {albumL + 3, top + 4, albumL + 3 + art, top + 4 + art});
        const float tx = albumL + 3 + art + 8;
        for (size_t k = 0; k < g.lines.size(); ++k)
            Text(dc, g.lines[k], {tx, top + k * rowH + 0.5f, albumR - 4, top + (k + 1) * rowH + 0.5f},
                 k == 0 ? kInk : kSecondaryInk);
    }
}

void ListView::PaintTiles(ID2D1DeviceContext* dc, D2D1_SIZE_F /*size*/) {
    const size_t n = m_source->Count();
    const size_t cols = TileColumns();
    const D2D1_SIZE_F ts = TileSize();
    const bool artist = m_source->Tiles() == media::TileStyle::Artist;
    const size_t focus = m_source->Focus();
    const float lineH = LineHeightPx() / m_scale;
    const size_t visible = VisibleLines() + 1;
    for (size_t i = 0; i < visible; ++i) {
        const float y = i * lineH;
        for (size_t c = 0; c < cols; ++c) {
            const size_t idx = (m_top + i) * cols + c;
            if (idx >= n) return;
            const float x = kTilePad + c * ts.width;
            const D2D1_RECT_F cell{x + 2, y + 3, x + ts.width - 2, y + ts.height - 3};
            const bool sel = m_source->IsSelected(idx), hot = idx == m_hot;
            if (sel || hot) {
                const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                            : wp::RowState::Hot;
                wp::RowPlate(dc, cell, st, m_focused);
            } else if (m_focused && idx == focus) {
                dc->DrawRoundedRectangle({{cell.left + 0.5f, cell.top + 0.5f, cell.right - 0.5f, cell.bottom - 0.5f}, 2, 2},
                                         Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
            }
            const media::TileInfo info = m_source->Tile(idx);
            if (artist) {
                const float a = kArtistTileArt, ay = y + (ts.height - a) / 2;
                DrawArt(dc, info.art, {x + 10, ay, x + 10 + a, ay + a});
                const float tx = x + 10 + a + 12;
                for (size_t k = 0; k < info.lines.size(); ++k)
                    Text(dc, info.lines[k], {tx, ay + 2 + k * 18, x + ts.width - 6, ay + 2 + (k + 1) * 18},
                         k == 0 ? kInk : kGreyInk);
            } else {
                const float a = kAlbumTileArt, ax = x + (ts.width - a) / 2, ay = y + 10;
                DrawArt(dc, info.art, {ax, ay, ax + a, ay + a});
                for (size_t k = 0; k < 2; ++k)
                    Text(dc, info.lines[k], {x + 6, ay + a + 5 + k * 17, x + ts.width - 6, ay + a + 5 + (k + 1) * 17},
                         k == 0 ? kInk : kGreyInk, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }
}

void ListView::DrawArt(ID2D1DeviceContext* dc, const metadb_handle_ptr& track, D2D1_RECT_F box) {
    const float side = box.right - box.left;
    winrt::com_ptr<IWICBitmapSource> src;
    const auto state = ArtCache::Get().Find(track, static_cast<int>(std::lround(side * m_scale)), &src);
    if (state == ArtCache::State::Pending) return;  // painted when it arrives

    D2D1_RECT_F dst = box;
    winrt::com_ptr<ID2D1Bitmap> bmp;
    if (src && SUCCEEDED(dc->CreateBitmapFromWicBitmap(src.get(), nullptr, bmp.put()))) {
        UINT bw = 0, bh = 0;
        src->GetSize(&bw, &bh);
        if (bw && bh && bw != bh) {  // keep the aspect ratio, bottom-aligned like WMP covers
            const float k = side / std::max(bw, bh);
            const float w = bw * k, h = bh * k;
            dst = {box.left + (side - w) / 2, box.bottom - h, box.left + (side + w) / 2, box.bottom};
        }
    }
    // WMP's soft drop shadow (wmploc ALBUMDROPSHADOW: a blurred square with an 8/200 margin).
    const float m = (dst.right - dst.left) * 8.0f / 184.0f;
    win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUMDROPSHADOW_200X200_PNG",
                        {dst.left - m + 1, dst.top - m + 2, dst.right + m + 1, dst.bottom + m + 2}, 0.45f);
    if (bmp) {
        dc->DrawBitmap(bmp.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    } else if (!win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUM_0_PNG", dst)) {
        dc->FillRectangle(dst, Brush(dc, 0xDCE4EE).get());  // no artwork files: plain placeholder
    }
    const float px = 1.0f / m_scale;
    dc->DrawRectangle({dst.left + px / 2, dst.top + px / 2, dst.right - px / 2, dst.bottom - px / 2},
                      Brush(dc, 0x000000, 0.18f).get(), px);
}

bool ListView::ThumbGeometry(float* top, float* height) const {
    const size_t n = LineCount(), v = VisibleLines();
    const float trackH = static_cast<float>(std::max<LONG>(RowsRect().bottom - RowsRect().top, 0));
    if (n <= v || trackH <= 0) return false;
    const float minH = 24.0f * m_scale;
    *height = std::max(minH, trackH * v / n);
    *top = (trackH - *height) * m_top / std::max<size_t>(MaxTop(), 1);
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
        wp::ScrollBar(dc, {w / s, h / s}, top / s, has ? height / s : 0, m_thumbHot || m_dragThumb);
    });
    PlaceVisual(m_scroll, static_cast<float>(rr.right - m_bounds.left), static_cast<float>(HeaderHeightPx()),
                static_cast<float>(w), static_cast<float>(h));
}

// ---- Input ----------------------------------------------------------------------------------

ListView::Part ListView::PartAt(POINT pt, size_t* row, size_t* group) const {
    if (!PtInRect(&m_bounds, pt)) return Part::None;
    if (pt.y < m_bounds.top + HeaderHeightPx()) return Part::Header;
    const RECT rr = RowsRect();
    if (pt.x >= rr.right) {
        float top = 0, height = 0;
        if (ThumbGeometry(&top, &height)) {
            const float y = static_cast<float>(pt.y - rr.top);
            if (y >= top && y < top + height) return Part::Thumb;
        }
        return Part::Track;
    }
    if (!m_source) return Part::Empty;
    const size_t line = m_top + static_cast<size_t>((pt.y - rr.top) / std::max(LineHeightPx(), 1));
    const float x = (pt.x - rr.left) / m_scale;
    switch (m_mode) {
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        if (x < kTilePad) return Part::Empty;
        const size_t col = static_cast<size_t>((x - kTilePad) / TileSize().width);
        const size_t idx = line * cols + col;
        if (col >= cols || idx >= m_source->Count()) return Part::Empty;
        if (row) *row = idx;
        return Part::Row;
    }
    case Mode::Grouped: {
        if (line >= m_lines.size()) return Part::Empty;
        const Line& l = m_lines[line];
        const auto widths = ColumnWidths((rr.right - rr.left) / m_scale);
        const float albumR = kGutter + (widths.empty() ? 0 : widths[0]);
        if (l.kind != Line::GroupHeader && x >= kGutter && x < albumR) {
            if (group) *group = l.group;
            return Part::AlbumColumn;
        }
        if (l.kind != Line::Item) return Part::Empty;
        if (row) *row = l.row;
        return Part::Row;
    }
    default:
        if (line < m_source->Count()) {
            if (row) *row = line;
            return Part::Row;
        }
        return Part::Empty;
    }
}

void ListView::OnMouseMove(POINT pt) {
    if (m_dragThumb) {
        const RECT rr = RowsRect();
        float top = 0, height = 0;
        if (ThumbGeometry(&top, &height)) {
            const float trackH = static_cast<float>(rr.bottom - rr.top) - height;
            const float y = static_cast<float>(pt.y - rr.top - m_dragOffset);
            const float frac = trackH > 0 ? std::clamp(y / trackH, 0.0f, 1.0f) : 0.0f;
            ScrollTo(static_cast<size_t>(std::lround(frac * MaxTop())));
        }
        return;
    }
    size_t row = SIZE_MAX, group = SIZE_MAX;
    const Part part = PartAt(pt, &row, &group);
    const size_t hot = part == Part::Row ? row : SIZE_MAX;
    const size_t hotGroup = part == Part::AlbumColumn ? group : SIZE_MAX;
    const bool thumbHot = part == Part::Thumb;
    if (hot != m_hot || hotGroup != m_hotGroup) {
        m_hot = hot;
        m_hotGroup = hotGroup;
        PaintRows();
    }
    if (thumbHot != m_thumbHot) {
        m_thumbHot = thumbHot;
        PaintScrollBar();
    }
}

void ListView::OnMouseLeave() {
    if (m_dragThumb) return;
    if (m_hot != SIZE_MAX || m_hotGroup != SIZE_MAX) {
        m_hot = SIZE_MAX;
        m_hotGroup = SIZE_MAX;
        PaintRows();
    }
    if (m_thumbHot) {
        m_thumbHot = false;
        PaintScrollBar();
    }
}

void ListView::OnLButtonDown(POINT pt, bool ctrl, bool shift) {
    size_t row = SIZE_MAX, group = SIZE_MAX;
    switch (PartAt(pt, &row, &group)) {
    case Part::Thumb: {
        float top = 0, height = 0;
        ThumbGeometry(&top, &height);
        m_dragThumb = true;
        m_dragOffset = static_cast<int>(pt.y - RowsRect().top - top);
        PaintScrollBar();
        break;
    }
    case Part::Track: {
        float top = 0, height = 0;
        if (ThumbGeometry(&top, &height)) {
            const size_t page = std::max<size_t>(VisibleLines(), 1) - 1;
            if (pt.y - RowsRect().top < top)
                ScrollTo(m_top > page ? m_top - page : 0);
            else
                ScrollTo(m_top + page);
        }
        break;
    }
    case Part::Row:
        MoveFocus(row, ctrl, shift);
        break;
    case Part::AlbumColumn: {  // WMP: clicking the album cell selects the whole album
        const auto& g = (*m_source->Groups())[group];
        if (g.count == 0) break;
        m_source->SelectRange(g.first, g.first + g.count - 1, ctrl);
        m_source->SetFocus(g.first);
        m_anchor = g.first;
        PaintRows();
        break;
    }
    default:
        break;
    }
}

void ListView::OnLButtonUp(POINT) {
    if (m_dragThumb) {
        m_dragThumb = false;
        PaintScrollBar();
    }
}

void ListView::OnDoubleClick(POINT pt) {
    size_t row = SIZE_MAX, group = SIZE_MAX;
    const Part part = PartAt(pt, &row, &group);
    if (part == Part::AlbumColumn && m_source) {
        const auto& g = (*m_source->Groups())[group];
        if (g.count) m_source->Activate(g.first);
        PaintRows();
    } else if (part == Part::Row && m_source) {
        m_source->Activate(row);
        PaintRows();
    }
}

void ListView::OnRButtonUp(HWND owner, POINT pt) {
    if (!m_source) return;
    size_t row = SIZE_MAX, group = SIZE_MAX;
    const Part part = PartAt(pt, &row, &group);
    if (part == Part::Row && !m_source->IsSelected(row)) {
        m_source->SelectOnly(row);
        m_source->SetFocus(row);
        m_anchor = row;
        PaintRows();
    } else if (part == Part::AlbumColumn) {
        const auto& g = (*m_source->Groups())[group];
        if (g.count) {
            m_source->SelectRange(g.first, g.first + g.count - 1, false);
            m_source->SetFocus(g.first);
            m_anchor = g.first;
            PaintRows();
        }
    }
    POINT screen = pt;
    ClientToScreen(owner, &screen);
    m_source->ContextMenu(owner, screen);
    Refresh();
}

void ListView::OnWheel(int delta) {
    const int step = m_mode == Mode::Tiles ? 1 : 3;
    const int lines = step * (-delta / WHEEL_DELTA);
    if (lines < 0)
        ScrollTo(m_top > static_cast<size_t>(-lines) ? m_top + lines : 0);
    else
        ScrollTo(m_top + lines);
}

void ListView::MoveFocus(size_t row, bool ctrl, bool shift) {
    if (!m_source || row >= m_source->Count()) return;
    if (shift && m_anchor != SIZE_MAX && m_anchor < m_source->Count()) {
        m_source->SelectRange(m_anchor, row, ctrl);
    } else if (ctrl) {
        m_source->Toggle(row);
        m_anchor = row;
    } else {
        m_source->SelectOnly(row);
        m_anchor = row;
    }
    m_source->SetFocus(row);
    EnsureVisible(row);
    PaintRows();
}

bool ListView::OnKeyDown(UINT vk, bool ctrl, bool shift) {
    if (!m_source) return false;
    const size_t n = m_source->Count();
    const size_t focus = m_source->Focus();
    // In the icon view up / down move by a whole row of tiles.
    const size_t step = m_mode == Mode::Tiles ? TileColumns() : 1;
    const size_t page = (std::max<size_t>(VisibleLines(), 2) - 1) * step;
    auto go = [&](size_t row) {
        if (n == 0) return;
        MoveFocus(std::min(row, n - 1), false, shift);
    };
    auto back = [&](size_t by) { return focus == SIZE_MAX ? 0 : (focus > by ? focus - by : 0); };
    auto fwd = [&](size_t by) { return focus == SIZE_MAX ? 0 : focus + by; };
    switch (vk) {
    case VK_UP: go(back(step)); return true;
    case VK_DOWN: go(fwd(step)); return true;
    case VK_LEFT:
        if (m_mode != Mode::Tiles) return false;
        go(back(1));
        return true;
    case VK_RIGHT:
        if (m_mode != Mode::Tiles) return false;
        go(fwd(1));
        return true;
    case VK_PRIOR: go(back(page)); return true;
    case VK_NEXT: go(fwd(page)); return true;
    case VK_HOME: go(0); return true;
    case VK_END: go(n ? n - 1 : 0); return true;
    case VK_RETURN:
        if (focus != SIZE_MAX && focus < n) m_source->Activate(focus);
        PaintRows();
        return true;
    case VK_DELETE:
        if (m_source->DeleteSelected()) Refresh();
        return true;
    case 'A':
        if (ctrl) {
            m_source->SelectAll();
            PaintRows();
            return true;
        }
        return false;
    default:
        return false;
    }
}

}  // namespace glasswave::ui
