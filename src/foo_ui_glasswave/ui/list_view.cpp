#include "pch.h"
#include "ui/list_view.h"
#include "gfx/composition_host.h"
#include "ui/text.h"
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

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

void PlaceVisual(const wuc::Visual& v, float x, float y, float w, float h) {
    v.Offset(float3{x, y, 0});
    v.Size(float2{std::max(w, 0.0f), std::max(h, 0.0f)});
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
}

int ListView::RowHeightPx() const { return static_cast<int>(std::lround(kRowHeight * m_scale)); }
int ListView::HeaderHeightPx() const { return static_cast<int>(std::lround(kHeaderHeight * m_scale)); }
int ListView::ScrollWidthPx() const { return static_cast<int>(std::lround(kScrollWidth * m_scale)); }

RECT ListView::RowsRect() const {
    return {m_bounds.left, m_bounds.top + HeaderHeightPx(), m_bounds.right - ScrollWidthPx(), m_bounds.bottom};
}

size_t ListView::VisibleRows() const {
    const RECT r = RowsRect();
    return static_cast<size_t>(std::max<LONG>(r.bottom - r.top, 0) / std::max(RowHeightPx(), 1));
}

size_t ListView::MaxTop() const {
    const size_t n = m_source ? m_source->Count() : 0, v = VisibleRows();
    return n > v ? n - v : 0;
}

