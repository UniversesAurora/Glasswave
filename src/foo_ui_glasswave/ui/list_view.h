#pragma once
#include "media/list_sources.h"

namespace glasswave::ui {

// Virtualized WMP 12 style list: column header, rows painted on demand into one
// viewport-sized surface (only visible rows are formatted), slim scrollbar.
class ListView {
public:
    explicit ListView(const winrt::Windows::UI::Composition::ContainerVisual& parent);

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
    enum class Part { None, Header, Row, Empty, Track, Thumb };
    Part PartAt(POINT pt, size_t* row) const;
    int RowHeightPx() const;
    int HeaderHeightPx() const;
    int ScrollWidthPx() const;
    RECT RowsRect() const;
    size_t VisibleRows() const;
    size_t MaxTop() const;
    void ScrollTo(size_t top);
    void MoveFocus(size_t row, bool ctrl, bool shift);
    void PaintHeader();
    void PaintRows();
    void PaintScrollBar();
    std::vector<float> ColumnWidths(float total) const;
    bool ThumbGeometry(float* top, float* height) const;  // in px, relative to the scrollbar

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_header{nullptr}, m_rows{nullptr}, m_scroll{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_rowsSurface{nullptr};
    SIZE m_rowsSurfaceSize{};

    media::ListSource* m_source = nullptr;
    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_focused = true;
    size_t m_top = 0;
    size_t m_hot = SIZE_MAX;
    size_t m_anchor = SIZE_MAX;
    bool m_thumbHot = false, m_dragThumb = false;
    int m_dragOffset = 0;
};

}  // namespace glasswave::ui
