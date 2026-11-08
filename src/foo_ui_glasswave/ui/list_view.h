#pragma once
#include "media/list_sources.h"

namespace glasswave::ui {

// Virtualized WMP 12 style list: column header, rows painted on demand into one
// viewport-sized surface (only visible rows are formatted), slim scrollbar.
//
// Three layouts, chosen by the source:
//  - plain details (playlists);
//  - details grouped by album (library songs): a header line per album and, in the
//    "expanded tile" style, an album column with the cover and album / artist / genre /
//    year beside the tracks; the album, artist and genre lines are links;
//  - icons (albums, artists, genres): a grid of tiles with covers.
// The view scrolls by "lines": rows, group lines (header / track / filler) or tile rows.
// Columns can be resized (drag a header edge) and reordered (drag a header); the layout
// is remembered per column set.
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
    // The first item row visible at or after the top line (SIZE_MAX when the view is empty).
    size_t TopRow() const;
    void ScrollToRow(size_t row);

    bool HitTest(POINT pt) const { return PtInRect(&m_bounds, pt) != FALSE; }
    bool Capturing() const { return m_drag != Drag::None || m_scrollPressed != 0; }
    LPCWSTR CursorAt(POINT pt) const;  // system cursor id, or null for the default
    void OnMouseMove(POINT pt);
    void OnMouseLeave();
    void OnLButtonDown(POINT pt, bool ctrl, bool shift);
    void OnLButtonUp(POINT pt);
    void CancelDrag();
    void OnDoubleClick(POINT pt);
    void OnRButtonUp(HWND owner, POINT pt);
    void OnWheel(int delta, bool horizontal);  // Shift+wheel / tilt wheel scroll sideways
    bool OnKeyDown(UINT vk, bool ctrl, bool shift);

    // A link in the album column was clicked: group index and line (0 album, 1 artist, 2 genre).
    std::function<void(size_t group, size_t line)> onLink;
    // The visible bottom has reached the last lines (used to load the next album page).
    std::function<void()> onNearEnd;
    // Right-click on empty space: the page's own actions (follow, collect, ...).
    std::function<void(POINT screen)> onEmptyContext;
    // Start the window's fade/repeat timers when a scrollbar state changes / is held.
    std::function<void()> onScrollAnim;
    std::function<void()> onScrollHold;
    bool AnimateScrollBar();  // one fade tick; true while a transition is running
    bool OnScrollRepeat();    // one auto-repeat tick; true while a press should continue

private:
    enum class Mode { Plain, Grouped, Tiles };
    enum class Part { None, HeaderCell, HeaderEdge, Row, GroupHeader, AlbumColumn, Link, Empty, ArrowUp, ArrowDown, Track, Thumb };
    enum class Drag { None, Thumb, Resize, PendingMove, Move, Marquee };
    struct Line {
        enum Kind : uint8_t { GroupHeader, Item, Filler } kind = Item;
        size_t row = SIZE_MAX;  // Item
        size_t group = 0;       // Grouped mode
        size_t sub = 0;         // line index inside the group's block (Item / Filler)
    };
    struct Col {
        size_t src;  // index into Source()->Columns()
        float x, w;  // DIP, from the list's left edge
    };
    struct Hit {
        Part part = Part::None;
        size_t row = SIZE_MAX, group = SIZE_MAX, line = 0, col = 0;  // col: display index
    };

