#pragma once
#include "core/signal.h"
#include "media/list_sources.h"

#include <memory>

namespace glasswave::ui {

class CardLayer;

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
    // Scrolled to the very top (a page shown again in another layout stays there).
    bool AtTop() const { return ContentTopDip() <= 0.5f; }
    void ScrollToTop();
    void ScrollToRow(size_t row);
    // DIP from the top of the view to the top of the row's line (negative above it), and back:
    // the row put that far down again, so whatever stood above it (an album's header, the
    // view's top) stays where it was while rows around it change.
    float RowOffset(size_t row) const;
    void ScrollToRow(size_t row, float offsetDip);
    // Where each sideways shelf stands (columns, by the group's key), to come back to on Back; a
    // shelf that arrives later takes its place then.
    std::map<std::wstring, float> ShelfPositions() const;
    void SetShelfPositions(const std::map<std::wstring, float>& positions);

    bool HitTest(POINT pt) const { return PtInRect(&m_bounds, pt) != FALSE; }
    bool Capturing() const { return m_drag != Drag::None || m_scrollPressed != 0; }
    LPCWSTR CursorAt(POINT pt) const;  // system cursor id, or null for the default
    // Text shown cut short under the pointer: its whole text and its box in client px.
    bool CutTextAt(POINT pt, std::wstring* text, RECT* rect) const;
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
    // Called after SetSource; the window restores a saved scroll position when possible.
    std::function<void()> onSourceChanged;
    // Sorting or grouping changed the presented list; the page may need another drain mode.
    std::function<void()> onPresentationChanged;
    // Start the window's fade/repeat timers when a scrollbar state changes / is held.
    std::function<void()> onScrollAnim;
    std::function<void()> onScrollHold;
    bool AnimateScrollBar();  // one fade tick; true while a transition is running
    // Whether the window can be seen: the cards' motion covers play only then.
    void SetMotion(bool on);
    bool OnScrollRepeat();    // one auto-repeat tick; true while a press should continue

private:
    enum class Mode { Plain, Grouped, Tiles };
    enum class Part {
        None,
        Status,
        HeaderCell,
        HeaderEdge,
        Row,
        GroupHeader,
        AlbumColumn,
        Link,
        Empty,
        ArrowUp,
        ArrowDown,
        Track,
        Thumb,
        Carousel, // a shelf's arrow: Hit::line 0 left, 1 right
        TilePlay  // a tile's play button
    };
    enum class Drag { None, Thumb, Resize, PendingMove, Move, Marquee, MoreButton, ShelfButton, CardPress, PlayPress };
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
    int StatusHeightPx() const;
    int ContentOffsetPx() const;
    int ScrollWidthPx() const;
    RECT RowsRect() const;
    float RowsWidth() const;  // DIP
    size_t LineCount() const;
    size_t LineOfRow(size_t row) const;
    size_t TileColumns() const;
    D2D1_SIZE_F TileSize() const;  // DIP
    float TilePitch() const;            // DIP from one tile column to the next
    float TileLeft(size_t column) const; // DIP, the tile's left edge
    // The same for a group's own tile style (None: the source's).
    media::TileStyle GroupTiles(size_t group) const;
    D2D1_SIZE_F TileSizeOf(media::TileStyle style) const;
    size_t TileColumnsOf(media::TileStyle style) const;
    // shelf: a shelf's (carousel) tiles keep their width; elsewhere the spare width spreads.
    float TilePitchOf(media::TileStyle style, bool shelf = false) const;
    float TileLeftOf(media::TileStyle style, float column, bool shelf = false) const;
    // A shelf (RowGroup::carousel): its columns, how far it is paged (in columns, animated),
    // its arrows (DIP in the rows' space, between the shelf's top and bottom).
    size_t ShelfColumns(size_t group) const;
    float ShelfPos(size_t group) const;
    D2D1_RECT_F ShelfArrow(size_t group, bool right, float top, float bottom) const;
    bool ShelfCanPage(size_t group, bool right) const;
    void PageShelf(size_t group, bool right);
    // Moves a shelf to `to` (columns), from where it shows, over ms (a page, a wheel's step).
    void SlideShelf(size_t group, float to, int ms);
    float ShelfVisible(size_t group) const; // columns the view holds, a part counted as such
    float ShelfLast(size_t group) const;    // the farthest it goes: its last tile at the right edge
    float ShelfShownPos(size_t group) const;
    struct ShelfSlide {
        size_t group = SIZE_MAX;
        float from = 0, to = 0;
        std::chrono::steady_clock::time_point start{};
        int ms = 0;
    } m_shelfSlide;
    // The play button over a tile's cover (DIP, the tile at x, y).
    D2D1_RECT_F TilePlayRect(media::TileStyle style, float x, float y) const;
    // The tile under a point of a tile line: its row, and its left edge (DIP).
    size_t TileAtLine(const Line& line, float x, float* left) const;
    size_t VisibleLines() const;
    size_t MaxTop() const;
    void ScrollTo(size_t top);
    void CheckNearEnd();
    void MoveFocus(size_t row, bool ctrl, bool shift);
    void UpdateStatusText();
    float ContentTopDip() const;
    void ScrollTopTo(float top);
    // Pixel scrolling (owner, 2026-11-20: tall rows of cards skipped by a notch, a thumb that
    // changed size over them; "smooth as Edge, at the display's refresh rate"). The scroll position
    // is the content's top in DIP. The rows, the cards and a sliding shelf lie in m_content inside
    // the clipping m_viewport; the wheel glides there: the rows are painted for the whole way and
    // the compositor moves m_content, so the motion runs at the display's rate with no repaint.
    float ContentHeightDip() const;
    float MaxScrollDip() const;
    void SetScrollDip(float top); // at once
    void ScrollBy(float dip) { SetScrollDip(ContentTopDip() + dip); }
    void GlideTo(float top);      // the wheel's glide
    bool Gliding() const;
    float VisualTopDip() const;   // where the content stands on screen (mid-glide)
    winrt::Windows::UI::Composition::ContainerVisual m_viewport{nullptr}, m_content{nullptr};
    // The scroll bar's thumb, a visual of its own whose place the compositor computes from
    // m_content's offset (an expression over m_thumbPlace), so it glides with the rows.
    winrt::Windows::UI::Composition::SpriteVisual m_thumb{nullptr};
    winrt::Windows::UI::Composition::CompositionPropertySet m_thumbPlace{nullptr};
    void PlaceThumb(); // the expression's terms for the current paint and scroll
    float m_paintTop = 0, m_paintHeight = 0; // DIP: the content the rows surface holds
    float m_paintViewH = -1;                 // while painting: that height (RowsHeightDip)
    struct Glide {
        float from = 0, to = 0;
        std::chrono::steady_clock::time_point start{};
        int ms = 0; // 0: none
    } m_glide;
    void PaintStatus();
    void PaintHeader();
    void PaintRows();
    void NoteGlow();
    void PaintScrollBar();
    void PaintPlain(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
    void PaintGrouped(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
    void PaintTiles(ID2D1DeviceContext* dc);
    void PaintTile(ID2D1DeviceContext* dc, size_t idx, float x, float y, media::TileStyle style, size_t focus);
    void PaintShelfArrows(ID2D1DeviceContext* dc);
    void PaintCells(ID2D1DeviceContext* dc, size_t row, float y, float rowH, const std::vector<Col>& cols);
    float PaintBadge(ID2D1DeviceContext* dc, const std::wstring& text, float x, float cy, float right);
    float PaintBadges(ID2D1DeviceContext* dc, const std::vector<std::wstring>& badges, float x, float cy, float right);
    void DrawArt(ID2D1DeviceContext* dc, const std::wstring& key, const metadb_handle_ptr& track, D2D1_RECT_F box);
    void DrawArt(ID2D1DeviceContext* dc, const metadb_handle_ptr& track, D2D1_RECT_F box) {
        DrawArt(dc, std::wstring(), track, box);
    }
    bool ThumbGeometry(float* top, float* height) const;  // in px, relative to the scrollbar

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::CompositionRectangleGeometry m_clipGeometry{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_header{nullptr}, m_status{nullptr}, m_rows{nullptr},
        m_scroll{nullptr};
    struct RowsBuffer {
        winrt::Windows::UI::Composition::CompositionDrawingSurface surface{nullptr};
        winrt::Windows::UI::Composition::CompositionSurfaceBrush brush{nullptr};
        SIZE size{};
        int viewPx = 0;
    };
    RowsBuffer m_rowsBuffers[2]; // drawn in turn (PaintRows)
    int m_rowsFront = 0;         // the one on screen

    media::ListSource* m_source = nullptr;
    Mode m_mode = Mode::Plain;
    bool m_groupArt = false;
    // Tiles mode with groups (an artist's overview): a header line per group, then rows of
    // tiles (Line::Item, `row` the row's first tile), heights in m_lineTop like Grouped.
    bool m_tileGroups = false;
    bool Lined() const { return m_mode == Mode::Grouped || m_tileGroups; } // m_lines / m_lineTop in use
    size_t TileGroupEnd(size_t group) const; // one past the group's last row shown as a tile
    bool ShowsMore(size_t group) const;
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
    float m_topOffsetDip = 0; // pixel scroll into m_top, used to anchor status-band changes
    std::wstring m_statusText;
    size_t m_hot = SIZE_MAX;
    bool m_hotDownload = false; // the mouse is on the hot row's download mark
    float m_downloadHot = 0; // the hot row's mark lighting under the mouse
    size_t m_hotGroup = SIZE_MAX;
    size_t m_hotGroupHeader = SIZE_MAX;  // group bar under the mouse: outline only, no fill
    size_t m_hotLinkGroup = SIZE_MAX, m_hotLinkLine = 0;
    // Shelves paged sideways, by group key: where each stands and its slide (ease-out).
    struct Shelf {
        float pos = 0, from = 0, target = 0;
        ULONGLONG start = 0;
    };
    std::map<std::wstring, Shelf> m_shelves;
    // The shelf sliding now: its tiles drawn into m_slide, moved by the compositor.
    winrt::Windows::UI::Composition::ContainerVisual m_slideClip{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_slide{nullptr};
    std::wstring m_slideKey; // empty: none
    ULONGLONG m_slideEnd = 0;
    float m_slideTop = 0; // the content's scroll when it started
    void FinishSlide();
    // The shelves of cards (TileStyle::Card) on screen, given whole to the card layer; a shelf
    // paging (m_cardSlideGroup, set before the repaint) slides from m_cardSlideFrom (columns).
    void LayoutCards();
    size_t m_cardSlideGroup = SIZE_MAX;
    float m_cardSlideFrom = 0;
    int m_cardSlideMs = 0;
    bool IsCard(size_t row) const { return m_source && row != SIZE_MAX && m_source->Card(row) != nullptr; }
    std::unique_ptr<CardLayer> m_cards;
    size_t m_cardPress = SIZE_MAX; // the card held down
    size_t m_playPress = SIZE_MAX; // the tile whose play orb is held down (it plays on release)
    size_t m_hotShelf = SIZE_MAX, m_hotShelfSide = 0; // the arrow under the mouse
    float m_shelfPlate = 0;                           // its plate fading in
    size_t m_shelfPressedGroup = SIZE_MAX, m_shelfPressedSide = 0; // the arrow held down
    float m_shelfPress = 0;                           // its pressed plate fading in
    // Windows 7's progress glow sweeping over running downloads' bars: on while one is painted.
    bool m_glowing = false, m_glowSeen = false;
    ULONGLONG m_glowPainted = 0;
    // A shelf's show-all ball: the one under the mouse lit (and the last one fading), held.
    size_t m_moreGroup = SIZE_MAX, m_moreOldGroup = SIZE_MAX, m_morePressedGroup = SIZE_MAX;
    float m_moreHot = 0, m_moreOldHot = 0, m_morePress = 0;
    std::wstring m_shelfUnder;                        // the shelf under the mouse: arrows shown
    std::map<std::wstring, float> m_shelfShown;       // each shelf's arrows, fading
    // The tile play button: shown on the hot tile, fading in; the last one fading out.
    size_t m_playRow = SIZE_MAX, m_playOldRow = SIZE_MAX;
    float m_playBlend = 0, m_playOldBlend = 0, m_playHotBlend = 0;
    bool m_playHot = false; // the mouse is on the button itself
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
    int CellAt(POINT pt, size_t* srcColumn) const;  // CellKind of the column under pt
    int StarAt(POINT pt) const;                     // 0..4 over a rating cell, else -1
    D2D1_POINT_2F m_marqueeFrom{}, m_marqueeTo{};  // content coordinates
    std::vector<size_t> m_marqueeBase;              // selection kept with Ctrl
    int m_artToken = 0;
    int m_iconToken = 0; // SubscribeSourceIcons
    core::Signal<>::Connection m_cellChanges; // the Download column follows
    // Decoded covers, kept across paints (bounded): a new cover arriving repaints the whole
    // view, and recreating every already-visible D2D bitmap each time made the list stutter.
    struct ArtBitmap {
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        uint64_t used = 0;
        uint64_t version = 0;  // ArtCache::Version when this bitmap was decoded
    };
    std::map<std::wstring, ArtBitmap> m_artBitmaps;
    core::Signal<>::Connection m_deviceReplaced; // the bitmaps belong to the rendering device
    uint64_t m_artClock = 0;
};

}  // namespace glasswave::ui
