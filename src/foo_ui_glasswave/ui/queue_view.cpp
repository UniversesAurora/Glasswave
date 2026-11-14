#include "pch.h"

#include "ui/queue_view.h"

#include "gfx/composition_host.h"
#include "gfx/effects.h"
#include "media/queue_order.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"
#include "ui/pane_common.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <deque>
#include <unordered_map>

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using Clock = std::chrono::steady_clock;

namespace {

// Apple Music's "Playing Next" (iOS 18) measured on a 390 pt screen, scaled to the pane.
constexpr float kRowH = 56;        // DIP
constexpr float kRowCover = 46;    // DIP
constexpr float kCoverRadius = 5;  // DIP
constexpr float kContentX = 20;    // DIP: covers and section titles
constexpr float kSectionH = 48;    // DIP: "History" with Clear
constexpr float kUpNextH = 58;     // DIP: "Continue Playing" with its subtitle
constexpr float kFooterH = 64;     // DIP: "Repeats N songs" below the last row (repeat all)
constexpr float kCapsuleH = 34;    // DIP
constexpr float kCapsuleGap = 10;  // DIP
constexpr float kHandleW = 18;     // DIP: the reorder handle's three lines
constexpr float kPlateRadius = 10; // DIP
constexpr float kHoverAlpha = 0.08f, kPressAlpha = 0.04f;
constexpr float kTopFade = 16, kBottomFade = 40; // DIP
constexpr float kRubber = 0.55f;                 // iOS rubber band constant
// Measured frame by frame from the owner's recording of Apple Music's Up Next on iOS 26
// (.local/refs/ios/ios-menu.mp4, 60 fps): rows move on a critically damped spring of about
// 20.5 rad/s (0.15 / 0.41 / 0.62 / 0.77 / 0.92 / 0.985 of the way at 33 / 67 / 100 / 134 /
// 200 / 300 ms, the same for a tapped row, Next and the shuffle reorder), and a row that
// leaves fades where it is, about linearly, in 125 ms; one that arrives fades in as fast.
constexpr SpringParams kRowSpring{1, 420, 41};
constexpr SpringParams kScrollSpring{1, 220, 30};
constexpr SpringParams kAdvanceSpring = kRowSpring;
constexpr float kFadeSeconds = 0.125f;

float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

// iOS rubber band: the further past the end, the less the content follows.
float Rubber(float overshoot, float dimension) {
    if (dimension <= 0) return 0;
    return (1.0f - 1.0f / (overshoot * kRubber / dimension + 1.0f)) * dimension;
}

} // namespace

struct QueueView::RowView {
    Row data;
    bool history = false;
    Spring y{0};
    bool placed = false; // the spring has a position
    float hover = 0;
    float opacity = 1;
    float enterOffset = 0;
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
    // Keep the clip fixed while m_root slides during the lyrics/queue transition.
    m_viewport = comp.CreateContainerVisual();
    m_viewport.Clip(comp.CreateInsetClip());
    parent.Children().InsertAtTop(m_viewport);
    m_root = comp.CreateContainerVisual();
    m_root.IsVisible(false);
    m_viewport.Children().InsertAtTop(m_root);
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
    for (auto* v : {&m_historyTitle, &m_upNextTitle, &m_empty, &m_shuffle, &m_repeat, &m_repeatFooter}) {
        *v = comp.CreateSpriteVisual();
        m_layer.Children().InsertAtTop(*v);
    }
    m_plateGlass.Create(m_root, m_layer);
    m_shuffleGlass.Create(m_root, m_layer);
    m_repeatGlass.Create(m_root, m_layer);
    m_scroll.SetParams(kScrollSpring);
    m_artToken = ArtCache::Get().Subscribe([this] {
        m_coversDirty = true;
        if (m_visible) StartFrames();
    });
}

