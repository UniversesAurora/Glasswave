#include "pch.h"
#include "ui/list_view_parts.h"

// The list view's geometry: rows, lines, tiles and shelves, columns, and scrolling with its glide.
namespace glasswave::ui {

using namespace listview;

namespace {

constexpr float kArtRowHeight = 40.0f;  // rows with the cover column: room for a real thumbnail

constexpr float kHeaderHeight = 23.0f;  // wmploc LISTVIEWHEADER cells are 23 px

constexpr float kScrollWidth = 17.0f;

constexpr float kFlexWidth = 260.0f;  // default width of a source's "flexible" (title) column

constexpr float kTileGroupHeader = 30.0f; // DIP: a group's header line among tiles

constexpr D2D1_SIZE_F kAlbumTile{132, 160};

constexpr D2D1_SIZE_F kArtistTile{240, 108};

// Song cards (TileStyle::Song, owner 2026-11-15 after Apple Music's song shelves): new UI values.
constexpr D2D1_SIZE_F kSongTile{280, 60};

constexpr D2D1_SIZE_F kShelfArrow{26, 56}; // DIP: a shelf's paging arrow, tall as Apple Music's

constexpr float kTilePlay = 30;        // DIP: the play orb on a cover

constexpr float kSongPlay = 22;        // DIP: the play triangle on a song card's cover

// A shelf's page slides on the compositor with Windows 8.1's fast-out curve.
constexpr int kShelfSlideMs = 500;

constexpr gfx::Curve kShelfCurve = gfx::curve::kFastOut;

float ShelfEase(float t) { return gfx::Ease(kShelfCurve, t); }

// The glide's curve: ease-out cubic, cubic-bezier(0.33, 1, 0.68, 1), as browsers scroll.
constexpr gfx::Curve kGlideCurve{0.33f, 1.0f, 0.68f, 1.0f};

constexpr int kGlideMs = 220;

float GlideAt(float t) { return gfx::Ease(kGlideCurve, t); }

} // namespace

int ListView::RowHeightPx() const {
    return static_cast<int>(std::lround((ArtColumnVisible() ? kArtRowHeight : kRowHeight) * m_scale));
}

int ListView::HeaderHeightPx() const {
    return m_mode == Mode::Tiles ? 0 : static_cast<int>(std::lround(kHeaderHeight * m_scale));
}

int ListView::StatusHeightPx() const {
    return m_statusText.empty() ? 0 : static_cast<int>(std::lround(kRowHeight * m_scale));
}

int ListView::ContentOffsetPx() const {
    return HeaderHeightPx() + StatusHeightPx();
}

int ListView::ScrollWidthPx() const { return static_cast<int>(std::lround(kScrollWidth * m_scale)); }

bool ListView::ArtColumnVisible() const {
    if (!m_source || m_mode == Mode::Tiles) return false;
    // Grouped Details (library songs) has no album column, so growing the rows is safe.
    // Expanded tile has the album column and no Art column; keep its measured height.
    if (m_mode == Mode::Grouped && m_groupArt) return false;
    const auto& columns = m_source->Columns();
    for (size_t i = 0; i < columns.size(); ++i) {
        const bool shown = !(i < m_hidden.size() ? m_hidden[i] != 0 : columns[i].hidden);
        if (shown && KindOf(columns[i]) == CellKind::Art) return true;
    }
    return false;
}

int ListView::LineHeightPx() const {
    if (m_mode == Mode::Tiles) return static_cast<int>(std::lround(TileSize().height * m_scale));
    return RowHeightPx();
}

D2D1_SIZE_F ListView::TileSize() const {
    return TileSizeOf(m_source ? m_source->Tiles() : media::TileStyle::Album);
}

size_t ListView::TileColumns() const {
    return TileColumnsOf(m_source ? m_source->Tiles() : media::TileStyle::Album);
}

float ListView::TilePitch() const {
    return TilePitchOf(m_source ? m_source->Tiles() : media::TileStyle::Album);
}

float ListView::TileLeft(size_t column) const {
    return TileLeftOf(m_source ? m_source->Tiles() : media::TileStyle::Album, static_cast<float>(column));
}

media::TileStyle ListView::GroupTiles(size_t group) const {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (groups && group < groups->size() && (*groups)[group].tiles != media::TileStyle::None)
        return (*groups)[group].tiles;
    return m_source ? m_source->Tiles() : media::TileStyle::Album;
}

D2D1_SIZE_F ListView::TileSizeOf(media::TileStyle style) const {
    switch (style) {
    case media::TileStyle::Artist: return kArtistTile;
    case media::TileStyle::Song: return kSongTile;
    case media::TileStyle::Card: return kCardTile;
    default: return kAlbumTile;
    }
}

size_t ListView::TileColumnsOf(media::TileStyle style) const {
    const float w = RowsWidth() - 2 * kTilePad;
    return static_cast<size_t>(std::max(1.0f, std::floor(w / TileSizeOf(style).width)));
}

// The row's spare width is shared out between the tiles (owner, 2026-11-14: it was all at
// the right), so they spread as the window is resized. A shelf's tiles keep their own width
// instead (owner, 2026-11-20, as the cards): the last one may show in part.
float ListView::TilePitchOf(media::TileStyle style, bool shelf) const {
    if (shelf || style == media::TileStyle::Card) return TileSizeOf(style).width;
    return std::max(TileSizeOf(style).width, (RowsWidth() - 2 * kTilePad) / static_cast<float>(TileColumnsOf(style)));
}

float ListView::TileLeftOf(media::TileStyle style, float column, bool shelf) const {
    const float pitch = TilePitchOf(style, shelf);
    return std::round((kTilePad + column * pitch + (pitch - TileSizeOf(style).width) / 2) * m_scale) / m_scale;
}

size_t ListView::ShelfColumns(size_t group) const {
    const auto& g = (*m_source->Groups())[group];
    const size_t rows = std::max<size_t>(g.tileLines, 1);
    return (g.count + rows - 1) / rows;
}

float ListView::ShelfPos(size_t group) const {
    const auto& g = (*m_source->Groups())[group];
    const auto it = m_shelves.find(g.key);
    // A restored place beyond a shelf that came back shorter: its end.
    return it == m_shelves.end() ? 0.0f : std::min(it->second.pos, ShelfLast(group));
}

std::map<std::wstring, float> ListView::ShelfPositions() const {
    std::map<std::wstring, float> out;
    for (const auto& [key, shelf] : m_shelves)
        if (shelf.target > 0) out[key] = shelf.target;
    return out;
}

void ListView::SetShelfPositions(const std::map<std::wstring, float>& positions) {
    if (positions.empty()) return;
    for (const auto& [key, pos] : positions) {
        Shelf& shelf = m_shelves[key];
        shelf.pos = shelf.target = pos;
    }
    PaintRows();
}

// How many of a shelf's columns the view holds (a part of one counts as that part).
float ListView::ShelfVisible(size_t group) const {
    return (RowsWidth() - 2 * kTilePad) / TilePitchOf(GroupTiles(group), true);
}

float ListView::ShelfLast(size_t group) const {
    return std::max(0.0f, static_cast<float>(ShelfColumns(group)) - ShelfVisible(group));
}

bool ListView::ShelfCanPage(size_t group, bool right) const {
    const auto& g = (*m_source->Groups())[group];
    const auto it = m_shelves.find(g.key);
    const float target = it == m_shelves.end() ? 0.0f : it->second.target;
    return right ? target < ShelfLast(group) - 0.01f : target > 0.01f;
}

void ListView::PageShelf(size_t group, bool right) {
    if (!ShelfCanPage(group, right) || group >= m_groupLine.size()) return;
    const auto& g = (*m_source->Groups())[group];
    const float page = std::max(1.0f, std::floor(ShelfVisible(group)));
    const float target = m_shelves[g.key].target;
    SlideShelf(group, target + (right ? page : -page), kShelfSlideMs);
}

// Where a shelf stands on screen now: mid-slide, between where it slid from and to.
float ListView::ShelfShownPos(size_t group) const {
    if (group != m_shelfSlide.group || m_shelfSlide.ms <= 0) return ShelfPos(group);
    const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_shelfSlide.start).count() /
                    static_cast<float>(m_shelfSlide.ms);
    if (t >= 1) return ShelfPos(group);
    return m_shelfSlide.from + (m_shelfSlide.to - m_shelfSlide.from) * ShelfEase(std::max(t, 0.0f));
}

