#include "pch.h"
#include "ui/list_view_parts.h"

// The list view's input: hit testing, the mouse, the header's menu and sorting, the marquee,
// the wheel and the keyboard.
namespace glasswave::ui {

using namespace listview;

namespace {

constexpr float kMinColumn = 24.0f;

constexpr float kEdgeGrip = 4.0f;     // header edge hit width on each side (DIP)

constexpr float kMoveThreshold = 6.0f;

constexpr int kShelfWheelMs = 220; // a horizontal wheel's step

} // namespace

ListView::Hit ListView::HitAt(POINT pt) const {
    Hit hit;
    if (!PtInRect(&m_bounds, pt)) return hit;
    const float x = (pt.x - m_bounds.left) / m_scale + ScrollX();  // content x
    if (pt.y < m_bounds.top + HeaderHeightPx()) {
        hit.part = Part::HeaderCell;
        hit.col = SIZE_MAX;
        if (!m_source) return hit;
        const auto cols = Columns(RowsWidth());
        for (size_t d = 0; d < cols.size(); ++d)
            if (std::abs(x - (cols[d].x + cols[d].w)) <= kEdgeGrip) {
                hit.part = Part::HeaderEdge;
                hit.col = d;
                return hit;
            }
        for (size_t d = 0; d < cols.size(); ++d)
            if (x >= cols[d].x && x < cols[d].x + cols[d].w) hit.col = d;
        return hit;
    }
    if (pt.y < m_bounds.top + ContentOffsetPx()) {
        hit.part = Part::Status;
        return hit;
    }
    const RECT rr = RowsRect();
    if (pt.x >= rr.right) {
        switch (ScrollBarModel::PartAt(BarGeometry(), static_cast<float>(pt.y - rr.top))) {
        case ScrollBarModel::kUp: hit.part = Part::ArrowUp; break;
        case ScrollBarModel::kDown: hit.part = Part::ArrowDown; break;
        case ScrollBarModel::kThumb: hit.part = Part::Thumb; break;
        default: hit.part = Part::Track; break;
        }
        return hit;
    }
    hit.part = Part::Empty;
    if (!m_source) return hit;
    // Below the last grouped line is empty space; LineAtDip would clamp it to the last row.
    const float contentY = ContentPoint(pt).y;
    if (Lined() && !m_lineTop.empty() && contentY >= m_lineTop.back()) return hit;
    const size_t line = LineAtDip(contentY);
    switch (m_mode) {
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        // A tile is what it draws (owner, 2026-11-20): the room spread between tiles is empty
        // space, where the page's own menu opens.
        const auto inside = [&](media::TileStyle style, float left, float top) {
            const D2D1_SIZE_F ts = TileSizeOf(style);
            const D2D1_RECT_F box = style == media::TileStyle::Card
                                        ? D2D1_RECT_F{left + kCardInset.x, top + kCardInset.y,
                                                      left + kCardInset.x + CardLayer::kCard.width,
                                                      top + kCardInset.y + CardLayer::kCard.height}
                                        : D2D1_RECT_F{left + 2, top + 3, left + ts.width - 2, top + ts.height - 3};
            return x >= box.left && x < box.right && contentY >= box.top && contentY < box.bottom;
        };
        const auto playAt = [&](size_t idx, media::TileStyle style, float left, float top) {
            if (!inside(style, left, top)) return;
            const D2D1_RECT_F box = TilePlayRect(style, left, top);
            hit.part = m_source->CanPlayTile(idx) && x >= box.left && x < box.right && contentY >= box.top &&
                               contentY < box.bottom
                           ? Part::TilePlay
                           : Part::Row;
            hit.row = idx;
        };
        if (m_tileGroups) {
            // A shelf's arrows lie over its tiles.
            const auto& groups = *m_source->Groups();
            for (size_t gi = 0; gi < groups.size() && gi < m_groupLine.size(); ++gi) {
                const auto& g = groups[gi];
                if (!g.carousel || !g.count) continue;
                const size_t header = m_groupLine[gi];
                const size_t rows = std::min(std::max<size_t>(g.tileLines, 1), g.count);
                if (header + 1 + rows >= m_lineTop.size()) continue;
                const float top = m_lineTop[header + 1], bottom = m_lineTop[header + 1 + rows];
                if (contentY < top || contentY >= bottom) continue;
                for (const bool right : {false, true}) {
                    const D2D1_RECT_F box = ShelfArrow(gi, right, top, bottom);
                    if (ShelfCanPage(gi, right) && x >= box.left && x < box.right && contentY >= box.top &&
                        contentY < box.bottom) {
                        hit.part = Part::Carousel;
                        hit.group = gi;
                        hit.line = right ? 1 : 0;
                        return hit;
                    }
                }
            }
            if (line >= m_lines.size()) return hit;
            const Line& l = m_lines[line];
            if (l.kind == Line::GroupHeader) {
                hit.group = l.group;
                const auto& g = groups[l.group];
                const float right = RowsWidth() - 8;
                const float w = g.carousel ? kShowAll : TextWidth(g.more, 200) + 2;
                if (ShowsMore(l.group) && x >= right - w && x <= right) {
                    hit.part = Part::Link;
                    hit.line = kMoreLink;
                }
                return hit; // the header's title selects nothing here
            }
            float left = 0;
            const size_t idx = TileAtLine(l, x, &left);
            if (idx == SIZE_MAX) return hit;
            playAt(idx, GroupTiles(l.group), left, m_lineTop[line]);
            return hit;
        }
        if (x < kTilePad) return hit;
        const size_t col = static_cast<size_t>((x - kTilePad) / TilePitch());
        const size_t idx = line * cols + col;
        if (col >= cols || idx >= m_source->Count()) return hit;
        playAt(idx, m_source->Tiles(), TileLeft(col), LineTopDip(line));
        return hit;
    }
    case Mode::Grouped: {
        if (line >= m_lines.size()) return hit;
        const Line& l = m_lines[line];
        if (l.kind == Line::GroupHeader) {  // WMP: clicking the group bar selects the group
            hit.part = Part::GroupHeader;
            hit.group = l.group;
            return hit;
        }
        float rowLeft = 0;
        if (AlbumColumn()) {
            const auto cols = Columns(RowsWidth());
            const float albumL = cols[0].x, albumR = cols[0].x + cols[0].w;
            rowLeft = albumR;
            if (l.kind != Line::GroupHeader && x >= albumL && x < albumR) {
                if (l.sub >= kGroupMinLines) return hit;  // below the cover and its lines: empty
                hit.part = Part::AlbumColumn;
                hit.group = l.group;
                const auto& g = (*m_source->Groups())[l.group];
                const size_t k = l.sub;
                const float tx = AlbumTextLeft(albumL);
                if (k < g.lines.size() && g.links[k] && !g.lines[k].empty() && x >= tx &&
                    x <= tx + TextWidth(g.lines[k], albumR - 4 - tx)) {
                    hit.part = Part::Link;
                    hit.line = k;
                }
                return hit;
            }
        }
        if (l.kind != Line::Item || x < rowLeft || x >= ContentWidth()) return hit;
        hit.part = Part::Row;
        hit.row = l.row;
        return hit;
    }
    default:
        if (line < m_source->Count() && x < ContentWidth()) {  // rows end with the last column
            hit.part = Part::Row;
            hit.row = line;
        }
        return hit;
    }
}

