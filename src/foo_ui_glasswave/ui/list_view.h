#pragma once
#include "media/list_sources.h"

namespace glasswave::ui {

// Virtualized WMP 12 style list: column header, rows painted on demand into one
// viewport-sized surface (only visible rows are formatted), slim scrollbar.
//
// Three layouts, chosen by the source:
//  - plain details (playlists);
//  - details grouped by album (library songs): a header line per album, the tracks next
//    to an album column with the cover and album / artist / genre / year;
//  - icons (albums, artists, genres): a grid of tiles with covers.
// The view scrolls by "lines": rows, group lines (header / track / filler) or tile rows.
class ListView {
public:
    explicit ListView(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~ListView();

    void SetSource(media::ListSource* source, bool resetScroll);
    media::ListSource* Source() const { return m_source; }
    void SetBounds(const RECT& px, float scale);
    const RECT& Bounds() const { return m_bounds; }
    void SetFocused(bool focused);
    void Refresh();  // data changed: clamp scroll and repaint
    void EnsureVisible(size_t row);

    bool HitTest(POINT pt) const { return PtInRect(&m_bounds, pt) != FALSE; }
    bool Capturing() const { return m_dragThumb; }
    void OnMouseMove(POINT pt);
    void OnMouseLeave();
    void OnLButtonDown(POINT pt, bool ctrl, bool shift);
    void OnLButtonUp(POINT pt);
    void OnDoubleClick(POINT pt);
    void OnRButtonUp(HWND owner, POINT pt);
    void OnWheel(int delta);
    bool OnKeyDown(UINT vk, bool ctrl, bool shift);

private:
    enum class Mode { Plain, Grouped, Tiles };
    enum class Part { None, Header, Row, AlbumColumn, Empty, Track, Thumb };
    struct Line {
        enum Kind : uint8_t { GroupHeader, Item, Filler } kind = Item;
        size_t row = SIZE_MAX;  // Item
        size_t group = 0;       // Grouped mode
        size_t sub = 0;         // line index inside the group's block (Item / Filler)
    };

    Part PartAt(POINT pt, size_t* row, size_t* group = nullptr) const;
    void BuildLines();
    int RowHeightPx() const;
    int LineHeightPx() const;
    int HeaderHeightPx() const;
    int ScrollWidthPx() const;
    RECT RowsRect() const;
    size_t LineCount() const;
    size_t LineOfRow(size_t row) const;
    size_t TileColumns() const;
    D2D1_SIZE_F TileSize() const;  // DIP
    size_t VisibleLines() const;
    size_t MaxTop() const;
    void ScrollTo(size_t top);
    void MoveFocus(size_t row, bool ctrl, bool shift);
    void PaintHeader();
    void PaintRows();
    void PaintScrollBar();
    void PaintPlain(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
    void PaintGrouped(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
    void PaintTiles(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
    void PaintCells(ID2D1DeviceContext* dc, size_t row, float y, float rowH, const std::vector<float>& widths,
                    size_t firstColumn);
    void DrawArt(ID2D1DeviceContext* dc, const metadb_handle_ptr& track, D2D1_RECT_F box);
    std::vector<float> ColumnWidths(float total) const;
    bool ThumbGeometry(float* top, float* height) const;  // in px, relative to the scrollbar

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_header{nullptr}, m_rows{nullptr}, m_scroll{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_rowsSurface{nullptr};
    SIZE m_rowsSurfaceSize{};

    media::ListSource* m_source = nullptr;
    Mode m_mode = Mode::Plain;
    std::vector<Line> m_lines;     // Grouped mode
    std::vector<size_t> m_rowLine;  // Grouped mode: line of each row
    std::vector<size_t> m_groupLine;  // Grouped mode: header line of each group
    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_focused = true;
    size_t m_top = 0;  // first visible line
    size_t m_hot = SIZE_MAX;
    size_t m_hotGroup = SIZE_MAX;
    size_t m_anchor = SIZE_MAX;
    bool m_thumbHot = false, m_dragThumb = false;
    int m_dragOffset = 0;
    int m_artToken = 0;
};

}  // namespace glasswave::ui