void ListView::SlideShelf(size_t group, float to, int ms) {
    if (group >= m_groupLine.size()) return;
    const auto& g = (*m_source->Groups())[group];
    // From where it shows now, so a slide given while another runs carries on from it.
    const float from = ShelfShownPos(group);
    Shelf& shelf = m_shelves[g.key];
    to = std::clamp(to, 0.0f, ShelfLast(group));
    // Nothing to move (a wheel at the shelf's end): the strip, if one runs, finishes as it is.
    if (std::abs(to - from) < 0.001f && std::abs(to - shelf.target) < 0.001f) return;
    if (!m_slideKey.empty()) {
        m_slideKey.clear();
        m_slide.StopAnimation(L"Offset");
        m_slideClip.IsVisible(false);
    }
    shelf.pos = shelf.target = to;
    m_shelfSlide = {group, from, to, std::chrono::steady_clock::now(), ms};
    const media::TileStyle style = GroupTiles(group);
    if (style == media::TileStyle::Card) { // the cards' strip slides itself (LayoutCards)
        m_cardSlideGroup = group;
        m_cardSlideFrom = from;
        m_cardSlideMs = ms;
        PaintRows();
        return;
    }
    // The strip: every column the slide passes over, the shelf's rows.
    const size_t rows = std::max<size_t>(g.tileLines, 1), shown = std::min(rows, g.count);
    const size_t header = m_groupLine[group];
    if (header + 1 + shown >= m_lineTop.size()) {
        PaintRows();
        return;
    }
    const float tileH = TileSizeOf(style).height, pitch = TilePitchOf(style, true);
    const size_t first = static_cast<size_t>(std::floor(std::min(from, to)));
    const size_t end = std::min(ShelfColumns(group),
                                static_cast<size_t>(std::ceil(std::max(from, to) + ShelfVisible(group))) + 1);
    // In content coordinates (absolute: m_content's offset is minus the scroll position).
    const float top = m_lineTop[header + 1];
    const float stripW = TileLeftOf(style, static_cast<float>(end - first), true) + pitch;
    const float stripH = tileH * static_cast<float>(shown);
    const size_t focus = m_source->Focus();
    auto& host = gfx::CompositionHost::Get();
    const int wpx = std::max(1, static_cast<int>(std::ceil(stripW * m_scale)));
    const int hpx = std::max(1, static_cast<int>(std::ceil(stripH * m_scale)));
    host.Repaint(m_slide, wpx, hpx, m_scale, [&](ID2D1DeviceContext* dc) {
        for (size_t c = first; c < end; ++c)
            for (size_t k = 0; k < shown; ++k) {
                const size_t idx = g.first + c * rows + k;
                if (idx >= g.first + g.count) continue;
                PaintTile(dc, idx, TileLeftOf(style, static_cast<float>(c - first), true), tileH * static_cast<float>(k),
                          style, focus);
            }
    });
    m_slideClip.Offset(float3{0, std::round(top * m_scale), 0});
    m_slideClip.Size(float2{RowsWidth() * m_scale, static_cast<float>(hpx)});
    m_slideClip.IsVisible(true);
    const auto offsetOf = [&](float pos) {
        return float3{std::round(-(pos - static_cast<float>(first)) * pitch * m_scale), 0, 0};
    };
    m_slide.Offset(offsetOf(from));
    const auto& comp = host.Compositor();
    auto slide = comp.CreateVector3KeyFrameAnimation();
    slide.InsertKeyFrame(1.0f, offsetOf(to),
                         gfx::Easing(kShelfCurve));
    slide.Duration(std::chrono::milliseconds(ms));
    m_slide.StartAnimation(L"Offset", slide);
    m_slideKey = g.key;
    m_slideEnd = GetTickCount64() + static_cast<ULONGLONG>(ms);
    m_slideTop = ContentTopDip();
    PaintRows(); // without the shelf's tiles, which the strip shows meanwhile
    if (onScrollAnim) onScrollAnim();
}