QueueView::~QueueView() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    for (auto& r : m_rows)
        ReleaseRow(*r);
    for (auto& r : m_retiringRows)
        ReleaseRow(*r);
    if (m_viewport) {
        if (auto parent = m_viewport.Parent()) parent.Children().Remove(m_viewport);
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
    const float w = static_cast<float>(std::max<LONG>(0, px.right - px.left));
    const float h = static_cast<float>(std::max<LONG>(0, px.bottom - px.top));
    m_viewport.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    m_viewport.Size(float2{w, h});
    m_root.Size(float2{w, h});
    if (!changed) return;
    m_layer.Size(float2{w, h});
    if (m_maskSurface && h > 0) {
        m_maskSurface.Resize(winrt::Windows::Graphics::SizeInt32{4, static_cast<int32_t>(h)});
        const float H = h / m_scale;
        gfx::CompositionHost::Get().Draw(m_maskSurface, m_scale, [&](ID2D1DeviceContext* dc) {
            const D2D1_GRADIENT_STOP s[] = {
                {0.0f, D2D1::ColorF(1, 1, 1, 0)},
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
    for (auto& r : m_rows)
        ReleaseRow(*r); // widths changed: redraw
    for (auto& r : m_retiringRows)
        ReleaseRow(*r);
    m_retiringRows.clear();
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
        m_pointerInside = false;
        m_hover = {};
        for (auto& r : m_rows)
            ReleaseRow(*r);
        for (auto& r : m_retiringRows)
            ReleaseRow(*r);
        m_retiringRows.clear();
    }
}

// ---- Data ------------------------------------------------------------------------------

void QueueView::SetRows(std::vector<Row> rows, bool history, bool playbackChanged) {
    // A row being dragged stays in the hand when the list changes under it (a playlist still
    // loading, play counts, the next song starting): it is found again below.
    const RowView* dragged =
        m_reorder >= 0 && static_cast<size_t>(m_reorder) < m_rows.size() ? m_rows[static_cast<size_t>(m_reorder)].get()
                                                                          : nullptr;
    // Rows keep their visuals and spring position when the same song is still listed, so a
    // song that starts slides out and the others move up instead of the list jumping.
    std::vector<std::unique_ptr<RowView>> old;
    std::vector<std::unique_ptr<RowView>> keep;
    for (auto& r : m_rows)
        (r->history == history ? old : keep).push_back(std::move(r));
    // Reuse rows along one continuous movement. Wrapped rows leave and re-enter at the
    // opposite end, while native shuffle can move the whole list up or down.
    std::ptrdiff_t shift = 0;
    bool aligned = false; // some song is listed before and after
    if (playbackChanged && !history) {
        std::vector<size_t> before, after;
        std::map<size_t, metadb_handle_ptr> incoming;
        for (const auto& r : rows) {
            after.push_back(r.playlistIndex);
            incoming.emplace(r.playlistIndex, r.handle);
        }
        for (const auto& r : old) {
            const auto found = incoming.find(r->data.playlistIndex);
            const bool same = found != incoming.end() && found->second == r->data.handle;
            before.push_back(same ? r->data.playlistIndex : SIZE_MAX);
            aligned |= same;
        }
        shift = media::QueueRowShift(before, after);
    }
    // As on iOS the list travels the whole way (a song tapped three rows down: the rows under
    // it rise three rows) on the row spring; the rows it leaves behind fade where they are. A
    // list with no song in common is replaced by a cross-fade in place.
    const float movement = static_cast<float>(shift) * kRowH;
    const bool replace = playbackChanged && !aligned;
    const auto rowKey = [history](const Row& data) {
        return std::to_string(reinterpret_cast<uintptr_t>(data.handle.get_ptr())) + ':' +
               std::to_string(history ? 0 : data.playlistIndex);
    };
    std::unordered_map<std::string, std::deque<size_t>> reusable;
    // A song whose slot changed (a drop under shuffle reorders only the playing order, so the
    // rows keep their playlist indices while a drop rewrote them here) is still the same row.
    std::unordered_map<const void*, std::deque<size_t>> sameSong;
    if (!replace && !playbackChanged)
        for (size_t i = 0; i < old.size(); ++i) {
            reusable[rowKey(old[i]->data)].push_back(i);
            sameSong[old[i]->data.handle.get_ptr()].push_back(i);
        }
    std::vector<std::unique_ptr<RowView>> fresh;
    for (auto& data : rows) {
        std::unique_ptr<RowView> view;
        if (!replace && playbackChanged) {
            const auto index = static_cast<std::ptrdiff_t>(fresh.size()) - shift;
            if (index >= 0 && static_cast<size_t>(index) < old.size()) {
                auto& candidate = old[static_cast<size_t>(index)];
                if (candidate && rowKey(candidate->data) == rowKey(data)) view = std::move(candidate);
            }
        } else if (!replace) {
            auto found = reusable.find(rowKey(data));
            while (found != reusable.end() && !found->second.empty() && !old[found->second.front()])
                found->second.pop_front();
            if (found != reusable.end() && !found->second.empty()) {
                view = std::move(old[found->second.front()]);
                found->second.pop_front();
            }
        }
        if (!view && !replace && !playbackChanged) {
            auto found = sameSong.find(data.handle.get_ptr());
            while (found != sameSong.end() && !found->second.empty() && !old[found->second.front()])
                found->second.pop_front();
            if (found != sameSong.end() && !found->second.empty()) {
                view = std::move(old[found->second.front()]);
                found->second.pop_front();
            }
        }
        if (!view) {
            view = std::make_unique<RowView>();
            view->history = history;
            // Rising with the list (Next, a tapped row): a new row comes up from below with the
            // others, solid. Otherwise (Previous brings a song back on top, shuffle, a new list)
            // it fades in at its place.
            const bool rises = playbackChanged && !replace && shift < 0 && m_visible;
            view->opacity = m_visible && !rises ? 0.0f : 1.0f;
            view->enterOffset = rises ? m_scroll.Position() - m_cardTop - movement : 0;
        }
        view->y.SetParams(playbackChanged ? kAdvanceSpring : kRowSpring);
        if (view->data.title != data.title || view->data.artist != data.artist) view->drawn = false;
        view->data = std::move(data);
        fresh.push_back(std::move(view));
    }
    for (auto& o : old) {
        if (!o) continue;
        if (m_visible && !history && o->root && o->root.IsVisible()) {
            o->y.Snap(o->y.Position()); // fades where it is (iOS), it does not slide away
            m_retiringRows.push_back(std::move(o));
        } else {
            ReleaseRow(*o);
        }
    }
    m_rows.clear();
    if (history) {
        m_historyCount = fresh.size();
        for (auto& r : fresh)
            m_rows.push_back(std::move(r));
        for (auto& r : keep)
            m_rows.push_back(std::move(r));
    } else {
        for (auto& r : keep)
            m_rows.push_back(std::move(r));
        for (auto& r : fresh)
            m_rows.push_back(std::move(r));
    }
    m_reorder = -1;
    m_hover = m_press = {};
    if (dragged) {
        for (size_t i = m_historyCount; i < m_rows.size(); ++i)
            if (m_rows[i].get() == dragged) m_reorder = static_cast<int>(i);
        const int count = static_cast<int>(m_rows.size() - m_historyCount);
        if (m_reorder >= 0) m_reorderTo = std::clamp(m_reorderTo, 0, std::max(0, count - 1));
    }
}

void QueueView::SetUpNext(std::vector<Row> rows, bool playbackChanged) {
    SetRows(std::move(rows), false, playbackChanged);
    Layout();
    if (playbackChanged)
        ScrollToCard(true);
    else
        KeepScrollInRange();
    RefreshHover();
    DrawChrome();
    if (m_visible) StartFrames();
}

void QueueView::SetHistory(std::vector<Row> rows) {
    SetRows(std::move(rows), true);
    Layout();
    KeepScrollInRange();
    RefreshHover();
    DrawChrome();
    if (m_visible) StartFrames();
}

void QueueView::SetShuffleRepeat(bool shuffle, int repeat) {
    if (shuffle == m_shuffleOn && repeat == m_repeatMode) return;
    m_shuffleOn = shuffle;
    m_repeatMode = repeat;
    DrawChrome();
}

void QueueView::SetRepeatCycle(size_t songs) {
    if (songs == m_repeatCycle) return;
    m_repeatCycle = songs;
    DrawChrome();
    Layout();
    KeepScrollInRange();
    Render();
}

void QueueView::ScrollToCard(bool animate) {
    Layout();
    const float target = std::min(m_cardTop, MaxScroll());
    m_followingPlayback = animate && m_visible;
    if (m_followingPlayback) {
        m_scroll.SetParams(kAdvanceSpring);
        m_scroll.SetTarget(target);
    } else
        m_scroll.Snap(target);
    m_velocity = 0;
    m_dragging = m_pending = false;
    if (m_visible) StartFrames();
}

// A list that became shorter than where it was scrolled to springs back to its end; left
// there, the rubber band would show the content scrolled away and the list empty.
void QueueView::KeepScrollInRange() {
    if (m_dragging || m_followingPlayback) return;
    const float max = MaxScroll();
    if (m_scroll.Target() <= max && m_scroll.Position() <= max) return;
    m_velocity = 0;
    m_scroll.SetParams(kScrollSpring);
    m_scroll.SetTarget(std::min(m_scroll.Target(), max));
    if (m_visible) StartFrames();
}

float QueueView::MaxScroll() const {
    const float viewH = (m_bounds.bottom - m_bounds.top) / m_scale;
    return std::max(m_cardTop, m_contentH - viewH);
}

float QueueView::ShownScroll() const {
    const float s = m_scroll.Position(), max = MaxScroll();
    if (m_followingPlayback) return s;
    const float viewH = (m_bounds.bottom - m_bounds.top) / m_scale;
    if (s < 0) return -Rubber(-s, viewH);
    if (s > max) return max + Rubber(s - max, viewH);
    return s;
}

void QueueView::SetBare(bool bare) {
    if (bare == m_bare) return;
    m_bare = bare;
    Layout();
    ScrollToCard();
    if (onFrames) onFrames();
}

float QueueView::CardTop() const {
    return m_cardTop - ShownScroll();
}

// ---- Layout and rendering --------------------------------------------------------------

void QueueView::Layout() {
    const float oldCardTop = m_cardTop;
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
    // History grows above the card. Translate the content coordinates together so the
    // card and the upcoming rows keep their screen positions and spring velocities.
    const float delta = m_cardTop - oldCardTop;
    if (delta != 0) {
        m_scroll.Shift(delta);
        m_downScroll += delta;
        for (auto& r : m_rows)
            if (!r->history && r->placed) r->y.Shift(delta);
        for (auto& r : m_retiringRows)
            r->y.Shift(delta);
    }
    if (m_bare) {
        m_capsulesTop = y;
    } else {
        y += pane::kHeaderHeight;
        m_capsulesTop = y + 4;
        y = m_capsulesTop + kCapsuleH + 20;
    }
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
            r.y.Snap(target + r.enterOffset);
            r.y.SetTarget(target);
            r.enterOffset = 0;
            r.placed = true;
        } else {
            r.y.SetTarget(target);
        }
    }
    // Repeat-all: Apple Music's line after the last row, which takes the empty row's place.
    m_footerTop = first + count * kRowH;
    const float rowsH = m_repeatCycle > 0 ? count * kRowH + kFooterH : std::max<size_t>(count, 1) * kRowH;
    m_contentH = first + rowsH + 32;
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
    const int w = std::max(1, static_cast<int>(std::ceil(W * m_scale))),
              h = static_cast<int>(std::ceil(kRowH * m_scale));
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
    const auto state =
        r.data.handle.is_valid()
            ? ArtCache::Get().Find(r.data.handle, static_cast<int>(std::ceil(kRowCover * m_scale)), &art)
            : ArtCache::State::Missing;
    m_dc->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    const bool drawn = host.Draw(r.surface, m_scale, m_dc.get(), [&](ID2D1DeviceContext* dc) {
        com_ptr<ID2D1Bitmap> bitmap;
        if (art) dc->CreateBitmapFromWicBitmap(art.get(), nullptr, bitmap.put());
        const float cy = kRowH / 2;
        pane::DrawCover(dc, bitmap.get(),
                        {kContentX, cy - kRowCover / 2, kContentX + kRowCover, cy + kRowCover / 2},
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
    r.drawn = drawn; // not drawn: the host clears it, and the row is drawn again
    r.coverDrawn = drawn && state != ArtCache::State::Pending;
}

void QueueView::DrawChrome() {
    if (!m_visible || m_bounds.right <= m_bounds.left) return;
    auto& host = gfx::CompositionHost::Get();
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    auto paint = [&](const wuc::SpriteVisual& visual, float wDip, float hDip,
                     const std::function<void(ID2D1DeviceContext*)>& draw) {
        const int w = std::max(1, static_cast<int>(std::ceil(wDip * m_scale))),
                  h = std::max(1, static_cast<int>(std::ceil(hDip * m_scale)));
        host.Repaint(visual, w, h, m_scale, draw);
        visual.Size(float2{static_cast<float>(w), static_cast<float>(h)});
    };
    paint(m_historyTitle, W, kSectionH, [&](ID2D1DeviceContext* dc) {
        pane::DrawTextLine(dc, Tr(L"History"), 20, DWRITE_FONT_WEIGHT_BOLD, {kContentX, 12}, W / 2,
                           pane::White(dc, 0.95f).get());
        pane::DrawTextLine(dc, Tr(L"Clear"), 16, DWRITE_FONT_WEIGHT_NORMAL, {W / 2, 15}, W / 2 - kContentX,
                           pane::White(dc, 0.6f).get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    });
    const size_t upNext = m_rows.size() - m_historyCount;
    paint(m_upNextTitle, W, kUpNextH, [&](ID2D1DeviceContext* dc) {
        pane::DrawTextLine(dc, Tr(L"Continue Playing"), 20, DWRITE_FONT_WEIGHT_BOLD, {kContentX, 6},
                           W - 2 * kContentX, pane::White(dc, 0.95f).get());
        const std::wstring sub = std::to_wstring(upNext) + Tr(upNext == 1 ? L" song" : L" songs");
        pane::DrawTextLine(dc, sub, 14, DWRITE_FONT_WEIGHT_NORMAL, {kContentX, 33}, W - 2 * kContentX,
                           pane::White(dc, 0.55f).get());
    });
    paint(m_empty, W, kRowH, [&](ID2D1DeviceContext* dc) {
        pane::DrawTextLine(dc, Tr(L"Nothing up next"), 15, DWRITE_FONT_WEIGHT_NORMAL, {kContentX, 18},
                           W - 2 * kContentX, pane::White(dc, 0.45f).get());
    });
    if (m_repeatCycle > 0) {
        // The repeat glyph and "Repeats N songs", centred together in grey (Apple Music).
        paint(m_repeatFooter, W, kFooterH, [&](ID2D1DeviceContext* dc) {
            const std::wstring text = Tr(L"Repeats") + L" " + std::to_wstring(m_repeatCycle) +
                                      Tr(m_repeatCycle == 1 ? L" song" : L" songs");
            auto* f = gfx::CompositionHost::Get().DWrite();
            auto format = pane::Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            com_ptr<IDWriteTextLayout> layout;
            if (FAILED(f->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), W, kFooterH,
                                           layout.put())))
                return;
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            constexpr float iconW = 20, gap = 8;
            const float x0 = std::round((W - iconW - gap - tm.width) / 2);
            auto ink = pane::White(dc, 0.55f);
            pane::DrawIcon(dc, pane::icon::kRepeatAll, 16, {x0 + iconW / 2, kFooterH / 2}, ink.get(),
                           DWRITE_FONT_WEIGHT_BOLD);
            dc->DrawTextLayout({x0 + iconW + gap, std::round((kFooterH - tm.height) / 2)}, layout.get(), ink.get());
        });
    }
    // Capsules: translucent when off, light with a dark glyph when on (Apple Music), both on
    // glass; the light one a little clearer than Apple's 78 % since it lies on glass.
    const float capW = (W - 2 * kContentX - kCapsuleGap) / 2;
    {
        const int w = std::max(1, static_cast<int>(std::ceil(capW * m_scale))),
                  h = std::max(1, static_cast<int>(std::ceil(kCapsuleH * m_scale)));
        m_shuffleGlass.Paint(w, h, m_scale);
        m_repeatGlass.Paint(w, h, m_scale);
    }
    auto capsule = [&](const wuc::SpriteVisual& visual, bool on,
                       const std::function<void(ID2D1DeviceContext*, ID2D1Brush*)>& glyph) {
        paint(visual, capW, kCapsuleH, [&](ID2D1DeviceContext* dc) {
            pane::DrawCapsule(dc, {0, 0, capW, kCapsuleH}, pane::White(dc, on ? 0.66f : 0.14f).get());
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(
                on ? D2D1::ColorF(0.18f, 0.16f, 0.18f, 0.9f) : D2D1::ColorF(1, 1, 1, 0.9f), ink.put());
            glyph(dc, ink.get());
        });
    };
    capsule(m_shuffle, m_shuffleOn, [&](ID2D1DeviceContext* dc, ID2D1Brush* ink) {
        pane::DrawIcon(dc, pane::icon::kShuffle, 16, {capW / 2, kCapsuleH / 2}, ink, DWRITE_FONT_WEIGHT_BOLD);
    });
    capsule(m_repeat, m_repeatMode != 0, [&](ID2D1DeviceContext* dc, ID2D1Brush* ink) {
        pane::DrawIcon(dc, m_repeatMode == 2 ? pane::icon::kRepeatOne : pane::icon::kRepeatAll, 16,
                       {capW / 2, kCapsuleH / 2}, ink, DWRITE_FONT_WEIGHT_BOLD);
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
    r.root.Opacity(r.opacity);
    const bool lifted = m_reorder >= 0 && m_rows[static_cast<size_t>(m_reorder)].get() == &r;
    const float k = lifted ? 1.03f : 1.0f; // a dragged row lifts a little
    r.root.Scale(float3{k, k, 1});
    const bool pressed = m_press.row >= 0 && m_rows[static_cast<size_t>(m_press.row)].get() == &r;
    r.plate.Opacity(lifted ? 1.0f : r.hover);
    const float a = lifted ? 0.14f : pressed ? kPressAlpha : kHoverAlpha;
    r.plateBrush.Color(
        winrt::Windows::UI::ColorHelper::FromArgb(static_cast<uint8_t>(std::lround(a * 255)), 255, 255, 255));
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
    place(m_shuffle, kContentX, m_capsulesTop, !m_bare);
    place(m_repeat, kContentX + capW + kCapsuleGap, m_capsulesTop, !m_bare);
    for (auto [glass, x] : {std::pair{&m_shuffleGlass, kContentX}, std::pair{&m_repeatGlass, kContentX + capW + kCapsuleGap}}) {
        const auto& v = glass->Visual();
        v.IsVisible(!m_bare);
        if (!m_bare) v.Offset(float3{std::round(x * s), std::round((m_capsulesTop - scroll) * s), 0});
    }
    place(m_upNextTitle, 0, m_upNextTop, true);
    place(m_empty, 0, m_upNextTop + kUpNextH, m_rows.size() == m_historyCount && m_repeatCycle == 0);
    // Under the last row as it moves now, so the line rides along with the rows' springs.
    float footerY = m_footerTop;
    for (size_t i = m_historyCount; i < m_rows.size(); ++i)
        footerY = std::max(footerY, m_rows[i]->y.Position() + kRowH);
    place(m_repeatFooter, 0, footerY, m_repeatCycle > 0);
    for (auto& r : m_retiringRows)
        RenderRow(*r);
    for (auto& r : m_rows)
        RenderRow(*r);
    // A dragged row draws above the others.
    if (m_reorder >= 0) {
        auto& r = *m_rows[static_cast<size_t>(m_reorder)];
        if (r.root) {
            m_layer.Children().Remove(r.root);
            m_layer.Children().InsertAtTop(r.root);
        }
    }
    // The glass under the plate showing most: the lifted row's, else the hovered one's.
    {
        const RowView* best = nullptr;
        float shownPlate = 0;
        for (const auto& r : m_rows) {
            if (!r->root) continue;
            const bool lifted = m_reorder >= 0 && m_rows[static_cast<size_t>(m_reorder)].get() == r.get();
            const float a = (lifted ? 1.0f : r->hover) * r->opacity;
            if (a > shownPlate) {
                shownPlate = a;
                best = r.get();
            }
        }
        const float viewH = (m_bounds.bottom - m_bounds.top) / s;
        const float shown = best ? best->y.Position() - scroll : 0;
        if (!best || shown + kRowH < 0 || shown > viewH) {
            m_plateGlass.Hide();
        } else {
            const bool lifted = m_reorder >= 0 && m_rows[static_cast<size_t>(m_reorder)].get() == best;
            const float pw = (W - 2 * pane::kSideMargin) * s, ph = kRowH * s;
            m_plateGlass.Show(float3{pane::kSideMargin * s, std::round(shown * s), 0}, float2{pw, ph}, kPlateRadius * s,
                              lifted ? 1.03f : 1.0f, float2{W * s / 2 - pane::kSideMargin * s, ph / 2}, shownPlate, s);
        }
    }
    m_coversDirty = false;
}

// ---- Frames ----------------------------------------------------------------------------

bool QueueView::NeedsFrames() const {
    if (!m_visible) return false;
    if (m_dragging || m_reorder >= 0 || m_velocity != 0 || m_scroll.Moving() || m_coversDirty ||
        !m_retiringRows.empty())
        return true;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const auto& r = *m_rows[i];
        const bool hovered = m_hover.row == static_cast<int>(i) && m_reorder < 0;
        if (r.y.Moving() || r.hover != (hovered ? 1.0f : 0.0f) || r.opacity != 1.0f) return true;
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
    if (!m_scroll.Moving()) m_followingPlayback = false;
    RefreshHover();
    for (size_t i = 0; i < m_rows.size(); ++i) {
        auto& r = *m_rows[i];
        r.y.Step(dt);
        r.opacity = std::min(1.0f, r.opacity + dt / kFadeSeconds);
        const bool hovered = m_hover.row == static_cast<int>(i) && m_reorder < 0 && !m_dragging;
        r.hover = Approach(r.hover, hovered ? 1.0f : 0.0f, dt, 0.25f);
    }
    for (auto& r : m_retiringRows) {
        r->y.Step(dt);
        r->opacity = std::max(0.0f, r->opacity - dt / kFadeSeconds);
    }
    std::erase_if(m_retiringRows, [this](auto& r) {
        if (r->opacity > 0) return false;
        ReleaseRow(*r);
        return true;
    });
    Render();
    return NeedsFrames();
}

// ---- Input -----------------------------------------------------------------------------

bool QueueView::HitTest(POINT pt) const {
    return m_visible && PtInRect(&m_bounds, pt) != FALSE;
}

QueueView::Hit QueueView::HitAt(POINT pt) const {
    Hit hit;
    const float x = (pt.x - m_bounds.left) / m_scale;
    const float y = (pt.y - m_bounds.top) / m_scale + ShownScroll();
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    if (m_historyCount > 0 && y >= m_historyTop && y < m_historyTop + kSectionH && x > W / 2) {
        hit.part = Part::Clear;
        return hit;
    }
    if (!m_bare && y >= m_capsulesTop && y < m_capsulesTop + kCapsuleH) {
        const float capW = (W - 2 * kContentX - kCapsuleGap) / 2;
        if (x >= kContentX && x < kContentX + capW)
            hit.part = Part::Shuffle;
        else if (x >= kContentX + capW + kCapsuleGap && x < W - kContentX)
            hit.part = Part::Repeat;
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
                                 std::max(0, static_cast<int>(count) - 1));
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
    m_pointer = pt;
    m_pointerInside = true;
    const Hit hit = HitAt(pt);
    if (hit.row != m_hover.row) StartFrames();
    m_hover = hit;
}

void QueueView::OnMouseLeave() {
    m_pointerInside = false;
    if (m_dragging || m_reorder >= 0) return;
    if (m_hover.row >= 0) StartFrames();
    m_hover = {};
}

// The row under a resting pointer, after the list moved or was refreshed under it (a refresh
// replaces the rows and used to drop the highlight while the pointer stayed on it).
void QueueView::RefreshHover() {
    if (!m_pointerInside || m_dragging || m_pending || m_reorder >= 0) return;
    const Hit hit = HitAt(m_pointer);
    if (hit.row != m_hover.row || hit.part != m_hover.part) {
        m_hover = hit;
        if (m_visible) StartFrames();
    }
}

void QueueView::OnLButtonDown(POINT pt) {
    m_press = HitAt(pt);
    m_downPt = pt;
    m_downScroll = m_scroll.Position();
    m_pending = true;
    m_velocity = 0;
    m_scroll.Snap(m_scroll.Position());
    m_followingPlayback = false;
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
        const int count = static_cast<int>(m_rows.size() - m_historyCount);
        m_reorder = -1;
        if (to != from && to >= 0 && from >= 0 && from < count && to < count) {
            std::vector<size_t> slots;
            for (size_t u = m_historyCount; u < m_rows.size(); ++u)
                slots.push_back(m_rows[u]->data.playlistIndex);
            // Reorder locally (the playlist change arrives later and finds the same rows).
            auto moved = std::move(m_rows[m_historyCount + static_cast<size_t>(from)]);
            m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(m_historyCount + from));
            m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(m_historyCount + to),
                          std::move(moved));
            std::vector<size_t> items;
            for (size_t u = 0; u < slots.size(); ++u) {
                auto& row = m_rows[m_historyCount + u]->data;
                items.push_back(row.playlistIndex);
                row.playlistIndex = slots[u];
            }
            if (onReorder) onReorder(slots, items);
        }
        Layout();
        KeepScrollInRange();
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
    m_followingPlayback = false;
    m_press = {};
    m_scroll.SetTarget(std::clamp(m_scroll.Position(), 0.0f, MaxScroll()));
    Layout();
    StartFrames();
}

void QueueView::OnWheel(int delta) {
    m_velocity = 0;
    m_followingPlayback = false;
    m_scroll.SetParams(kScrollSpring);
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

} // namespace glasswave::ui
