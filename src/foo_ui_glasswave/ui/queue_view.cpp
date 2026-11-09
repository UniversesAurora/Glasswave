#include "pch.h"

#include "ui/queue_view.h"

#include "gfx/composition_host.h"
#include "gfx/effects.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"
#include "ui/pane_common.h"

#include <winrt/Windows.Foundation.Collections.h>

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using Clock = std::chrono::steady_clock;

namespace {

// Apple Music's "Playing Next" (iOS 18) measured on a 390 pt screen, scaled to the pane.
constexpr float kRowH = 56;          // DIP
constexpr float kRowCover = 46;      // DIP
constexpr float kCoverRadius = 5;    // DIP
constexpr float kContentX = 20;      // DIP: covers and section titles
constexpr float kSectionH = 48;      // DIP: "History" with Clear
constexpr float kUpNextH = 58;       // DIP: "Continue Playing" with its subtitle
constexpr float kCapsuleH = 34;      // DIP
constexpr float kCapsuleGap = 10;    // DIP
constexpr float kHandleW = 18;       // DIP: the reorder handle's three lines
constexpr float kPlateRadius = 10;   // DIP
constexpr float kHoverAlpha = 0.08f, kPressAlpha = 0.04f;
constexpr float kTopFade = 16, kBottomFade = 40;  // DIP
constexpr float kRubber = 0.55f;     // iOS rubber band constant
constexpr SpringParams kRowSpring{1, 300, 30};
constexpr SpringParams kScrollSpring{1, 220, 30};

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// iOS rubber band: the further past the end, the less the content follows.
float Rubber(float overshoot, float dimension) {
    if (dimension <= 0) return 0;
    return (1.0f - 1.0f / (overshoot * kRubber / dimension + 1.0f)) * dimension;
}

void DrawShuffleGlyph(ID2D1DeviceContext* dc, D2D1_POINT_2F c, ID2D1Brush* ink) {
    com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    const float w = 9, h = 6, stroke = 1.8f;
    auto arrow = [&](D2D1_POINT_2F a, D2D1_POINT_2F b) {
        dc->DrawLine(a, b, ink, stroke);
        dc->DrawLine({b.x - 3, b.y - 2.6f}, b, ink, stroke);
        dc->DrawLine({b.x - 3, b.y + 2.6f}, b, ink, stroke);
    };
    // Two crossing paths with arrowheads on the right.
    com_ptr<ID2D1PathGeometry> path;
    f->CreatePathGeometry(path.put());
    com_ptr<ID2D1GeometrySink> sink;
    path->Open(sink.put());
    sink->BeginFigure({c.x - w, c.y + h}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier({{c.x - 2, c.y + h}, {c.x + 1, c.y - h}, {c.x + w - 1, c.y - h}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({c.x - w, c.y - h}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier({{c.x - 2, c.y - h}, {c.x + 1, c.y + h}, {c.x + w - 1, c.y + h}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    dc->DrawGeometry(path.get(), ink, stroke);
    arrow({c.x + w - 2, c.y - h}, {c.x + w, c.y - h});
    arrow({c.x + w - 2, c.y + h}, {c.x + w, c.y + h});
}

void DrawRepeatGlyph(ID2D1DeviceContext* dc, D2D1_POINT_2F c, ID2D1Brush* ink, bool one) {
    const float w = 8, h = 5, stroke = 1.8f;
    // A loop of two rounded halves with arrowheads (top to the right, bottom to the left).
    dc->DrawLine({c.x - w + 3, c.y - h}, {c.x + w, c.y - h}, ink, stroke);
    dc->DrawLine({c.x + w, c.y - h}, {c.x + w - 3, c.y - h - 2.8f}, ink, stroke);
    dc->DrawLine({c.x + w, c.y - h}, {c.x + w - 3, c.y - h + 2.8f}, ink, stroke);
    dc->DrawLine({c.x - w, c.y + h}, {c.x + w - 3, c.y + h}, ink, stroke);
    dc->DrawLine({c.x - w, c.y + h}, {c.x - w + 3, c.y + h - 2.8f}, ink, stroke);
    dc->DrawLine({c.x - w, c.y + h}, {c.x - w + 3, c.y + h + 2.8f}, ink, stroke);
    dc->DrawLine({c.x - w, c.y + h - 1}, {c.x - w, c.y - h + 2}, ink, stroke);
    dc->DrawLine({c.x + w, c.y - h + 1}, {c.x + w, c.y + h - 2}, ink, stroke);
    if (one) pane::DrawTextLine(dc, L"1", 8, DWRITE_FONT_WEIGHT_BOLD, {c.x - 3, c.y - 6}, 6, ink);
}

}  // namespace

struct QueueView::RowView {
    Row data;
    bool history = false;
    Spring y{0};
    bool placed = false;  // the spring has a position
    float hover = 0;
    wuc::ContainerVisual root{nullptr};
    wuc::ShapeVisual plate{nullptr};
    wuc::CompositionRoundedRectangleGeometry plateShape{nullptr};
    wuc::CompositionColorBrush plateBrush{nullptr};
    wuc::SpriteVisual sprite{nullptr};
    wuc::CompositionDrawingSurface surface{nullptr};
    bool drawn = false, coverDrawn = false;
};

// ---- Construction ----------------------------------------------------------------------

QueueView::QueueView(const wuc::ContainerVisual& parent) {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    m_dc = host.CreateDeviceContext();
    m_root = comp.CreateContainerVisual();
    m_root.IsVisible(false);
    parent.Children().InsertAtTop(m_root);
    m_layer = comp.CreateLayerVisual();
    m_root.Children().InsertAtTop(m_layer);
    try {
        auto mask = gfx::MakeAlphaMaskEffect(wuc::CompositionEffectSourceParameter(L"source"),
                                             wuc::CompositionEffectSourceParameter(L"mask"));
        auto brush = comp.CreateEffectFactory(mask).CreateBrush();
        m_maskSurface = host.CreateSurface(1, 1);
        auto maskBrush = comp.CreateSurfaceBrush(m_maskSurface);
        maskBrush.Stretch(wuc::CompositionStretch::Fill);
        brush.SetSourceParameter(L"mask", maskBrush);
        m_layer.Effect(brush);
    } catch (...) {
        m_maskSurface = nullptr;
    }
    for (auto* v : {&m_historyTitle, &m_upNextTitle, &m_empty, &m_shuffle, &m_repeat}) {
        *v = comp.CreateSpriteVisual();
        m_layer.Children().InsertAtTop(*v);
    }
    m_scroll.SetParams(kScrollSpring);
    m_artToken = ArtCache::Get().Subscribe([this] {
        m_coversDirty = true;
        if (m_visible) StartFrames();
    });
}

QueueView::~QueueView() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    for (auto& r : m_rows) ReleaseRow(*r);
    if (m_root) {
        if (auto parent = m_root.Parent()) parent.Children().Remove(m_root);
    }
}

void QueueView::StartFrames() {
    if (onFrames) onFrames();
}

void QueueView::SetBounds(const RECT& px, float scale) {
    const bool changed = px.right - px.left != m_bounds.right - m_bounds.left ||
                         px.bottom - px.top != m_bounds.bottom - m_bounds.top || scale != m_scale;
    m_bounds = px;
    m_scale = scale;
    m_root.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    if (!changed) return;
    const float w = static_cast<float>(std::max<LONG>(0, px.right - px.left));
    const float h = static_cast<float>(std::max<LONG>(0, px.bottom - px.top));
    m_layer.Size(float2{w, h});
    if (m_maskSurface && h > 0) {
        m_maskSurface.Resize(winrt::Windows::Graphics::SizeInt32{4, static_cast<int32_t>(h)});
        const float H = h / m_scale;
        gfx::CompositionHost::Get().Draw(m_maskSurface, m_scale, [&](ID2D1DeviceContext* dc) {
            const D2D1_GRADIENT_STOP s[] = {{0.0f, D2D1::ColorF(1, 1, 1, 0)},
                                            {std::min(0.45f, kTopFade / std::max(1.0f, H)), D2D1::ColorF(1, 1, 1, 1)},
                                            {std::max(0.55f, 1.0f - kBottomFade / std::max(1.0f, H)), D2D1::ColorF(1, 1, 1, 1)},
                                            {1.0f, D2D1::ColorF(1, 1, 1, 0)}};
            com_ptr<ID2D1GradientStopCollection> stops;
            dc->CreateGradientStopCollection(s, 4, stops.put());
            com_ptr<ID2D1LinearGradientBrush> b;
            dc->CreateLinearGradientBrush({{0, 0}, {0, H}}, stops.get(), b.put());
            dc->FillRectangle({0, 0, 4 / m_scale, H}, b.get());
        });
    }
    for (auto& r : m_rows) ReleaseRow(*r);  // widths changed: redraw
    Layout();
    DrawChrome();
    if (m_visible) Render();
}

void QueueView::SetVisible(bool visible) {
    if (visible == m_visible) return;
    m_visible = visible;
    m_root.IsVisible(visible);
    if (visible) {
        Layout();
        DrawChrome();
        Render();
    } else {
        for (auto& r : m_rows) ReleaseRow(*r);
    }
}

// ---- Data ------------------------------------------------------------------------------

void QueueView::SetRows(std::vector<Row> rows, bool history) {
    // Rows keep their visuals and spring position when the same song is still listed, so a
    // song that starts slides out and the others move up instead of the list jumping.
    std::vector<std::unique_ptr<RowView>> old;
    std::vector<std::unique_ptr<RowView>> keep;
    for (auto& r : m_rows) (r->history == history ? old : keep).push_back(std::move(r));
    std::vector<std::unique_ptr<RowView>> fresh;
    for (auto& data : rows) {
        std::unique_ptr<RowView> view;
        for (auto& o : old)
            if (o && o->data.handle == data.handle) {
                view = std::move(o);
                break;
            }
        if (!view) {
            view = std::make_unique<RowView>();
            view->history = history;
            view->y.SetParams(kRowSpring);
        }
        if (view->data.title != data.title || view->data.artist != data.artist) view->drawn = false;
        view->data = std::move(data);
        fresh.push_back(std::move(view));
    }
    for (auto& o : old)
        if (o) ReleaseRow(*o);
    m_rows.clear();
    if (history) {
        m_historyCount = fresh.size();
        for (auto& r : fresh) m_rows.push_back(std::move(r));
        for (auto& r : keep) m_rows.push_back(std::move(r));
    } else {
        for (auto& r : keep) m_rows.push_back(std::move(r));
        for (auto& r : fresh) m_rows.push_back(std::move(r));
    }
    m_reorder = -1;
    m_hover = m_press = {};
}

void QueueView::SetUpNext(std::vector<Row> rows) {
    SetRows(std::move(rows), false);
    Layout();
    DrawChrome();
    if (m_visible) StartFrames();
}

void QueueView::SetHistory(std::vector<Row> rows) {
    // The card stays where it is when history grows above it.
    const float before = m_cardTop;
    SetRows(std::move(rows), true);
    Layout();
    const float delta = m_cardTop - before;
    if (delta != 0) {
        const float target = m_scroll.Target() + delta;
        m_scroll.Snap(m_scroll.Position() + delta);
        m_scroll.SetTarget(target);
    }
    DrawChrome();
    if (m_visible) StartFrames();
}

void QueueView::SetShuffleRepeat(bool shuffle, int repeat) {
    if (shuffle == m_shuffleOn && repeat == m_repeatMode) return;
    m_shuffleOn = shuffle;
    m_repeatMode = repeat;
    DrawChrome();
}

void QueueView::ScrollToCard() {
    Layout();
    m_scroll.Snap(std::min(m_cardTop, MaxScroll()));
    m_velocity = 0;
}

float QueueView::MaxScroll() const {
    const float viewH = (m_bounds.bottom - m_bounds.top) / m_scale;
    return std::max(m_cardTop, m_contentH - viewH);
}

float QueueView::ShownScroll() const {
    const float s = m_scroll.Position(), max = MaxScroll();
    const float viewH = (m_bounds.bottom - m_bounds.top) / m_scale;
    if (s < 0) return -Rubber(-s, viewH);
    if (s > max) return max + Rubber(s - max, viewH);
    return s;
}

float QueueView::CardTop() const { return m_cardTop - ShownScroll(); }

// ---- Layout and rendering --------------------------------------------------------------

void QueueView::Layout() {
    float y = 8;
    m_historyTop = y;
    size_t i = 0;
    if (m_historyCount > 0) {
        y += kSectionH;
        for (; i < m_historyCount; ++i) {
            auto& r = *m_rows[i];
            if (!r.placed) {
                r.y.Snap(y);
                r.placed = true;
            } else {
                r.y.SetTarget(y);
            }
            y += kRowH;
        }
        y += 12;
    }
    m_cardTop = y;
    y += pane::kHeaderHeight;
    m_capsulesTop = y + 4;
    y = m_capsulesTop + kCapsuleH + 20;
    m_upNextTop = y;
    y += kUpNextH;
    const float first = y;
    const size_t count = m_rows.size() - m_historyCount;
    // While a row is dragged the others make room at its slot.
    const int dragged = m_reorder >= 0 ? m_reorder - static_cast<int>(m_historyCount) : -1;
    for (size_t u = 0; u < count; ++u) {
        auto& r = *m_rows[m_historyCount + u];
        if (static_cast<int>(u) == dragged) {
            r.y.Snap(m_reorderY);
            r.placed = true;
            continue;
        }
        size_t slot = u;
        if (dragged >= 0) {
            if (static_cast<int>(u) > dragged) --slot;
            if (static_cast<int>(slot) >= m_reorderTo) ++slot;
        }
        const float target = first + slot * kRowH;
        if (!r.placed) {
            r.y.Snap(target);
            r.placed = true;
        } else {
            r.y.SetTarget(target);
        }
    }
    m_contentH = first + std::max<size_t>(count, 1) * kRowH + 32;
}

void QueueView::ReleaseRow(RowView& r) {
    if (r.root) {
        if (auto parent = r.root.Parent()) parent.Children().Remove(r.root);
    }
    r.root = nullptr;
    r.plate = nullptr;
    r.plateShape = nullptr;
    r.plateBrush = nullptr;
    r.sprite = nullptr;
    r.surface = nullptr;
    r.drawn = r.coverDrawn = false;
}

void QueueView::DrawRow(RowView& r) {
    auto& host = gfx::CompositionHost::Get();
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    const int w = std::max(1, static_cast<int>(std::ceil(W * m_scale))), h = static_cast<int>(std::ceil(kRowH * m_scale));
    if (!r.surface) {
        r.surface = host.CreateSurface(w, h);
        auto brush = host.Compositor().CreateSurfaceBrush(r.surface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        r.sprite.Brush(brush);
        r.sprite.Size(float2{static_cast<float>(w), static_cast<float>(h)});
    }
    com_ptr<IWICBitmapSource> art;
    const auto state = r.data.handle.is_valid()
                           ? ArtCache::Get().Find(r.data.handle, static_cast<int>(std::ceil(kRowCover * m_scale)), &art)
                           : ArtCache::State::Missing;
    m_dc->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    host.Draw(r.surface, m_scale, m_dc.get(), [&](ID2D1DeviceContext* dc) {
        com_ptr<ID2D1Bitmap> bitmap;
        if (art) dc->CreateBitmapFromWicBitmap(art.get(), nullptr, bitmap.put());
        const float cy = kRowH / 2;
        pane::DrawCover(dc, bitmap.get(), {kContentX, cy - kRowCover / 2, kContentX + kRowCover, cy + kRowCover / 2},
                        kCoverRadius);
        const float textX = kContentX + kRowCover + 12;
        const float right = r.history ? W - kContentX : W - kContentX - kHandleW - 16;
        pane::DrawTextLine(dc, r.data.title, 16, DWRITE_FONT_WEIGHT_NORMAL, {textX, cy - 21}, right - textX,
                           pane::White(dc, 0.95f).get());
        pane::DrawTextLine(dc, r.data.artist, 14, DWRITE_FONT_WEIGHT_NORMAL, {textX, cy + 1}, right - textX,
                           pane::White(dc, 0.6f).get());
        if (!r.history) {
            // Apple Music's reorder handle: three short lines.
            auto ink = pane::White(dc, 0.4f);
            const float x0 = W - kContentX - kHandleW;
            for (int k = -1; k <= 1; ++k)
                dc->DrawLine({x0, cy + k * 5.0f}, {x0 + kHandleW, cy + k * 5.0f}, ink.get(), 1.4f);
        }
    });
    r.drawn = true;
    r.coverDrawn = state != ArtCache::State::Pending;
}

void QueueView::DrawChrome() {
    if (!m_visible || m_bounds.right <= m_bounds.left) return;
    auto& host = gfx::CompositionHost::Get();
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    auto paint = [&](const wuc::SpriteVisual& visual, float wDip, float hDip, const std::function<void(ID2D1DeviceContext*)>& draw) {
        const int w = std::max(1, static_cast<int>(std::ceil(wDip * m_scale))), h = std::max(1, static_cast<int>(std::ceil(hDip * m_scale)));
        host.Repaint(visual, w, h, m_scale, draw);
        visual.Size(float2{static_cast<float>(w), static_cast<float>(h)});
    };
    paint(m_historyTitle, W, kSectionH, [&](ID2D1DeviceContext* dc) {
        pane::DrawTextLine(dc, Tr(L"History"), 20, DWRITE_FONT_WEIGHT_BOLD, {kContentX, 12}, W / 2, pane::White(dc, 0.95f).get());
        pane::DrawTextLine(dc, Tr(L"Clear"), 16, DWRITE_FONT_WEIGHT_NORMAL, {W / 2, 15}, W / 2 - kContentX,
                           pane::White(dc, 0.6f).get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    });
    const size_t upNext = m_rows.size() - m_historyCount;
    paint(m_upNextTitle, W, kUpNextH, [&](ID2D1DeviceContext* dc) {
        pane::DrawTextLine(dc, Tr(L"Continue Playing"), 20, DWRITE_FONT_WEIGHT_BOLD, {kContentX, 6}, W - 2 * kContentX,
                           pane::White(dc, 0.95f).get());
        const std::wstring sub = std::to_wstring(upNext) + Tr(upNext == 1 ? L" song" : L" songs");
        pane::DrawTextLine(dc, sub, 14, DWRITE_FONT_WEIGHT_NORMAL, {kContentX, 33}, W - 2 * kContentX,
                           pane::White(dc, 0.55f).get());
    });
    paint(m_empty, W, kRowH, [&](ID2D1DeviceContext* dc) {
        pane::DrawTextLine(dc, Tr(L"Nothing up next"), 15, DWRITE_FONT_WEIGHT_NORMAL, {kContentX, 18}, W - 2 * kContentX,
                           pane::White(dc, 0.45f).get());
    });
    // Capsules: translucent when off, light with a dark glyph when on (Apple Music).
    const float capW = (W - 2 * kContentX - kCapsuleGap) / 2;
    auto capsule = [&](const wuc::SpriteVisual& visual, bool on, const std::function<void(ID2D1DeviceContext*, ID2D1Brush*)>& glyph) {
        paint(visual, capW, kCapsuleH, [&](ID2D1DeviceContext* dc) {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect({0, 0, capW, kCapsuleH}, kCapsuleH / 2, kCapsuleH / 2);
            dc->FillRoundedRectangle(rr, pane::White(dc, on ? 0.78f : 0.14f).get());
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(on ? D2D1::ColorF(0.18f, 0.16f, 0.18f, 0.9f) : D2D1::ColorF(1, 1, 1, 0.9f), ink.put());
            glyph(dc, ink.get());
        });
    };
    capsule(m_shuffle, m_shuffleOn, [&](ID2D1DeviceContext* dc, ID2D1Brush* ink) {
        DrawShuffleGlyph(dc, {capW / 2, kCapsuleH / 2}, ink);
    });
    capsule(m_repeat, m_repeatMode != 0, [&](ID2D1DeviceContext* dc, ID2D1Brush* ink) {
        DrawRepeatGlyph(dc, {capW / 2, kCapsuleH / 2}, ink, m_repeatMode == 2);
    });
}

void QueueView::RenderRow(RowView& r) {
    const float viewH = (m_bounds.bottom - m_bounds.top) / m_scale;
    const float shown = r.y.Position() - ShownScroll();
    if (shown + kRowH < -viewH || shown > 2 * viewH) {
        if (r.root) ReleaseRow(r);
        return;
    }
    auto& comp = gfx::CompositionHost::Get().Compositor();
    const float W = (m_bounds.right - m_bounds.left) / m_scale, s = m_scale;
    if (!r.root) {
        r.root = comp.CreateContainerVisual();
        r.plate = comp.CreateShapeVisual();
        r.plateShape = comp.CreateRoundedRectangleGeometry();
        r.plateBrush = comp.CreateColorBrush(winrt::Windows::UI::Colors::White());
        auto sprite = comp.CreateSpriteShape(r.plateShape);
        sprite.FillBrush(r.plateBrush);
        r.plate.Shapes().Append(sprite);
        r.sprite = comp.CreateSpriteVisual();
        r.root.Children().InsertAtTop(r.plate);
        r.root.Children().InsertAtTop(r.sprite);
        m_layer.Children().InsertAtBottom(r.root);
        const float pw = (W - 2 * pane::kSideMargin) * s, ph = kRowH * s;
        r.plate.Offset(float3{pane::kSideMargin * s, 0, 0});
        r.plate.Size(float2{pw, ph});
        r.plateShape.Size(float2{pw, ph});
        r.plateShape.CornerRadius(float2{kPlateRadius * s, kPlateRadius * s});
        r.root.CenterPoint(float3{W * s / 2, ph / 2, 0});
        r.drawn = r.coverDrawn = false;
    }
    if (!r.drawn || (m_coversDirty && !r.coverDrawn)) DrawRow(r);
    const bool visible = shown + kRowH >= 0 && shown <= viewH;
    r.root.IsVisible(visible);
    r.root.Offset(float3{0, std::round(shown * s), 0});
    const bool lifted = m_reorder >= 0 && m_rows[static_cast<size_t>(m_reorder)].get() == &r;
    const float k = lifted ? 1.03f : 1.0f;  // a dragged row lifts a little
    r.root.Scale(float3{k, k, 1});
    const bool pressed = m_press.row >= 0 && m_rows[static_cast<size_t>(m_press.row)].get() == &r;
    r.plate.Opacity(lifted ? 1.0f : r.hover);
    const float a = lifted ? 0.14f : pressed ? kPressAlpha : kHoverAlpha;
    r.plateBrush.Color(winrt::Windows::UI::ColorHelper::FromArgb(static_cast<uint8_t>(std::lround(a * 255)), 255, 255, 255));
}

void QueueView::Render() {
    if (!m_visible) return;
    const float s = m_scale, scroll = ShownScroll();
    auto place = [&](const wuc::SpriteVisual& v, float x, float y, bool show) {
        v.IsVisible(show);
        if (show) v.Offset(float3{std::round(x * s), std::round((y - scroll) * s), 0});
    };
    place(m_historyTitle, 0, m_historyTop, m_historyCount > 0);
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    const float capW = (W - 2 * kContentX - kCapsuleGap) / 2;
    place(m_shuffle, kContentX, m_capsulesTop, true);
    place(m_repeat, kContentX + capW + kCapsuleGap, m_capsulesTop, true);
    place(m_upNextTitle, 0, m_upNextTop, true);
    place(m_empty, 0, m_upNextTop + kUpNextH, m_rows.size() == m_historyCount);
    for (auto& r : m_rows) RenderRow(*r);
    // A dragged row draws above the others.
    if (m_reorder >= 0) {
        auto& r = *m_rows[static_cast<size_t>(m_reorder)];
        if (r.root) {
            m_layer.Children().Remove(r.root);
            m_layer.Children().InsertAtTop(r.root);
        }
    }
    m_coversDirty = false;
}

// ---- Frames ----------------------------------------------------------------------------

bool QueueView::NeedsFrames() const {
    if (!m_visible) return false;
    if (m_dragging || m_reorder >= 0 || m_velocity != 0 || m_scroll.Moving() || m_coversDirty) return true;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const auto& r = *m_rows[i];
        const bool hovered = m_hover.row == static_cast<int>(i) && m_reorder < 0;
        if (r.y.Moving() || r.hover != (hovered ? 1.0f : 0.0f)) return true;
    }
    return false;
}

bool QueueView::Tick(float dt) {
    if (!m_visible) return false;
    if (m_velocity != 0) {
        const float max = MaxScroll();
        float s = m_scroll.Position() - m_velocity * dt;
        m_velocity *= std::pow(0.95f, dt * 60.0f);
        // Past an end the motion runs into the rubber band and springs back.
        if (s < 0 || s > max) {
            m_velocity = 0;
            m_scroll.Snap(s);
            m_scroll.SetTarget(std::clamp(s, 0.0f, max));
        } else {
            m_scroll.Snap(s);
            if (std::abs(m_velocity) < 20) m_velocity = 0;
        }
    }
    m_scroll.Step(dt);
    for (size_t i = 0; i < m_rows.size(); ++i) {
        auto& r = *m_rows[i];
        r.y.Step(dt);
        const bool hovered = m_hover.row == static_cast<int>(i) && m_reorder < 0 && !m_dragging;
        r.hover = Approach(r.hover, hovered ? 1.0f : 0.0f, dt, 0.25f);
    }
    Render();
    return NeedsFrames();
}

// ---- Input -----------------------------------------------------------------------------

bool QueueView::HitTest(POINT pt) const { return m_visible && PtInRect(&m_bounds, pt) != FALSE; }

QueueView::Hit QueueView::HitAt(POINT pt) const {
    Hit hit;
    const float x = (pt.x - m_bounds.left) / m_scale;
    const float y = (pt.y - m_bounds.top) / m_scale + ShownScroll();
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    if (m_historyCount > 0 && y >= m_historyTop && y < m_historyTop + kSectionH && x > W / 2) {
        hit.part = Part::Clear;
        return hit;
    }
    if (y >= m_capsulesTop && y < m_capsulesTop + kCapsuleH) {
        const float capW = (W - 2 * kContentX - kCapsuleGap) / 2;
        if (x >= kContentX && x < kContentX + capW) hit.part = Part::Shuffle;
        else if (x >= kContentX + capW + kCapsuleGap && x < W - kContentX) hit.part = Part::Repeat;
        return hit;
    }
    if (x < pane::kSideMargin || x > W - pane::kSideMargin) return hit;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const float top = m_rows[i]->y.Position();
        if (y >= top && y < top + kRowH) {
            hit.row = static_cast<int>(i);
            hit.part = !m_rows[i]->history && x >= W - kContentX - kHandleW - 12 ? Part::Handle : Part::Row;
            return hit;
        }
    }
    return hit;
}

void QueueView::OnMouseMove(POINT pt) {
    if (m_reorder >= 0) {
        const float y = (pt.y - m_bounds.top) / m_scale + ShownScroll();
        m_reorderY = y - m_grab;
        const size_t count = m_rows.size() - m_historyCount;
        const float first = m_upNextTop + kUpNextH;
        m_reorderTo = std::clamp(static_cast<int>(std::lround((m_reorderY - first) / kRowH)), 0,
                                 static_cast<int>(count) - 1);
        Layout();
        StartFrames();
        return;
    }
    if (m_pending && !m_dragging &&
        (std::abs(pt.y - m_downPt.y) > 4 * m_scale || std::abs(pt.x - m_downPt.x) > 4 * m_scale)) {
        m_dragging = true;
        m_hover = {};
        m_trail.clear();
    }
    if (m_dragging) {
        m_scroll.Snap(m_downScroll - (pt.y - m_downPt.y) / m_scale);
        m_trail.emplace_back(Clock::now(), m_scroll.Position());
        if (m_trail.size() > 8) m_trail.erase(m_trail.begin());
        StartFrames();
        return;
    }
    const Hit hit = HitAt(pt);
    if (hit.row != m_hover.row) StartFrames();
    m_hover = hit;
}

void QueueView::OnMouseLeave() {
    if (m_dragging || m_reorder >= 0) return;
    if (m_hover.row >= 0) StartFrames();
    m_hover = {};
}

void QueueView::OnLButtonDown(POINT pt) {
    m_press = HitAt(pt);
    m_downPt = pt;
    m_downScroll = m_scroll.Position();
    m_pending = true;
    m_velocity = 0;
    m_scroll.Snap(m_scroll.Position());
    if (m_press.part == Part::Handle) {
        m_reorder = m_press.row;
        const float y = (pt.y - m_bounds.top) / m_scale + ShownScroll();
        m_reorderY = m_rows[static_cast<size_t>(m_reorder)]->y.Position();
        m_grab = y - m_reorderY;
        m_reorderTo = m_reorder - static_cast<int>(m_historyCount);
        m_pending = false;
    }
    StartFrames();
}

void QueueView::OnLButtonUp(POINT pt) {
    const Hit press = m_press;
    m_press = {};
    m_pending = false;
    if (m_reorder >= 0) {
        const int from = m_reorder - static_cast<int>(m_historyCount), to = m_reorderTo;
        const size_t fromIndex = m_rows[static_cast<size_t>(m_reorder)]->data.playlistIndex;
        m_reorder = -1;
        if (to != from && to >= 0) {
            // Reorder locally (the playlist change arrives later and finds the same rows).
            auto moved = std::move(m_rows[m_historyCount + static_cast<size_t>(from)]);
            m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(m_historyCount + from));
            m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(m_historyCount + to), std::move(moved));
            const size_t base = fromIndex - static_cast<size_t>(from);
            for (size_t u = 0; u < m_rows.size() - m_historyCount; ++u)
                m_rows[m_historyCount + u]->data.playlistIndex = base + u;
            if (onMove) onMove(fromIndex, base + static_cast<size_t>(to));
        }
        Layout();
        StartFrames();
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        m_velocity = 0;
        if (m_trail.size() >= 2) {
            const auto& a = m_trail.front();
            const auto& b = m_trail.back();
            const float secs = std::chrono::duration<float>(b.first - a.first).count();
            const float age = std::chrono::duration<float>(Clock::now() - b.first).count();
            // Content offset grows as the pointer moves up: velocity is in pointer terms.
            if (secs > 0.005f && age < 0.08f) m_velocity = -(b.second - a.second) / secs;
            if (std::abs(m_velocity) < 50) m_velocity = 0;
        }
        m_scroll.SetTarget(std::clamp(m_scroll.Position(), 0.0f, MaxScroll()));
        StartFrames();
        return;
    }
    const Hit hit = HitAt(pt);
    if (hit.part != press.part || hit.row != press.row) return;
    switch (hit.part) {
    case Part::Row:
    case Part::Handle: {
        const auto& r = *m_rows[static_cast<size_t>(hit.row)];
        if (r.history) {
            if (onPlayHistory) onPlayHistory(r.data.handle);
        } else if (onPlay) {
            onPlay(r.data.playlistIndex);
        }
        break;
    }
    case Part::Clear:
        if (onClearHistory) onClearHistory();
        break;
    case Part::Shuffle:
        if (onShuffle) onShuffle();
        break;
    case Part::Repeat:
        if (onRepeat) onRepeat();
        break;
    case Part::None:
        break;
    }
}

void QueueView::OnCaptureLost() {
    m_pending = m_dragging = false;
    m_reorder = -1;
    m_press = {};
    m_scroll.SetTarget(std::clamp(m_scroll.Position(), 0.0f, MaxScroll()));
    Layout();
    StartFrames();
}

void QueueView::OnWheel(int delta) {
    m_velocity = 0;
    m_scroll.SetTarget(std::clamp(m_scroll.Target() - delta / 120.0f * 80.0f, 0.0f, MaxScroll()));
    m_hover = {};
    StartFrames();
}

void QueueView::OnRButtonUp(POINT pt, POINT screen) {
    const Hit hit = HitAt(pt);
    if (hit.row < 0) return;
    const auto& r = *m_rows[static_cast<size_t>(hit.row)];
    if (!r.history && onRowMenu) onRowMenu(r.data.playlistIndex, screen);
}

}  // namespace glasswave::ui