void ListView::FinishSlide() {
    if (m_slideKey.empty()) return;
    m_slideKey.clear();
    m_slide.StopAnimation(L"Offset");
    m_slideClip.IsVisible(false);
    PaintRows();
}

D2D1_RECT_F ListView::ShelfArrow(size_t, bool right, float top, float bottom) const {
    const float y = std::round(((top + bottom - kShelfArrow.height) / 2) * m_scale) / m_scale;
    const float x = right ? RowsWidth() - 2 - kShelfArrow.width : 2.0f;
    return {x, y, x + kShelfArrow.width, y + kShelfArrow.height};
}

D2D1_RECT_F ListView::TilePlayRect(media::TileStyle style, float x, float y) const {
    const D2D1_SIZE_F ts = TileSizeOf(style);
    switch (style) {
    case media::TileStyle::Card: {
        const float r = x + kCardInset.x + CardLayer::kCard.width - CardLayer::kPlayInset;
        const float b = y + kCardInset.y + CardLayer::kCard.height - CardLayer::kPlayInset;
        return {r - CardLayer::kPlay, b - CardLayer::kPlay, r, b};
    }
    case media::TileStyle::Song: {
        const float a = kSongTileArt, ay = y + (ts.height - a) / 2, cx = x + 8 + a / 2, cy = ay + a / 2;
        return {cx - kSongPlay / 2, cy - kSongPlay / 2, cx + kSongPlay / 2, cy + kSongPlay / 2};
    }
    case media::TileStyle::Artist: {
        const float a = kArtistTileArt, ax = x + 10, ay = y + (ts.height - a) / 2;
        return {ax + a - kTilePlay - 4, ay + a - kTilePlay - 4, ax + a - 4, ay + a - 4};
    }
    default: {
        const float a = kAlbumTileArt, ax = x + (ts.width - a) / 2, ay = y + 10;
        return {ax + a - kTilePlay - 4, ay + a - kTilePlay - 4, ax + a - 4, ay + a - 4};
    }
    }
}