LPCWSTR ListView::CursorAt(POINT pt) const {
    if (m_drag == Drag::Resize) return IDC_SIZEWE;
    switch (HitAt(pt).part) {
    case Part::HeaderEdge: return IDC_SIZEWE;
    case Part::Link:
    case Part::Carousel:
    case Part::TilePlay: return IDC_HAND;
    default: return nullptr;
    }
}

void ListView::OnMouseMove(POINT pt) {
    switch (m_drag) {
    case Drag::Thumb: {
        const RECT rr = RowsRect();
        float top = 0, height = 0;
        if (ThumbGeometry(&top, &height)) {
            const float arrow = kScrollArrow * m_scale;
            const float trackH = static_cast<float>(rr.bottom - rr.top) - arrow * 2 - height;
            const float y = static_cast<float>(pt.y - rr.top - arrow - m_dragOffset);
            const float frac = trackH > 0 ? std::clamp(y / trackH, 0.0f, 1.0f) : 0.0f;
            SetScrollDip(frac * MaxScrollDip());
        }
        return;
    }
    case Drag::Resize: {
        const auto cols = Columns(RowsWidth());
        if (m_dragCol < cols.size()) {
            const size_t src = cols[m_dragCol].src;
            if (m_widths.size() <= src) m_widths.resize(src + 1, -1.0f);
            m_widths[src] = std::max(kMinColumn, m_dragStartW + (pt.x - m_dragStartX) / m_scale);
            PaintHeader();
            PaintRows();
        }
        return;
    }
    case Drag::PendingMove:
        if (std::abs(pt.x - m_dragStartX) < kMoveThreshold * m_scale) return;
        m_drag = Drag::Move;
        [[fallthrough]];
    case Drag::Move:
        m_dragX = pt.x;
        PaintHeader();
        return;
    case Drag::Marquee: {
        // Scroll when dragged past the top or bottom (one line per mouse move, no timer).
        const RECT rr = RowsRect();
        if (pt.y < rr.top && m_top > 0) ScrollTo(m_top - 1);
        else if (pt.y >= rr.bottom) ScrollTo(m_top + 1);
        pt.y = std::clamp<LONG>(pt.y, rr.top, rr.bottom - 1);
        m_marqueeTo = ContentPoint(pt);
        UpdateMarquee();
        PaintRows();
        return;
    }
    default:
        break;
    }
    const Hit hit = HitAt(pt);
    const size_t hotHeader = hit.part == Part::HeaderCell ? hit.col : SIZE_MAX;
    if (hotHeader != m_hotHeader) {
        m_hotHeader = hotHeader;
        PaintHeader();
    }
    const size_t hot = hit.part == Part::Row || hit.part == Part::TilePlay ? hit.row : SIZE_MAX;
    // The tile play button: on the hot tile that can play, fading in (AnimateScrollBar).
    const size_t playRow =
        m_mode == Mode::Tiles && hot != SIZE_MAX && !IsCard(hot) && m_source->CanPlayTile(hot) ? hot : SIZE_MAX;
    const bool playHot = hit.part == Part::TilePlay;
    if (playRow != m_playRow || playHot != m_playHot) {
        if (playRow != m_playRow) {
            m_playOldRow = m_playRow;
            m_playOldBlend = m_playBlend;
            m_playRow = playRow;
            m_playBlend = 0;
            m_playHotBlend = 0;
        }
        m_playHot = playHot;
        if (onScrollAnim) onScrollAnim();
    }
    // The shelf under the mouse shows its arrows (fading in); the arrow under it its plate.
    std::wstring shelfKey;
    if (m_mode == Mode::Tiles && m_tileGroups && hit.part != Part::None) {
        const D2D1_POINT_2F c = ContentPoint(pt);
        const auto& groups = *m_source->Groups();
        for (size_t gi = 0; gi < groups.size() && gi < m_groupLine.size(); ++gi) {
            const auto& g = groups[gi];
            if (!g.carousel || !g.count) continue;
            const size_t header = m_groupLine[gi];
            const size_t rows = std::min(std::max<size_t>(g.tileLines, 1), g.count);
            if (header + 1 + rows < m_lineTop.size() && c.y >= m_lineTop[header + 1] &&
                c.y < m_lineTop[header + 1 + rows])
                shelfKey = g.key;
        }
    }
    if (shelfKey != m_shelfUnder) {
        m_shelfUnder = shelfKey;
        if (onScrollAnim) onScrollAnim();
    }
    m_cards->SetHot(m_source, IsCard(hot) ? hot : SIZE_MAX, playHot);
    const size_t hotShelf = hit.part == Part::Carousel ? hit.group : SIZE_MAX;
    const size_t hotShelfSide = hit.part == Part::Carousel ? hit.line : 0;
    if (hotShelf != m_hotShelf || hotShelfSide != m_hotShelfSide) {
        m_hotShelf = hotShelf;
        m_hotShelfSide = hotShelfSide;
        m_shelfPlate = 0;
        if (onScrollAnim) onScrollAnim();
        PaintRows();
    }
    if (const int star = hot != SIZE_MAX ? StarAt(pt) : -1; star != m_hotStar) {
        m_hotStar = star;
        if (hot == m_hot) PaintRows();  // otherwise the hot-row change below repaints
    }
    const size_t hotGroup = hit.part == Part::AlbumColumn || hit.part == Part::Link ? hit.group : SIZE_MAX;
    const size_t hotGroupHeader = hit.part == Part::GroupHeader ? hit.group : SIZE_MAX;
    if (hotGroupHeader != m_hotGroupHeader) {
        m_hotGroupHeader = hotGroupHeader;
        PaintRows();
    }
    const size_t linkGroup = hit.part == Part::Link ? hit.group : SIZE_MAX;
    // A shelf's show-all ball lights under the mouse (fading, AnimateScrollBar).
    const size_t moreGroup = hit.part == Part::Link && hit.line == kMoreLink ? hit.group : SIZE_MAX;
    if (moreGroup != m_moreGroup) {
        m_moreOldGroup = m_moreGroup;
        m_moreOldHot = m_moreHot;
        m_moreGroup = moreGroup;
        m_moreHot = 0;
        if (onScrollAnim) onScrollAnim();
    }
    const size_t linkLine = hit.part == Part::Link ? hit.line : 0;
    // The download mark darkens under the mouse.
    const bool hotDownload = hit.part == Part::Row && CellAt(pt, nullptr) == static_cast<int>(CellKind::Download);
    if (hotDownload != m_hotDownload) {
        m_hotDownload = hotDownload;
        if (onScrollAnim) onScrollAnim(); // the plate fades
    }
    if (hot != m_hot) { // another row's mark starts unlit, and lights if the mouse is on it
        m_downloadHot = 0;
        if (hotDownload && onScrollAnim) onScrollAnim();
    }
    if (hot != m_hot || hotGroup != m_hotGroup || linkGroup != m_hotLinkGroup || linkLine != m_hotLinkLine) {
        m_hot = hot;
        m_hotGroup = hotGroup;
        m_hotLinkGroup = linkGroup;
        m_hotLinkLine = linkLine;
        PaintRows();
    }
    if (m_scrollBar.SetHot(BarPart(hit.part)) && onScrollAnim) onScrollAnim();
}