void ListView::SetSource(media::ListSource* source, bool resetScroll) {
    m_source = source;
    m_hot = SIZE_MAX;
    m_anchor = source ? source->Focus() : SIZE_MAX;
    if (resetScroll) m_top = 0;
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
    m_top = std::min(m_top, MaxTop());
    if (m_source && m_hot != SIZE_MAX && m_hot >= m_source->Count()) m_hot = SIZE_MAX;
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
    const size_t v = std::max<size_t>(VisibleRows(), 1);
    if (row < m_top)
        ScrollTo(row);
    else if (row >= m_top + v)
        ScrollTo(row - v + 1);
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

// ---- Painting -------------------------------------------------------------------------------

void ListView::PaintHeader() {
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = HeaderHeightPx();
    const float s = m_scale;
    host.Repaint(m_header, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        wp::HeaderBackground(dc, size);
        if (!m_source) return;
        winrt::com_ptr<ID2D1SolidColorBrush> ink;
        dc->CreateSolidColorBrush(Hex(0x4C607A), ink.put());
        const auto widths = ColumnWidths(size.width - kScrollWidth);
        float x = kGutter;
        const auto& cols = m_source->Columns();
        for (size_t i = 0; i < cols.size(); ++i) {
            auto layout = MakeText(cols[i].title, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, widths[i] - 2 * kCellPad,
                                   size.height,
                                   cols[i].rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            dc->DrawTextLayout({x + kCellPad, 0}, layout.get(), ink.get());
            x += widths[i];
            wp::HeaderSeparator(dc, x, size.height);
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
        winrt::com_ptr<ID2D1SolidColorBrush> primary, secondary, playing, grey;
        dc->CreateSolidColorBrush(Hex(0x1E1E1E), primary.put());
        dc->CreateSolidColorBrush(Hex(0x4A4A4A), secondary.put());
        dc->CreateSolidColorBrush(Hex(0x1E5BA8), playing.put());
        dc->CreateSolidColorBrush(Hex(0x7A7A7A), grey.put());

        const size_t n = m_source->Count();
        if (n == 0) {
            auto layout = MakeText(m_source->EmptyText(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, size.width - 40, 40,
                                   DWRITE_TEXT_ALIGNMENT_CENTER);
            dc->DrawTextLayout({20, 20}, layout.get(), grey.get());
            return;
        }
        const auto widths = ColumnWidths(size.width);
        const size_t focus = m_source->Focus();
        const bool paused = playback_control::get()->is_paused();
        const size_t visible = VisibleRows() + 1;
        const float rowH = RowHeightPx() / s;  // whole device pixels, matches hit testing
        for (size_t i = 0; i < visible && m_top + i < n; ++i) {
            const size_t row = m_top + i;
            const float y = i * rowH;
            const bool sel = m_source->IsSelected(row), hot = row == m_hot;
            const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                        : (hot ? wp::RowState::Hot : wp::RowState::Normal);
            wp::RowPlate(dc, {2, y + 1, size.width - 2, y + rowH}, st, m_focused);
            if (m_focused && row == focus && !sel) {
                winrt::com_ptr<ID2D1SolidColorBrush> dot;
                dc->CreateSolidColorBrush(Hex(0x7DA2CE, 0.6f), dot.put());
                dc->DrawRoundedRectangle({{2.5f, y + 1.5f, size.width - 2.5f, y + rowH - 0.5f}, 2, 2}, dot.get(),
                                         1.0f);
            }
            const bool isPlaying = m_source->IsPlaying(row);
            if (isPlaying) wp::PlayingMarker(dc, {kGutter / 2 + 1, y + rowH / 2 + 0.5f}, paused);
            float x = kGutter;
            const auto& cols = m_source->Columns();
            for (size_t c = 0; c < cols.size(); ++c) {
                const std::wstring text = m_source->Cell(row, c);
                if (!text.empty()) {
                    auto layout = MakeText(text, 12.0f, isPlaying && c == 0 ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                                                            : DWRITE_FONT_WEIGHT_NORMAL,
                                           widths[c] - 2 * kCellPad, rowH,
                                           cols[c].rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                              : DWRITE_TEXT_ALIGNMENT_LEADING);
                    ID2D1Brush* b = isPlaying ? playing.get() : (c == 0 ? primary.get() : secondary.get());
                    dc->DrawTextLayout({x + kCellPad, y + 0.5f}, layout.get(), b);
                }
                x += widths[c];
            }
        }
    });
}

bool ListView::ThumbGeometry(float* top, float* height) const {
    const size_t n = m_source ? m_source->Count() : 0, v = VisibleRows();
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

ListView::Part ListView::PartAt(POINT pt, size_t* row) const {
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
    const size_t r = m_top + static_cast<size_t>((pt.y - rr.top) / std::max(RowHeightPx(), 1));
    if (m_source && r < m_source->Count()) {
        if (row) *row = r;
        return Part::Row;
    }
    return Part::Empty;
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
    size_t row = SIZE_MAX;
    const Part part = PartAt(pt, &row);
    const size_t hot = part == Part::Row ? row : SIZE_MAX;
    const bool thumbHot = part == Part::Thumb;
    if (hot != m_hot) {
        m_hot = hot;
        PaintRows();
    }
    if (thumbHot != m_thumbHot) {
        m_thumbHot = thumbHot;
        PaintScrollBar();
    }
}

void ListView::OnMouseLeave() {
    if (m_dragThumb) return;
    if (m_hot != SIZE_MAX) {
        m_hot = SIZE_MAX;
        PaintRows();
    }
    if (m_thumbHot) {
        m_thumbHot = false;
        PaintScrollBar();
    }
}

void ListView::OnLButtonDown(POINT pt, bool ctrl, bool shift) {
    size_t row = SIZE_MAX;
    switch (PartAt(pt, &row)) {
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
            const size_t page = std::max<size_t>(VisibleRows(), 1) - 1;
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
    size_t row = SIZE_MAX;
    if (PartAt(pt, &row) == Part::Row && m_source) {
        m_source->Activate(row);
        PaintRows();
    }
}

void ListView::OnRButtonUp(HWND owner, POINT pt) {
    if (!m_source) return;
    size_t row = SIZE_MAX;
    if (PartAt(pt, &row) == Part::Row && !m_source->IsSelected(row)) {
        m_source->SelectOnly(row);
        m_source->SetFocus(row);
        m_anchor = row;
        PaintRows();
    }
    POINT screen = pt;
    ClientToScreen(owner, &screen);
    m_source->ContextMenu(owner, screen);
    Refresh();
}

void ListView::OnWheel(int delta) {
    const int lines = 3 * (-delta / WHEEL_DELTA);
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
    const size_t page = std::max<size_t>(VisibleRows(), 2) - 1;
    auto go = [&](size_t row) {
        if (n == 0) return;
        MoveFocus(std::min(row, n - 1), false, shift);
    };
    switch (vk) {
    case VK_UP: go(focus == SIZE_MAX ? 0 : (focus > 0 ? focus - 1 : 0)); return true;
    case VK_DOWN: go(focus == SIZE_MAX ? 0 : focus + 1); return true;
    case VK_PRIOR: go(focus == SIZE_MAX ? 0 : (focus > page ? focus - page : 0)); return true;
    case VK_NEXT: go(focus == SIZE_MAX ? 0 : focus + page); return true;
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