size_t ListView::TileAtLine(const Line& line, float x, float* left) const {
    const auto& groups = *m_source->Groups();
    const auto& g = groups[line.group];
    const media::TileStyle style = GroupTiles(line.group);
    const float pitch = TilePitchOf(style, g.carousel);
    if (x < kTilePad || x >= RowsWidth()) return SIZE_MAX;
    if (g.carousel) {
        const float pos = ShelfPos(line.group);
        const float column = std::floor((x - kTilePad) / pitch + pos);
        if (column < 0) return SIZE_MAX;
        const size_t rows = std::max<size_t>(g.tileLines, 1);
        const size_t idx = g.first + static_cast<size_t>(column) * rows + line.sub;
        if (idx >= g.first + g.count) return SIZE_MAX;
        if (left) *left = TileLeftOf(style, column - pos, true);
        return idx;
    }
    const size_t col = static_cast<size_t>((x - kTilePad) / pitch);
    const size_t idx = line.row + col;
    if (col >= TileColumnsOf(style) || idx >= TileGroupEnd(line.group)) return SIZE_MAX;
    if (left) *left = TileLeftOf(style, static_cast<float>(col));
    return idx;
}

RECT ListView::RowsRect() const {
    return {m_bounds.left, m_bounds.top + ContentOffsetPx(), m_bounds.right - ScrollWidthPx(),
            m_bounds.bottom};
}