void ListView::OnMouseLeave() {
    if (m_drag != Drag::None) return;
    m_cards->SetHot(m_source, SIZE_MAX, false);
    if (m_hotHeader != SIZE_MAX) {
        m_hotHeader = SIZE_MAX;
        PaintHeader();
    }
    if (m_playRow != SIZE_MAX) {
        m_playOldRow = m_playRow;
        m_playOldBlend = m_playBlend;
        m_playRow = SIZE_MAX;
        m_playHot = false;
        if (onScrollAnim) onScrollAnim();
    }
    if (m_moreGroup != SIZE_MAX) {
        m_moreOldGroup = m_moreGroup;
        m_moreOldHot = m_moreHot;
        m_moreGroup = SIZE_MAX;
        if (onScrollAnim) onScrollAnim();
    }
    if (m_hotShelf != SIZE_MAX || !m_shelfUnder.empty()) {
        m_hotShelf = SIZE_MAX;
        m_shelfUnder.clear();
        if (onScrollAnim) onScrollAnim();
        PaintRows();
    }
    m_hotDownload = false;
    m_downloadHot = 0;
    if (m_hot != SIZE_MAX || m_hotGroup != SIZE_MAX || m_hotGroupHeader != SIZE_MAX || m_hotLinkGroup != SIZE_MAX) {
        m_hot = SIZE_MAX;
        m_hotGroup = SIZE_MAX;
        m_hotGroupHeader = SIZE_MAX;
        m_hotLinkGroup = SIZE_MAX;
        PaintRows();
    }
    if (m_scrollBar.SetHot(ScrollBarModel::kNone) && onScrollAnim) onScrollAnim();
}