    Hit HitAt(POINT pt) const;
    void BuildLines();
    void LoadColumnLayout();
    void SaveColumnLayout() const;
    void ShowHeaderMenu(HWND owner, POINT pt, size_t displayCol);
    void SortBy(size_t srcColumn);
    // Columns have their own widths (none stretches to the window); x is in content DIP,
    // from the list's left edge before horizontal scrolling.
    std::vector<Col> Columns(float total) const;
    float ContentWidth() const;  // right end of the last column (DIP)
    float ScrollX() const { return m_mode == Mode::Tiles ? 0.0f : m_scrollX; }
    void ScrollXTo(float x);
    // A point in content coordinates: x with the horizontal scroll, y from the first line.
    D2D1_POINT_2F ContentPoint(POINT pt) const;
    void UpdateMarquee();
    bool AlbumColumn() const { return m_mode == Mode::Grouped && m_groupArt; }
    // A visible cover column makes the detail rows taller (the covers are drawn as tall as
    // the row allows). Grouped Details has no album column, so it grows too; Expanded tile
    // keeps WMP's height because its album column already shows art.
    bool ArtColumnVisible() const;
    float ArtSize() const;
    float AlbumTextLeft(float albumLeft) const;
    int RowHeightPx() const;
    int LineHeightPx() const;
    // Grouped lines have their own height: section titles carry no cover and keep the plain
    // row height even when the cover column makes the track rows taller.
    float LineHeightDip(const Line& line) const;
    float LineTopDip(size_t line) const;  // from the first line
    size_t LineAtDip(float y) const;
    float RowsHeightDip() const;
    int HeaderHeightPx() const;
    int ScrollWidthPx() const;
    RECT RowsRect() const;
    float RowsWidth() const;  // DIP
    size_t LineCount() const;
    size_t LineOfRow(size_t row) const;
    size_t TileColumns() const;
    D2D1_SIZE_F TileSize() const;  // DIP
    size_t VisibleLines() const;
    size_t MaxTop() const;
    void ScrollTo(size_t top);
    void CheckNearEnd();
    void MoveFocus(size_t row, bool ctrl, bool shift);
    void PaintHeader();
    void PaintRows();
    void PaintScrollBar();
    void PaintPlain(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
    void PaintGrouped(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
    void PaintTiles(ID2D1DeviceContext* dc);
    void PaintCells(ID2D1DeviceContext* dc, size_t row, float y, float rowH, const std::vector<Col>& cols);
    void DrawArt(ID2D1DeviceContext* dc, const std::wstring& key, const metadb_handle_ptr& track, D2D1_RECT_F box);
    void DrawArt(ID2D1DeviceContext* dc, const metadb_handle_ptr& track, D2D1_RECT_F box) {
        DrawArt(dc, std::wstring(), track, box);
    }
    bool ThumbGeometry(float* top, float* height) const;  // in px, relative to the scrollbar

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_header{nullptr}, m_rows{nullptr}, m_scroll{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_rowsSurface{nullptr};
    SIZE m_rowsSurfaceSize{};

    media::ListSource* m_source = nullptr;
    Mode m_mode = Mode::Plain;
    bool m_groupArt = false;
    std::vector<Line> m_lines;        // Grouped mode
    std::vector<float> m_lineTop;     // Grouped mode: DIP top of each line, plus the end
    std::vector<size_t> m_rowLine;    // Grouped mode: line of each row
    std::vector<size_t> m_groupLine;  // Grouped mode: header line of each group

    // Column layout of the current column set (source column indices / DIP widths, < 0 = default)
    std::wstring m_columnKey;
    std::vector<size_t> m_order;
    std::vector<float> m_widths;
    std::vector<uint8_t> m_hidden;  // per source column

    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_focused = true;
    size_t m_top = 0;  // first visible line
    size_t m_hot = SIZE_MAX;
    size_t m_hotGroup = SIZE_MAX;
    size_t m_hotGroupHeader = SIZE_MAX;  // group bar under the mouse: outline only, no fill
    size_t m_hotLinkGroup = SIZE_MAX, m_hotLinkLine = 0;
    size_t m_hotHeader = SIZE_MAX;  // display index of the hovered header cell
    size_t m_anchor = SIZE_MAX;
    int m_scrollHot = 0;      // 1 up, 2 down, 3 track, 4 thumb
    int m_scrollPressed = 0;
    int m_scrollTrackDir = 0;  // track click above (1) or below (2) the thumb
    POINT m_scrollPt{};
    std::array<float, 5> m_scrollBlend{};  // 0 normal .. 1 hot .. 2 pressed
    Drag m_drag = Drag::None;
    int m_dragOffset = 0;   // thumb
    size_t m_dragCol = 0;   // display index
    int m_dragStartX = 0, m_dragX = 0;
    float m_dragStartW = 0;
    float m_scrollX = 0;  // DIP, horizontal scroll of the detail layouts
    int m_hotStar = -1;   // star under the mouse in the hot row's rating cell
    std::unordered_set<const void*> m_favorites;  // tracks in the Favorites playlist (per paint)
    int CellAt(POINT pt, size_t* srcColumn) const;  // CellKind of the column under pt
    int StarAt(POINT pt) const;                     // 0..4 over a rating cell, else -1
    D2D1_POINT_2F m_marqueeFrom{}, m_marqueeTo{};  // content coordinates
    std::vector<size_t> m_marqueeBase;              // selection kept with Ctrl
    int m_artToken = 0;
    // Decoded covers, kept across paints (bounded): a new cover arriving repaints the whole
    // view, and recreating every already-visible D2D bitmap each time made the list stutter.
    struct ArtBitmap {
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        uint64_t used = 0;
        uint64_t version = 0;  // ArtCache::Version when this bitmap was decoded
    };
    std::map<std::wstring, ArtBitmap> m_artBitmaps;
    uint64_t m_artClock = 0;
};

}  // namespace glasswave::ui