float ListView::RowsWidth() const {
    const RECT r = RowsRect();
    return std::max<LONG>(r.right - r.left, 0) / m_scale;
}

float ListView::ArtSize() const { return kGroupMinLines * (RowHeightPx() / m_scale) - 8; }

float ListView::AlbumTextLeft(float albumLeft) const { return albumLeft + 3 + ArtSize() + 8; }

float ListView::LineHeightDip(const Line& line) const {
    if (m_tileGroups)
        return line.kind == Line::GroupHeader ? kTileGroupHeader : TileSizeOf(GroupTiles(line.group)).height;
    if (m_mode == Mode::Grouped && !m_groupArt && line.kind == Line::GroupHeader)
        return static_cast<float>(std::lround(kRowHeight * m_scale)) / m_scale;
    return RowHeightPx() / m_scale;
}

float ListView::LineTopDip(size_t line) const {
    if (Lined()) return m_lineTop.empty() ? 0 : m_lineTop[std::min(line, m_lineTop.size() - 1)];
    return static_cast<float>(line) * (LineHeightPx() / m_scale);
}

size_t ListView::LineAtDip(float y) const {
    if (Lined()) {
        if (m_lineTop.size() < 2) return 0;
        auto it = std::upper_bound(m_lineTop.begin(), m_lineTop.end(), y);
        const size_t line = it == m_lineTop.begin() ? 0 : static_cast<size_t>(it - m_lineTop.begin() - 1);
        return std::min(line, m_lineTop.size() - 2);
    }
    return y > 0 ? static_cast<size_t>(y / std::max(LineHeightPx() / m_scale, 0.001f)) : 0;
}

float ListView::RowsHeightDip() const {
    if (m_paintViewH >= 0) return m_paintViewH;
    const RECT r = RowsRect();
    return std::max<LONG>(r.bottom - r.top, 0) / m_scale;
}

size_t ListView::LineCount() const {
    const size_t n = m_source ? m_source->Count() : 0;
    switch (m_mode) {
    case Mode::Grouped: return m_lines.size();
    case Mode::Tiles: {
        if (m_tileGroups) return m_lines.size();
        const size_t cols = TileColumns();
        return (n + cols - 1) / cols;
    }
    default: return n;
    }
}

size_t ListView::LineOfRow(size_t row) const {
    switch (m_mode) {
    case Mode::Grouped: return row < m_rowLine.size() ? m_rowLine[row] : 0;
    case Mode::Tiles:
        if (m_tileGroups) return row < m_rowLine.size() ? m_rowLine[row] : 0;
        return row / TileColumns();
    default: return row;
    }
}

size_t ListView::VisibleLines() const {
    const float viewH = RowsHeightDip();
    if (Lined() && !m_lineTop.empty()) {
        size_t n = 0;
        float y = -m_topOffsetDip;
        for (size_t i = m_top; i < m_lines.size() && y < viewH; ++i) {
            y += LineHeightDip(m_lines[i]);
            ++n;
        }
        return n;
    }
    const float lineH = std::max(LineHeightPx() / m_scale, 0.001f);
    return static_cast<size_t>((viewH + std::max(m_topOffsetDip, 0.0f)) / lineH);
}

size_t ListView::MaxTop() const {
    const size_t n = LineCount();
    if (Lined() && !m_lineTop.empty()) {
        const float viewH = RowsHeightDip();
        const float total = m_lineTop.back();
        if (total <= viewH) return 0;
        // Stop where the last line's bottom reaches the viewport bottom; scrolling further
        // would only add empty space below it.
        auto it = std::lower_bound(m_lineTop.begin(), m_lineTop.end(), total - viewH);
        const size_t line = static_cast<size_t>(it - m_lineTop.begin());
        return std::min(line, n ? n - 1 : 0);
    }
    const size_t v = VisibleLines();
    return n > v ? n - v : 0;
}