void ListView::OnLButtonDown(POINT pt, bool ctrl, bool shift) {
    const Hit hit = HitAt(pt);
    switch (hit.part) {
    case Part::Thumb: {
        float top = 0, height = 0;
        ThumbGeometry(&top, &height);
        m_drag = Drag::Thumb;
        m_scrollBar.Press(ScrollBarModel::kThumb, static_cast<float>(pt.y - RowsRect().top), BarGeometry(), 0, 0);
        m_dragOffset = static_cast<int>(pt.y - RowsRect().top - top);
        if (onScrollAnim) onScrollAnim();
        break;
    }
    case Part::Track:
    case Part::ArrowUp:
    case Part::ArrowDown: {
        const float y = static_cast<float>(pt.y - RowsRect().top);
        if (const float step = m_scrollBar.Press(BarPart(hit.part), y, BarGeometry(), kScrollStep, ScrollPage());
            step != 0)
            ScrollBy(step);
        if (onScrollAnim) onScrollAnim();
        if (onScrollHold) onScrollHold();
        break;
    }
    case Part::HeaderEdge: {
        const auto cols = Columns(RowsWidth());
        m_drag = Drag::Resize;
        m_dragCol = hit.col;
        m_dragStartX = pt.x;
        m_dragStartW = cols[hit.col].w;
        break;
    }
    case Part::HeaderCell:
        // The album column stays in front; any other column can be dragged elsewhere.
        if (hit.col != SIZE_MAX && !(AlbumColumn() && hit.col == 0)) {
            m_drag = Drag::PendingMove;
            m_dragCol = hit.col;
            m_dragStartX = m_dragX = pt.x;
            PaintHeader();  // pressed look
        } else if (hit.col == 0 && AlbumColumn()) {
            SortBy(0);  // the album column cannot move but still sorts
        }
        break;
    case Part::Row: {
        if (IsCard(hit.row)) { // a card presses in, and opens when released on it
            m_drag = Drag::CardPress;
            m_cardPress = hit.row;
            m_source->SetFocus(hit.row);
            m_cards->SetPressed(m_source, hit.row);
            break;
        }
        // Rating stars and the Favorite star act on the clicked song (and select it).
        const int kind = CellAt(pt, nullptr);
        if (const int star = StarAt(pt); kind == static_cast<int>(CellKind::Rating) && star >= 0)
            m_source->Rate(hit.row, star + 1);
        else if (kind == static_cast<int>(CellKind::Favorite))
            m_source->ToggleFavorite(hit.row);
        else if (kind == static_cast<int>(CellKind::Download))
            m_source->ToggleDownload(hit.row);
        MoveFocus(hit.row, ctrl, shift);
        break;
    }
    case Part::Carousel: // held down: the pressed plate; it pages when released on it
        m_drag = Drag::ShelfButton;
        m_shelfPressedGroup = hit.group;
        m_shelfPressedSide = hit.line;
        if (onScrollAnim) onScrollAnim();
        break;
    case Part::TilePlay: // held: the orb's pressed look; it plays when released on it
        m_drag = Drag::PlayPress;
        m_playPress = hit.row;
        if (IsCard(hit.row)) m_cards->SetPressed(m_source, hit.row);
        PaintRows();
        break;
    case Part::Link:
        if (hit.line == kMoreLink && m_source->Groups() && hit.group < m_source->Groups()->size() &&
            (*m_source->Groups())[hit.group].carousel) {
            // The show-all ball: held while the button is down, acting when it comes up on it.
            m_drag = Drag::MoreButton;
            m_morePressedGroup = hit.group;
            if (onScrollAnim) onScrollAnim();
        } else if (hit.line == kMoreLink)
            m_source->ShowGroupMore(hit.group);
        else if (onLink)
            onLink(hit.group, hit.line);
        break;
    case Part::Empty: {
        // Empty space: start a rubber band. Without Ctrl the click clears the selection.
        if (!m_source || m_source->Count() == 0) break;
        m_drag = Drag::Marquee;
        m_marqueeFrom = m_marqueeTo = ContentPoint(pt);
        m_marqueeBase.clear();
        if (ctrl)
            for (size_t r = 0, n = m_source->Count(); r < n; ++r)
                if (m_source->IsSelected(r)) m_marqueeBase.push_back(r);
        m_source->SetSelection(m_marqueeBase);
        PaintRows();
        break;
    }
    case Part::AlbumColumn:
    case Part::GroupHeader: {  // WMP: the album cell / group bar selects the whole group
        const auto& g = (*m_source->Groups())[hit.group];
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

void ListView::OnLButtonUp(POINT pt) {
    const Drag drag = m_drag;
    m_drag = Drag::None;
    if (m_scrollBar.Release() && onScrollAnim) onScrollAnim();
    switch (drag) {
    case Drag::ShelfButton: {
        const size_t group = m_shelfPressedGroup, side = m_shelfPressedSide;
        m_shelfPressedGroup = SIZE_MAX;
        if (onScrollAnim) onScrollAnim();
        const Hit up = HitAt(pt);
        if (up.part == Part::Carousel && up.group == group && up.line == side) PageShelf(group, side == 1);
        break;
    }
    case Drag::MoreButton: {
        const size_t group = m_morePressedGroup;
        m_morePressedGroup = SIZE_MAX;
        if (onScrollAnim) onScrollAnim();
        const Hit up = HitAt(pt);
        if (up.part == Part::Link && up.line == kMoreLink && up.group == group) m_source->ShowGroupMore(group);
        break;
    }
    case Drag::Thumb: PaintScrollBar(); break;
    case Drag::Marquee: PaintRows(); break;
    case Drag::PlayPress: {
        const size_t row = std::exchange(m_playPress, SIZE_MAX);
        m_cards->SetPressed(m_source, SIZE_MAX);
        const Hit up = HitAt(pt);
        PaintRows();
        if (up.part == Part::TilePlay && up.row == row && m_source) m_source->PlayTile(row);
        break;
    }
    case Drag::CardPress: {
        const size_t row = std::exchange(m_cardPress, SIZE_MAX);
        m_cards->SetPressed(m_source, SIZE_MAX);
        const Hit up = HitAt(pt);
        if (up.part == Part::Row && up.row == row && m_source) m_source->Activate(row); // may leave the page
        break;
    }
    case Drag::Resize: SaveColumnLayout(); break;
    case Drag::PendingMove: {  // a click on a header sorts by that column
        const auto cols = Columns(RowsWidth());
        if (m_dragCol < cols.size()) SortBy(cols[m_dragCol].src);
        PaintHeader();
        break;
    }
    case Drag::Move: {
        const auto cols = Columns(RowsWidth());
        const float x = (pt.x - m_bounds.left) / m_scale + ScrollX();
        size_t target = cols.size();
        for (size_t d = 0; d < cols.size(); ++d)
            if (x < cols[d].x + cols[d].w / 2) {
                target = d;
                break;
            }
        if (AlbumColumn()) target = std::max<size_t>(target, 1);
        if (m_dragCol < cols.size() && target != m_dragCol && target != m_dragCol + 1) {
            // Move in m_order, which also holds the hidden columns: before the column now
            // shown at `target`, or after the last shown one.
            const size_t src = cols[m_dragCol].src;
            m_order.erase(std::find(m_order.begin(), m_order.end(), src));
            auto at = target < cols.size() ? std::find(m_order.begin(), m_order.end(), cols[target].src)
                                           : std::find(m_order.begin(), m_order.end(), cols.back().src) + 1;
            m_order.insert(at, src);
            SaveColumnLayout();
        }
        PaintHeader();
        PaintRows();
        break;
    }
    default: break;
    }
}

void ListView::CancelDrag() {
    if (m_drag == Drag::PlayPress) {
        m_playPress = SIZE_MAX;
        m_cards->SetPressed(m_source, SIZE_MAX);
    }
    if (m_drag == Drag::CardPress) {
        m_cardPress = SIZE_MAX;
        m_cards->SetPressed(m_source, SIZE_MAX);
    }
    if (m_drag != Drag::None) m_drag = Drag::None;
    if (m_scrollBar.Pressed() != ScrollBarModel::kNone || m_scrollBar.Hot() != ScrollBarModel::kNone) {
        m_scrollBar.Release();
        m_scrollBar.SetHot(ScrollBarModel::kNone);
        if (onScrollAnim) onScrollAnim();
    }
    PaintHeader();
    PaintRows();
    PaintScrollBar();
}

void ListView::OnDoubleClick(POINT pt) {
    const Hit hit = HitAt(pt);
    if (hit.part == Part::AlbumColumn && m_source) {
        // The album's songs only, as the click on the album selected them (owner, 2026-11-15).
        m_source->ActivateGroup(hit.group);
        PaintRows();
    } else if (hit.part == Part::Row && m_source && !IsCard(hit.row)) { // a card opened on the click
        m_source->Activate(hit.row);
        PaintRows();
    }
}

void ListView::OnRButtonUp(HWND owner, POINT pt) {
    if (!m_source) return;
    const Hit hit = HitAt(pt);
    if (hit.part == Part::Status) return;
    if (hit.part == Part::Row && !m_source->IsSelected(hit.row)) {
        m_source->SelectOnly(hit.row);
        m_source->SetFocus(hit.row);
        m_anchor = hit.row;
        PaintRows();
    } else if (hit.part == Part::GroupHeader || hit.part == Part::AlbumColumn || hit.part == Part::Link) {
        const auto& g = (*m_source->Groups())[hit.group];
        if (g.count) {
            // A right-click on a group that is already fully selected keeps the rest of the
            // selection: the menu then acts on everything the user selected. Otherwise the
            // group becomes the selection.
            bool allSelected = true;
            for (size_t r = g.first; allSelected && r < g.first + g.count; ++r)
                if (!m_source->IsSelected(r)) allSelected = false;
            if (!allSelected) {
                m_source->SelectRange(g.first, g.first + g.count - 1, false);
                m_anchor = g.first;
            }
            m_source->SetFocus(g.first);
            PaintRows();
        }
    } else if (hit.part == Part::HeaderCell || hit.part == Part::HeaderEdge) {
        ShowHeaderMenu(owner, pt, hit.col);
        return;
    }
    POINT screen = pt;
    ClientToScreen(owner, &screen);
    if ((hit.part == Part::Empty || hit.part == Part::None) && onEmptyContext) {
        onEmptyContext(screen);
        return;
    }
    m_source->ContextMenu(owner, screen);
    Refresh();
}

void ListView::SortBy(size_t src) {
    if (!m_source || !m_source->CanSort(src)) return;
    // WMP 12 cycles a column: ascending, descending, then back to the default order.
    if (m_source->SortColumn() != src)
        m_source->Sort(src, false);
    else if (!m_source->SortDescending())
        m_source->Sort(src, true);
    else
        m_source->Sort(SIZE_MAX, false);
    if (m_source->PersistSort()) SaveColumnLayout();
    Refresh();
    if (onPresentationChanged) onPresentationChanged();
}

void ListView::ShowHeaderMenu(HWND owner, POINT pt, size_t displayCol) {
    if (!m_source) return;
    const auto& srcCols = m_source->Columns();
    const auto cols = Columns(RowsWidth());
    const size_t src = displayCol < cols.size() ? cols[displayCol].src : SIZE_MAX;
    enum : UINT { kSort = 1, kReset = 2, kGroups = 3, kColumn = 100 };
    HMENU menu = CreatePopupMenu();
    HMENU choose = CreatePopupMenu();
    if (src != SIZE_MAX && m_source->CanSort(src) && !srcCols[src].title.empty()) {
        AppendMenuW(menu, MF_STRING, kSort, (Tr(L"Sort by") + L" \u201C" + Tr(srcCols[src].title) + L"\u201D").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (m_source->CanGroup()) {
        AppendMenuW(menu, MF_STRING | (m_source->Grouping() ? MF_CHECKED : 0), kGroups, Tr(L"Show groups").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    for (size_t i = 0; i < srcCols.size(); ++i) {
        if (srcCols[i].title.empty()) continue;
        const bool shown = i < m_hidden.size() ? m_hidden[i] == 0 : !srcCols[i].hidden;
        UINT flags = MF_STRING | (shown ? MF_CHECKED : 0);
        if ((AlbumColumn() && i == 0) || (shown && cols.size() <= 1)) flags |= MF_GRAYED;  // keep one
        AppendMenuW(choose, flags, kColumn + i, srcCols[i].title.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(choose), L"Choose &columns");
    AppendMenuW(menu, MF_STRING, kReset, L"&Reset columns");
    POINT screen = pt;
    ClientToScreen(owner, &screen);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, owner);
    DestroyMenu(menu);  // also destroys the submenu
    if (cmd == kSort) {
        SortBy(src);
    } else if (cmd == kGroups) {
        const bool show = !m_source->Grouping();
        cfg_showGroups.set(show ? 1 : 0);
        m_source->SetGrouping(show);
        Refresh();
        if (onPresentationChanged) onPresentationChanged();
    } else if (cmd == kReset) {
        Store().erase(m_columnKey);
        Persist();
        m_columnKey.clear();
        if (m_source->PersistSort() && m_source->SortColumn() != SIZE_MAX) m_source->Sort(SIZE_MAX, false);
        Refresh();  // reloads the defaults
        if (onPresentationChanged) onPresentationChanged();
    } else if (cmd >= kColumn && cmd < kColumn + srcCols.size()) {
        const size_t i = cmd - kColumn;
        if (m_hidden.size() < srcCols.size()) m_hidden.resize(srcCols.size(), 0);
        m_hidden[i] = m_hidden[i] ? 0 : 1;
        SaveColumnLayout();
        Refresh();  // the cover column changes the row height
    }
}

// Text the list shows cut short under the pointer (a tile's line, a cell): its whole text and
// where it stands, in client px, for the window's tooltip (owner, 2026-11-14).
bool ListView::CutTextAt(POINT pt, std::wstring* text, RECT* rect) const {
    if (!m_source || m_drag != Drag::None) return false;
    const Hit hit = HitAt(pt);
    if (hit.part != Part::Row || hit.row >= m_source->Count()) return false;
    const D2D1_POINT_2F c = ContentPoint(pt);
    const RECT rr = RowsRect();
    const auto toClient = [&](D2D1_RECT_F r) {
        return RECT{rr.left + static_cast<LONG>(std::floor((r.left - ScrollX()) * m_scale)),
                    rr.top + static_cast<LONG>(std::floor((r.top - ContentTopDip()) * m_scale)),
                    rr.left + static_cast<LONG>(std::ceil((r.right - ScrollX()) * m_scale)),
                    rr.top + static_cast<LONG>(std::ceil((r.bottom - ContentTopDip()) * m_scale))};
    };
    const auto check = [&](const std::wstring& t, D2D1_RECT_F box) {
        if (t.empty() || c.x < box.left || c.x >= box.right || c.y < box.top || c.y >= box.bottom) return false;
        if (TextWidth(t, 100000) <= box.right - box.left - 1) return false;
        *text = t;
        *rect = toClient(box);
        return true;
    };
    const size_t line = LineOfRow(hit.row);
    const float y = LineTopDip(line);
    if (m_mode == Mode::Tiles) {
        // The tile's style and left edge, wherever its group puts it (a shelf slides).
        media::TileStyle style = m_source->Tiles();
        float x = 0;
        if (m_tileGroups) {
            if (line >= m_lines.size() || TileAtLine(m_lines[line], c.x, &x) != hit.row) return false;
            style = GroupTiles(m_lines[line].group);
        } else {
            x = TileLeft(hit.row % TileColumns());
        }
        const D2D1_SIZE_F ts = TileSizeOf(style);
        const media::TileInfo info = m_source->Tile(hit.row);
        if (style == media::TileStyle::Song) {
            const float tx = x + 8 + kSongTileArt + 10, mid = y + ts.height / 2;
            return check(info.lines[0], {tx, mid - 18, x + ts.width - 6, mid}) ||
                   check(info.lines[1], {tx, mid, x + ts.width - 6, mid + 18});
        }
        if (style == media::TileStyle::Artist) {
            const float a = kArtistTileArt, ay = y + (ts.height - a) / 2, tx = x + 10 + a + 12;
            for (size_t k = 0; k < info.lines.size(); ++k)
                if (check(info.lines[k], {tx, ay + 2 + k * 18, x + ts.width - 6, ay + 2 + (k + 1) * 18})) return true;
            return false;
        }
        const float a = kAlbumTileArt, ay = y + 10;
        for (size_t k = 0; k < 2; ++k)
            if (check(info.lines[k], {x + 6, ay + a + 5 + k * 17, x + ts.width - 6, ay + a + 5 + (k + 1) * 17}))
                return true;
        return false;
    }
    const float rowH = m_mode == Mode::Grouped && line < m_lines.size() ? LineHeightDip(m_lines[line]) : RowHeightPx() / m_scale;
    for (const Col& col : Columns(RowsWidth())) {
        if (c.x < col.x || c.x >= col.x + col.w) continue;
        if (KindOf(m_source->Columns()[col.src]) != CellKind::Text) return false;
        return check(m_source->Cell(hit.row, col.src), {col.x + kCellPad, y, col.x + col.w - kCellPad, y + rowH});
    }
    return false;
}

void ListView::UpdateMarquee() {
    if (!m_source) return;
    const float x0 = std::min(m_marqueeFrom.x, m_marqueeTo.x), x1 = std::max(m_marqueeFrom.x, m_marqueeTo.x);
    const float y0 = std::min(m_marqueeFrom.y, m_marqueeTo.y), y1 = std::max(m_marqueeFrom.y, m_marqueeTo.y);
    // Lines the band touches vertically, then whatever of each line it touches horizontally.
    const size_t first = LineAtDip(std::max(0.0f, y0));
    const size_t last = LineAtDip(std::max(0.0f, y1));
    std::vector<size_t> rows = m_marqueeBase;
    const float right = ContentWidth();
    switch (m_mode) {
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        const float tw = TileSize().width;
        for (size_t line = first; line <= last; ++line)
            for (size_t c = 0; c < cols; ++c) {
                const float tx0 = TileLeft(c), tx1 = tx0 + tw;
                size_t idx = line * cols + c;
                if (m_tileGroups) {
                    if (line >= m_lines.size() || m_lines[line].kind != Line::Item) break;
                    idx = m_lines[line].row + c;
                    if (idx >= TileGroupEnd(m_lines[line].group)) break;
                }
                if (idx < m_source->Count() && x1 >= tx0 && x0 < tx1) rows.push_back(idx);
            }
        break;
    }
    case Mode::Grouped: {
        float left = 0;
        if (AlbumColumn()) {
            const auto cols = Columns(0);
            left = cols[0].x + cols[0].w;
        }
        if (x1 < left || x0 >= right) break;
        for (size_t line = first; line <= last && line < m_lines.size(); ++line)
            if (m_lines[line].kind == Line::Item) rows.push_back(m_lines[line].row);
        break;
    }
    default:
        if (x1 < 0 || x0 >= right) break;
        for (size_t r = first; r <= last && r < m_source->Count(); ++r) rows.push_back(r);
        break;
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    m_source->SetSelection(rows);
}

void ListView::OnWheel(int delta, bool horizontal) {
    if (horizontal) {  // Shift+wheel or a tilt wheel: sideways, no scrollbar shown
        // Over a shelf it moves the shelf, 100 DIP a notch, gliding (MX Master's thumb wheel).
        if (m_mode == Mode::Tiles && m_tileGroups && !m_shelfUnder.empty() && m_source && m_source->Groups()) {
            const auto& groups = *m_source->Groups();
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (groups[gi].key != m_shelfUnder || !groups[gi].carousel) continue;
                const float pitch = TilePitchOf(GroupTiles(gi), true);
                const bool running = m_shelfSlide.group == gi && ShelfShownPos(gi) != ShelfPos(gi);
                const float base = running ? m_shelfSlide.to : ShelfPos(gi);
                SlideShelf(gi, base - static_cast<float>(delta) / WHEEL_DELTA * 100.0f / pitch, kShelfWheelMs);
                return;
            }
        }
        if (m_mode != Mode::Tiles) ScrollXTo(m_scrollX - delta / static_cast<float>(WHEEL_DELTA) * 48.0f);
        return;
    }
    // As browsers do: 100 DIP a notch for Windows' three lines (a page for "one screen"),
    // fractions for fine wheels, added to the glide still running.
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float notch = lines == WHEEL_PAGESCROLL ? RowsHeightDip() * 0.875f : 100.0f * static_cast<float>(lines) / 3.0f;
    const float from = Gliding() ? m_glide.to : ContentTopDip();
    GlideTo(from - static_cast<float>(delta) / WHEEL_DELTA * notch);
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

} // namespace glasswave::ui
