#include "pch.h"
#include "ui/list_view_parts.h"

namespace glasswave::ui {

using namespace listview;

namespace listview {

cfg_var_modern::cfg_string cfg_columns({0x5e8b2c47, 0x9a1d, 0x4f63, {0xb0, 0xe2, 0x6c, 0x4a, 0x7d, 0x9f, 0x1b, 0x38}},
                                       "");

cfg_var_modern::cfg_int cfg_showGroups({0x5e8b2c48, 0x9a1d, 0x4f63, {0xb0, 0xe2, 0x6c, 0x4a, 0x7d, 0x9f, 0x1b, 0x38}},
                                       1);

} // namespace listview

ListView::ListView(const wuc::ContainerVisual& parent) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    m_clipGeometry = comp.CreateRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clipGeometry));
    m_header = comp.CreateSpriteVisual();
    m_status = comp.CreateSpriteVisual();
    m_rows = comp.CreateSpriteVisual();
    m_scroll = comp.CreateSpriteVisual();
    m_viewport = comp.CreateContainerVisual();
    m_viewport.Clip(comp.CreateInsetClip());
    m_content = comp.CreateContainerVisual();
    m_viewport.Children().InsertAtTop(m_content);
    m_root.Children().InsertAtTop(m_viewport);
    m_content.Children().InsertAtTop(m_rows);
    m_root.Children().InsertAtTop(m_status);
    m_root.Children().InsertAtTop(m_header);
    m_root.Children().InsertAtTop(m_scroll);
    m_thumb = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_thumb);
    // thumb top (px) = clamp(a + b * (top - content offset), lo, hi): content coordinates are
    // absolute, so the scroll position in px is minus m_content's offset, mid-glide included.
    m_thumbPlace = comp.CreatePropertySet();
    for (const wchar_t* term : {L"x", L"a", L"b", L"top", L"lo", L"hi"}) m_thumbPlace.InsertScalar(term, 0);
    auto follow = comp.CreateExpressionAnimation(
        L"Vector3(p.x, Clamp(p.a + p.b * (p.top - c.Offset.Y), p.lo, p.hi), 0)");
    follow.SetReferenceParameter(L"p", m_thumbPlace);
    follow.SetReferenceParameter(L"c", m_content);
    m_thumb.StartAnimation(L"Offset", follow);
    // A shelf sliding to another page: its tiles in their own strip under the rows (whose
    // surface is clear there), clipped to the list, moved by the compositor.
    m_slideClip = comp.CreateContainerVisual();
    m_slideClip.Clip(comp.CreateInsetClip());
    m_slideClip.IsVisible(false);
    m_slide = comp.CreateSpriteVisual();
    m_slideClip.Children().InsertAtTop(m_slide);
    m_content.Children().InsertBelow(m_slideClip, m_rows);
    m_cards = std::make_unique<CardLayer>(m_content, m_rows);
    m_status.IsVisible(false);
    parent.Children().InsertAtTop(m_root);
    // A plain list draws covers only in its Album art column.
    m_artToken = ArtCache::Get().Subscribe([this] {
        if (m_mode != Mode::Plain || ArtColumnVisible()) PaintRows();
    });
    m_iconToken = SubscribeSourceIcons([this] {
        if (m_mode == Mode::Tiles) PaintRows();
    });
    m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] {
        m_artBitmaps.clear();
        PaintRows();
    });
    m_cellChanges = media::WatchControlCells([this] {
        if (m_source && m_mode != Mode::Tiles)
            for (const auto& column : m_source->Columns())
                if (KindOf(column) == CellKind::Download) {
                    PaintRows();
                    return;
                }
    });
}

ListView::~ListView() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    UnsubscribeSourceIcons(m_iconToken);
}

// ---- Data -----------------------------------------------------------------------------------

bool ListView::LoadColumnLayout() {
    const auto& cols = m_source->Columns();
    std::wstring key;
    if (!m_source->ColumnSetKey().empty()) key = m_source->ColumnSetKey() + L":";
    for (const auto& c : cols) key += c.title + L'|';
    if (AlbumColumn()) key += L"#art";
    if (key == m_columnKey && m_order.size() == cols.size()) return false;
    m_columnKey = key;
    m_order.resize(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) m_order[i] = i;
    m_widths.assign(cols.size(), -1.0f);
    m_hidden.resize(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) m_hidden[i] = cols[i].hidden ? 1 : 0;
    auto it = Store().find(key);
    if (it == Store().end()) return false;
    const SavedLayout& s = it->second;
    std::vector<size_t> sorted = s.order;
    std::sort(sorted.begin(), sorted.end());
    bool valid = sorted.size() == cols.size();
    for (size_t i = 0; valid && i < sorted.size(); ++i) valid = sorted[i] == i;
    if (valid) m_order = s.order;
    if (s.widths.size() == cols.size()) m_widths = s.widths;
    if (s.hidden.size() == cols.size()) m_hidden = s.hidden;
    if (m_source->PersistSort() && s.sortColumn < cols.size() && m_source->CanSort(s.sortColumn) &&
        (m_source->SortColumn() != s.sortColumn || m_source->SortDescending() != s.sortDescending)) {
        m_source->Sort(s.sortColumn, s.sortDescending);
        return true;
    }
    return false;
}