std::vector<ListView::Col> ListView::Columns(float /*total*/) const {
    std::vector<Col> out;
    if (!m_source) return out;
    const auto& cols = m_source->Columns();
    // Every column keeps its own width, the "flexible" one included (it only gets a wider
    // default), so the last column ends where the user put it, not at the window edge.
    auto widthOf = [&](size_t i) {
        return i < m_widths.size() && m_widths[i] > 0 ? m_widths[i] : (cols[i].flex ? kFlexWidth : cols[i].width);
    };
    auto shown = [&](size_t i) { return !(i < m_hidden.size() ? m_hidden[i] != 0 : cols[i].hidden); };
    // With the album column the playing-marker gutter sits after it, before the tracks.
    const bool album = AlbumColumn();
    float x = album ? 0.0f : kGutter;
    for (size_t d = 0; d < cols.size(); ++d) {
        const size_t src = d < m_order.size() ? m_order[d] : d;
        if (!shown(src)) continue;
        const float w = widthOf(src);
        out.push_back({src, x, w});
        x += w;
        if (album && out.size() == 1) x += kGutter;
    }
    return out;
}

float ListView::ContentWidth() const {
    const auto cols = Columns(0);
    return cols.empty() ? 0.0f : cols.back().x + cols.back().w;
}

void ListView::ScrollXTo(float x) {
    const float max = std::max(0.0f, ContentWidth() + 4 - RowsWidth());
    x = std::clamp(std::round(x * m_scale) / m_scale, 0.0f, max);
    if (x == m_scrollX) return;
    m_scrollX = x;
    PaintHeader();
    PaintRows();
}

int ListView::CellAt(POINT pt, size_t* srcColumn) const {
    if (!m_source || m_mode == Mode::Tiles) return static_cast<int>(CellKind::Text);
    const float x = ContentPoint(pt).x;
    const auto& srcCols = m_source->Columns();
    for (const Col& c : Columns(0))
        if (x >= c.x && x < c.x + c.w) {
            if (srcColumn) *srcColumn = c.src;
            return static_cast<int>(KindOf(srcCols[c.src]));
        }
    return static_cast<int>(CellKind::Text);
}

int ListView::StarAt(POINT pt) const {
    size_t src = SIZE_MAX;
    if (CellAt(pt, &src) != static_cast<int>(CellKind::Rating)) return -1;
    const float x = ContentPoint(pt).x;
    for (const Col& c : Columns(0))
        if (c.src == src) {
            const float rel = x - (c.x + kCellPad);
            return rel >= 0 && rel < 5 * kStar ? static_cast<int>(rel / kStar) : -1;
        }
    return -1;
}

D2D1_POINT_2F ListView::ContentPoint(POINT pt) const {
    const RECT rr = RowsRect();
    return {(pt.x - rr.left) / m_scale + ScrollX(), (pt.y - rr.top) / m_scale + ContentTopDip()};
}

float ListView::ContentTopDip() const {
    return LineTopDip(m_top) + m_topOffsetDip;
}

void ListView::ScrollTopTo(float top) {
    top = std::max(top, 0.0f);
    const size_t n = LineCount();
    if (n == 0) {
        m_top = 0;
        m_topOffsetDip = 0;
        return;
    }
    if (Lined() && !m_lineTop.empty()) {
        const auto it = std::upper_bound(m_lineTop.begin(), m_lineTop.end(), top);
        const size_t line = it == m_lineTop.begin() ? 0 : static_cast<size_t>(it - m_lineTop.begin() - 1);
        m_top = std::min(line, n - 1);
        m_topOffsetDip = std::max(top - LineTopDip(m_top), 0.0f);
        return;
    }
    const float lineH = std::max(LineHeightPx() / m_scale, 0.001f);
    const size_t line = top > 0 ? static_cast<size_t>(top / lineH) : 0;
    m_top = std::min(line, n - 1);
    m_topOffsetDip = std::max(top - LineTopDip(m_top), 0.0f);
}