void ListView::SaveColumnLayout() const {
    if (m_columnKey.empty()) return;
    const bool persistSort = m_source && m_source->PersistSort();
    const size_t sortCol = persistSort ? m_source->SortColumn() : SIZE_MAX;
    const bool sortDesc = persistSort && sortCol != SIZE_MAX && m_source->SortDescending();
    Store()[m_columnKey] = {m_order, m_widths, m_hidden, sortCol, sortDesc};
    Persist();
}

void ListView::BuildLines() {
    auto assemble = [this] {
        m_lines.clear();
        m_lineTop.clear();
        m_rowLine.clear();
        m_groupLine.clear();
        m_mode = Mode::Plain;
        m_groupArt = false;
        m_tileGroups = false;
        if (!m_source) return;
        if (m_source->Tiles() != media::TileStyle::None) {
            m_mode = Mode::Tiles;
            if (const auto* groups = m_source->Groups(); groups && !groups->empty()) {
                m_tileGroups = true;
                m_rowLine.assign(m_source->Count(), 0);
                for (size_t gi = 0; gi < groups->size(); ++gi) {
                    const auto& g = (*groups)[gi];
                    const size_t cols = TileColumnsOf(GroupTiles(gi));
                    m_groupLine.push_back(m_lines.size());
                    m_lines.push_back({Line::GroupHeader, SIZE_MAX, gi, 0});
                    if (g.carousel) {  // a shelf: its rows, the tiles column by column across them
                        const size_t rows = std::max<size_t>(g.tileLines, 1);
                        const size_t first = m_lines.size();
                        for (size_t k = 0; k < std::min(rows, g.count); ++k)
                            m_lines.push_back({Line::Item, g.first, gi, k});
                        for (size_t t = g.first; t < g.first + g.count && t < m_rowLine.size(); ++t)
                            m_rowLine[t] = first + (t - g.first) % rows;
                        continue;
                    }
                    const size_t end = TileGroupEnd(gi);
                    for (size_t r = g.first, k = 0; r < end; r += cols, ++k) {
                        m_lines.push_back({Line::Item, r, gi, k});
                        for (size_t t = r; t < std::min(end, r + cols); ++t) m_rowLine[t] = m_lines.size() - 1;
                    }
                    // Rows past the shown ones belong to the group's last line (for scrolling to them).
                    for (size_t t = end; t < g.first + g.count && t < m_rowLine.size(); ++t)
                        m_rowLine[t] = m_lines.size() - 1;
                }
            }
        } else if (const auto* groups = m_source->Groups(); groups && !groups->empty()) {
            m_mode = Mode::Grouped;
            m_groupArt = m_source->GroupArt();
            m_rowLine.assign(m_source->Count(), 0);
            for (size_t gi = 0; gi < groups->size(); ++gi) {
                const auto& g = (*groups)[gi];
                m_groupLine.push_back(m_lines.size());
                m_lines.push_back({Line::GroupHeader, SIZE_MAX, gi, 0});
                // With the album column a group is at least as tall as its cover.
                const size_t lines = m_groupArt ? std::max(g.count, kGroupMinLines) : g.count;
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
    };
    assemble();
    if (!m_source) return;
    if (LoadColumnLayout()) assemble();
    // The album column stays first and visible.
    if (AlbumColumn() && !m_order.empty() && m_order[0] != 0) {
        m_order.erase(std::find(m_order.begin(), m_order.end(), size_t{0}));
        m_order.insert(m_order.begin(), 0);
    }
    if (AlbumColumn() && !m_hidden.empty()) m_hidden[0] = 0;
    m_lineTop.clear();
    if (Lined()) {
        m_lineTop.reserve(m_lines.size() + 1);
        float y = 0;
        for (const Line& line : m_lines) {
            m_lineTop.push_back(y);
            y += LineHeightDip(line);
        }
        m_lineTop.push_back(y);
    }
}

// The header's `more` link: while the group hides rows or has more to load, and to collapse it.
bool ListView::ShowsMore(size_t group) const {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (!groups || group >= groups->size()) return false;
    const auto& g = (*groups)[group];
    return !g.more.empty() &&
           (g.carousel || g.tileLines == 0 || g.moreBeyond || TileGroupEnd(group) < g.first + g.count);
}

size_t ListView::TileGroupEnd(size_t group) const {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (!groups || group >= groups->size()) return 0;
    const auto& g = (*groups)[group];
    if (g.carousel) return std::min(g.first + g.count, m_source->Count());
    const size_t shown = g.tileLines ? std::min(g.count, g.tileLines * TileColumnsOf(GroupTiles(group))) : g.count;
    return std::min(g.first + shown, m_source->Count());
}

void ListView::SetSource(media::ListSource* source, bool resetScroll) {
    if (source != m_source) {
        m_cards->Clear();
        m_columnKey.clear();
    }
    m_cardPress = SIZE_MAX;
    m_source = source;
    if (m_source && m_source->CanGroup()) m_source->SetGrouping(cfg_showGroups.get() != 0);
    m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    m_hotGroupHeader = SIZE_MAX;
    m_hotLinkGroup = SIZE_MAX;
    m_hotShelf = SIZE_MAX;
    m_moreGroup = m_moreOldGroup = m_morePressedGroup = m_shelfPressedGroup = SIZE_MAX;
    m_moreHot = m_moreOldHot = m_morePress = 0;
    m_playRow = m_playOldRow = SIZE_MAX;
    m_playBlend = m_playOldBlend = m_playHotBlend = 0;
    m_playHot = false;
    m_anchor = source ? source->Focus() : SIZE_MAX;
    if (!m_slideKey.empty()) {
        m_slideKey.clear();
        m_slideClip.IsVisible(false);
    }
    if (resetScroll) {
        m_shelves.clear();
        m_top = 0;
        m_topOffsetDip = 0;
        m_statusText = m_source ? m_source->LoadingText() : std::wstring();
        m_scrollX = 0;
    }
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
    m_clipGeometry.Size(float2{std::max(w, 0.0f), std::max(h, 0.0f)});
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
    UpdateStatusText();
    BuildLines();  // rows, groups or the view style may have changed
    if (ContentTopDip() > MaxScrollDip()) ScrollTopTo(MaxScrollDip());
    m_scrollX = std::clamp(m_scrollX, 0.0f, std::max(0.0f, ContentWidth() + 4 - RowsWidth()));
    if (m_source && m_hot != SIZE_MAX && m_hot >= m_source->Count()) m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    m_hotGroupHeader = SIZE_MAX;
    m_hotLinkGroup = SIZE_MAX;
    PaintStatus();
    PaintHeader();
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

void ListView::ScrollTo(size_t top) {
    top = std::min(top, MaxTop());
    if (top == m_top && m_topOffsetDip == 0) return;
    m_top = top;
    m_topOffsetDip = 0;
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

void ListView::CheckNearEnd() {
    if (!onNearEnd || !m_source || m_source->Count() == 0) return;
    const size_t lines = LineCount();
    if (lines == 0) return;
    if (m_top + VisibleLines() + 3 >= lines) onNearEnd();
}

void ListView::EnsureVisible(size_t row) {
    if (!m_source || row >= m_source->Count()) return;
    const size_t rowLine = LineOfRow(row);
    size_t line = rowLine;
    // Bring a group's header into view together with its first track.
    if (m_mode == Mode::Grouped && rowLine > 0 && rowLine < m_lines.size() &&
        m_lines[rowLine - 1].kind == Line::GroupHeader)
        line = rowLine - 1;
    if (Lined() && !m_lineTop.empty()) {
        const float viewH = RowsHeightDip();
        const float top = ContentTopDip();
        // Visibility is decided by the row itself, not by the group header: a header at the
        // bottom edge does not mean the track below it is visible.
        const float rowTop = LineTopDip(rowLine);
        const float rowBottom = LineTopDip(rowLine + 1);
        if (rowTop >= top && rowBottom <= top + viewH) return;
        if (rowTop < top) {
            ScrollTo(line);
            return;
        }
        // Scroll the smallest amount that puts the row's bottom at the view's bottom.
        auto it = std::lower_bound(m_lineTop.begin(), m_lineTop.end(), rowBottom - viewH);
        const size_t target = it == m_lineTop.end() ? m_lines.size() : static_cast<size_t>(it - m_lineTop.begin());
        ScrollTo(std::min(target, rowLine));
        return;
    }
    const size_t v = std::max<size_t>(VisibleLines(), 1);
    const size_t last = LineOfRow(row);
    if (line < m_top)
        ScrollTo(line);
    else if (last >= m_top + v)
        ScrollTo(last - v + 1);
}

size_t ListView::TopRow() const {
    if (!m_source || m_source->Count() == 0) return SIZE_MAX;
    switch (m_mode) {
    case Mode::Tiles:
        if (!m_tileGroups) return std::min(m_top * TileColumns(), m_source->Count() - 1);
        [[fallthrough]];
    case Mode::Grouped:
        for (size_t line = m_top; line < m_lines.size(); ++line)
            if (m_lines[line].kind == Line::Item && m_lines[line].row < m_source->Count())
                return m_lines[line].row;
        return SIZE_MAX;
    default: return m_top < m_source->Count() ? m_top : SIZE_MAX;
    }
}

void ListView::ScrollToRow(size_t row) {
    if (!m_source || row >= m_source->Count()) return;
    ScrollTo(LineOfRow(row));
}

void ListView::ScrollToTop() {
    ScrollTopTo(0);
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

float ListView::RowOffset(size_t row) const {
    if (!m_source || row >= m_source->Count()) return 0;
    return LineTopDip(LineOfRow(row)) - ContentTopDip();
}

void ListView::ScrollToRow(size_t row, float offsetDip) {
    if (!m_source || row >= m_source->Count()) return;
    const float top = std::clamp(LineTopDip(LineOfRow(row)) - offsetDip, 0.0f, MaxScrollDip());
    ScrollTopTo(top);
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

}  // namespace glasswave::ui