float ListView::ContentHeightDip() const {
    if (Lined()) return m_lineTop.empty() ? 0.0f : m_lineTop.back();
    return static_cast<float>(LineCount()) * (LineHeightPx() / m_scale);
}

float ListView::MaxScrollDip() const { return std::max(0.0f, ContentHeightDip() - RowsHeightDip()); }

void ListView::SetScrollDip(float top) {
    m_glide.ms = 0;
    top = std::round(std::clamp(top, 0.0f, MaxScrollDip()) * m_scale) / m_scale;
    if (std::abs(top - ContentTopDip()) < 0.01f) return;
    ScrollTopTo(top);
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

bool ListView::Gliding() const {
    return m_glide.ms > 0 &&
           std::chrono::steady_clock::now() - m_glide.start < std::chrono::milliseconds(m_glide.ms);
}

float ListView::VisualTopDip() const {
    if (!Gliding()) return ContentTopDip();
    const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_glide.start).count() /
                    static_cast<float>(m_glide.ms);
    return m_glide.from + (m_glide.to - m_glide.from) * GlideAt(std::clamp(t, 0.0f, 1.0f));
}

void ListView::GlideTo(float top) {
    top = std::round(std::clamp(top, 0.0f, MaxScrollDip()) * m_scale) / m_scale;
    if (std::abs(top - ContentTopDip()) < 0.01f) return;
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    if (!animations) {
        SetScrollDip(top);
        return;
    }
    const float viewH = RowsHeightDip();
    float from = std::round(VisualTopDip() * m_scale) / m_scale;
    // A long way glides only its last screen and a half (the rows painted for it stay bounded).
    const float run = 1.5f * viewH;
    const bool jump = std::abs(top - from) > run;
    if (jump) from = top - std::copysign(run, top - from);
    ScrollTopTo(top);
    // The rows for the whole way, from where the content stands to where it goes, with a margin
    // either side: `from` is this thread's estimate of where the running glide is, and the
    // compositor (which started it a draw and a commit later) can be a little behind it.
    const float margin = 0.25f * viewH;
    m_glide = {from, top, std::chrono::steady_clock::now(), kGlideMs};
    m_paintTop = std::max(0.0f, std::min(from, top) - margin);
    m_paintHeight = std::max(from, top) + viewH + margin - m_paintTop;
    PaintRows();
    // Content coordinates are absolute (the offset is minus the scroll position, whatever range
    // is painted), so the glide goes on from where the compositor shows the content now, not
    // from an estimate: started from the estimate, the content jumped ahead at every wheel
    // notch of a running glide (owner, 2026-11-21: scrolling up and down, content flashed over
    // the home cards). A long jump starts from its last screen and a half.
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    auto glide = comp.CreateVector3KeyFrameAnimation();
    if (jump)
        glide.InsertKeyFrame(0.0f, float3{0, -std::round(from * m_scale), 0});
    else
        glide.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
    glide.InsertKeyFrame(1.0f, float3{0, -std::round(top * m_scale), 0},
                         gfx::Easing(kGlideCurve));
    glide.Duration(std::chrono::milliseconds(kGlideMs));
    m_content.StartAnimation(L"Offset", glide);
    PaintScrollBar();
    CheckNearEnd();
}

void ListView::UpdateStatusText() {
    std::wstring next = m_source ? m_source->LoadingText() : std::wstring();
    if (next == m_statusText) return;
    // The band takes its own room above the rows and pushes them down; the scroll position
    // stays, so it never hides the first row (scrolling to make room hid it on every load).
    m_statusText = std::move(next);
}

} // namespace glasswave::ui
