#include "pch.h"

#include "ui/now_playing_pane.h"

#include "gfx/composition_host.h"
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

// Segmented control: as wide and tall as the queue's capsules, a light thumb inside the track.
constexpr float kTrackX = 20, kTrackH = 34; // DIP
constexpr float kThumbInset = 3;            // DIP
constexpr float kTrackAlpha = 0.14f, kThumbAlpha = 0.78f;
constexpr SpringParams kSwitchSpring{1, 260, 30};
constexpr float kLyricsRise = 24, kQueueRise = 48; // DIP the two sides move while switching

wuc::ContainerVisual MakeRoot(const wuc::ContainerVisual& parent) {
    auto root = gfx::CompositionHost::Get().Compositor().CreateContainerVisual();
    root.IsVisible(false);
    parent.Children().InsertAtTop(root);
    return root;
}

// Apple Music's lyrics glyph: a speech bubble with a quotation mark. Sized like the list
// glyph beside it (17 DIP Fluent icon), bubble and tail together centred on `c`.
void DrawLyricsIcon(ID2D1DeviceContext* dc, D2D1_POINT_2F c, ID2D1Brush* ink) {
    com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    const float l = c.x - 8, t = c.y - 7, r = c.x + 8, b = c.y + 4, rad = 3;
    com_ptr<ID2D1PathGeometry> path;
    if (FAILED(f->CreatePathGeometry(path.put()))) return;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;
    const auto arc = [&](float x, float y) {
        sink->AddArc({{x, y}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    };
    sink->BeginFigure({l + rad, t}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({r - rad, t});
    arc(r, t + rad);
    sink->AddLine({r, b - rad});
    arc(r - rad, b);
    sink->AddLine({l + 7, b});
    sink->AddLine({l + 3, b + 3}); // tail
    sink->AddLine({l + 3.2f, b});
    sink->AddLine({l + rad, b});
    arc(l, b - rad);
    sink->AddLine({l, t + rad});
    arc(l + rad, t);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    dc->DrawGeometry(path.get(), ink, 1.4f);
    const float qy = (t + b) / 2 - 0.8f;
    for (const float x : {c.x - 2.4f, c.x + 2.6f}) {
        dc->FillEllipse(D2D1::Ellipse({x, qy}, 1.4f, 1.4f), ink);
        dc->DrawLine({x + 1.2f, qy + 0.3f}, {x - 0.6f, qy + 3.0f}, ink, 1.1f);
    }
}

} // namespace

NowPlayingPane::NowPlayingPane(const wuc::ContainerVisual& parent)
    : m_root(MakeRoot(parent)), m_lyrics(m_root), m_queue(m_root) {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    m_dc = host.CreateDeviceContext();
    m_tint = comp.CreateSpriteVisual();
    m_tint.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(
        static_cast<uint8_t>(std::lround(pane::kPaneTint * 255)), 0, 0, 0)));
    m_root.Children().InsertAtBottom(m_tint);
    m_header = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_header);
    m_bar = comp.CreateContainerVisual();
    m_root.Children().InsertAtTop(m_bar);
    for (auto* visual : {&m_track, &m_thumb}) {
        *visual = comp.CreateSpriteVisual();
        m_bar.Children().InsertAtTop(*visual);
    }
    for (size_t i = 0; i < 2; ++i) {
        m_segLight[i] = comp.CreateSpriteVisual();
        m_segDark[i] = comp.CreateSpriteVisual();
        m_bar.Children().InsertAtTop(m_segLight[i]);
        m_bar.Children().InsertAtTop(m_segDark[i]);
    }
    m_t.SetParams(kSwitchSpring);
    m_lyrics.onSeek = [this](double s) {
        if (onSeek) onSeek(s);
    };
    m_lyrics.onFrames = [this] { StartFrames(); };
    m_queue.onFrames = [this] { StartFrames(); };
}

NowPlayingPane::~NowPlayingPane() {
    if (m_root) {
        if (auto parent = m_root.Parent()) parent.Children().Remove(m_root);
    }
}

void NowPlayingPane::StartFrames() {
    if (onFrames) onFrames();
}

RECT NowPlayingPane::BarRect() const {
    const LONG h = static_cast<LONG>(std::lround(pane::kBottomBar * m_scale));
    return {m_bounds.left, std::max(m_bounds.top, m_bounds.bottom - h), m_bounds.right, m_bounds.bottom};
}

RECT NowPlayingPane::LyricsRect() const {
    const LONG header = static_cast<LONG>(std::lround(pane::kHeaderHeight * m_scale));
    const RECT bar = BarRect();
    return {m_bounds.left, std::min(bar.top, m_bounds.top + header), m_bounds.right, bar.top};
}

RECT NowPlayingPane::QueueRect() const {
    return {m_bounds.left, m_bounds.top, m_bounds.right, BarRect().top};
}

void NowPlayingPane::SetBounds(const RECT& px, float scale) {
    const bool sizeChanged = px.right - px.left != m_bounds.right - m_bounds.left ||
                             px.bottom - px.top != m_bounds.bottom - m_bounds.top || scale != m_scale;
    const bool positionChanged = px.left != m_bounds.left || px.top != m_bounds.top;
    m_bounds = px;
    m_scale = scale;
    m_lyrics.SetBounds(LyricsRect(), scale);
    m_queue.SetBounds(QueueRect(), scale);
    if (sizeChanged)
        LayoutParts();
    else if (positionChanged)
        LayoutGeometry();
    ApplyTransition();
}

void NowPlayingPane::LayoutGeometry() {
    const float w = static_cast<float>(std::max<LONG>(0, m_bounds.right - m_bounds.left));
    const float h = static_cast<float>(std::max<LONG>(0, m_bounds.bottom - m_bounds.top));
    m_tint.Offset(float3{static_cast<float>(m_bounds.left), static_cast<float>(m_bounds.top), 0});
    m_tint.Size(float2{w, h});
    const RECT bar = BarRect();
    m_bar.Offset(float3{static_cast<float>(bar.left), static_cast<float>(bar.top), 0});
    m_bar.Size(float2{w, static_cast<float>(bar.bottom - bar.top)});
    const RECT track = TrackRect();
    const float tw = static_cast<float>(track.right - track.left),
                th = static_cast<float>(track.bottom - track.top);
    m_track.Offset(
        float3{static_cast<float>(track.left - bar.left), static_cast<float>(track.top - bar.top), 0});
    m_track.Size(float2{tw, th});
    const float inset = kThumbInset * m_scale, segW = tw / 2;
    m_thumb.Size(float2{segW - 2 * inset, th - 2 * inset});
    for (size_t i = 0; i < 2; ++i)
        for (auto& v : {m_segLight[i], m_segDark[i]})
            v.Offset(float3{std::round(track.left - bar.left + i * segW),
                            static_cast<float>(track.top - bar.top), 0});
}

void NowPlayingPane::LayoutParts() {
    LayoutGeometry();
    PaintHeader();
    PaintBar();
}

void NowPlayingPane::PaintHeader() {
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(1, m_bounds.right - m_bounds.left);
    const int h = static_cast<int>(std::lround(pane::kHeaderHeight * m_scale));
    m_header.Size(float2{static_cast<float>(w), static_cast<float>(h)});
    auto surface = host.CreateSurface(w, h);
    m_dc->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    host.Draw(surface, m_scale, m_dc.get(), [&](ID2D1DeviceContext* dc) {
        com_ptr<ID2D1Bitmap> cover;
        if (m_art) dc->CreateBitmapFromWicBitmap(m_art.get(), nullptr, cover.put());
        pane::DrawHeader(dc, w / m_scale, cover.get(), m_title, m_artist);
    });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::None);
    brush.HorizontalAlignmentRatio(0);
    brush.VerticalAlignmentRatio(0);
    m_header.Brush(brush);
}

void NowPlayingPane::PaintBar() {
    auto& host = gfx::CompositionHost::Get();
    const RECT track = TrackRect();
    const int w = std::max<int>(1, (track.right - track.left) / 2),
              h = std::max<int>(1, track.bottom - track.top);
    const float W = w / m_scale, H = h / m_scale;
    const int trackW = std::max<int>(1, track.right - track.left);
    host.Repaint(m_track, trackW, h, m_scale, [&](ID2D1DeviceContext* dc) {
        pane::DrawCapsule(dc, {0, 0, trackW / m_scale, H}, pane::White(dc, kTrackAlpha).get());
    });
    const int thumbW = std::max(1, static_cast<int>(std::lround(trackW / 2.0f - 2 * kThumbInset * m_scale)));
    const int thumbH = std::max(1, static_cast<int>(std::lround(h - 2 * kThumbInset * m_scale)));
    host.Repaint(m_thumb, thumbW, thumbH, m_scale, [&](ID2D1DeviceContext* dc) {
        pane::DrawCapsule(dc, {0, 0, thumbW / m_scale, thumbH / m_scale}, pane::White(dc, kThumbAlpha).get());
    });
    for (size_t i = 0; i < 2; ++i) {
        const std::wstring label = Tr(i == 0 ? L"Lyrics" : L"Up Next");
        for (const bool dark : {false, true}) {
            auto& visual = dark ? m_segDark[i] : m_segLight[i];
            host.Repaint(visual, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
                com_ptr<ID2D1SolidColorBrush> ink;
                dc->CreateSolidColorBrush(
                    dark ? D2D1::ColorF(0.18f, 0.16f, 0.18f, 0.9f) : D2D1::ColorF(1, 1, 1, 0.9f), ink.put());
                // Icon and label centred together.
                auto* f = host.DWrite();
                auto format = pane::Format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                com_ptr<IDWriteTextLayout> layout;
                if (FAILED(f->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), format.get(),
                                               W, H, layout.put())))
                    return;
                DWRITE_TEXT_METRICS tm{};
                layout->GetMetrics(&tm);
                const float iconW = 22, gap = 6;
                const float x0 = std::round((W - iconW - gap - tm.width) / 2);
                const D2D1_POINT_2F c{x0 + iconW / 2, H / 2};
                if (i == 0)
                    DrawLyricsIcon(dc, c, ink.get());
                else
                    pane::DrawIcon(dc, pane::icon::kList, 17, c, ink.get());
                dc->DrawTextLayout({x0 + iconW + gap, std::round((H - tm.height) / 2)}, layout.get(),
                                   ink.get());
            });
            visual.Size(float2{static_cast<float>(w), static_cast<float>(h)});
        }
    }
}

void NowPlayingPane::SetVisible(bool visible) {
    if (visible == m_visible) return;
    m_visible = visible;
    m_root.IsVisible(visible);
    if (visible) {
        LayoutParts();
        if (m_mode == Mode::Queue) {
            if (onQueueShown) onQueueShown();
            m_queue.ScrollToCard();
        }
    }
    ApplyTransition();
    if (visible) StartFrames();
}

void NowPlayingPane::SetMode(Mode mode) {
    if (mode == Mode::Queue && m_mode != Mode::Queue && !m_barDragging) {
        if (onQueueShown) onQueueShown();
        m_queue.ScrollToCard();
    }
    m_mode = mode;
    m_t.SetTarget(mode == Mode::Queue ? 1.0f : 0.0f);
    m_lastFrame = Clock::now();
    ApplyTransition();
    StartFrames();
}

void NowPlayingPane::SetTrack(const std::wstring& title, const std::wstring& artist,
                              const com_ptr<IWICBitmapSource>& art) {
    // Unrelated thumbnail completions must not reset a paused lyric presentation.
    if (m_title == title && m_artist == artist && m_art.get() == art.get()) return;
    m_title = title;
    m_artist = artist;
    m_art = art;
    m_lyrics.SetHasTrack(!title.empty());
    if (m_visible) PaintHeader();
}

// Lyrics fade out and rise a little while the queue fades in and rises into place; the header
// follows the queue's song card.
void NowPlayingPane::ApplyTransition() {
    const float t = std::clamp(m_t.Position(), 0.0f, 1.0f), s = m_scale;
    const float lyricsAlpha = std::clamp(1.0f - t * 1.5f, 0.0f, 1.0f);
    const float queueAlpha = std::clamp((t - 0.15f) / 0.85f, 0.0f, 1.0f);
    m_lyrics.SetVisible(m_visible && t < 0.999f);
    m_queue.SetVisible(m_visible && t > 0.001f);
    const RECT qr = QueueRect();
    // The theme roots are children of fixed clipped viewports; these offsets are relative.
    m_lyrics.Root().Opacity(lyricsAlpha);
    m_lyrics.Root().Offset(float3{0, std::round(-t * kLyricsRise * s), 0});
    m_queue.Root().Opacity(queueAlpha);
    m_queue.Root().Offset(float3{0, std::round((1 - t) * kQueueRise * s), 0});
    const float cardY = qr.top + m_queue.CardTop() * s + (1 - t) * kQueueRise * s;
    const float headerY = static_cast<float>(m_bounds.top) * (1 - t) + cardY * t;
    m_header.Offset(float3{static_cast<float>(m_bounds.left), std::round(headerY), 0});
    // The thumb slides with the switch; each segment's dark glyph shows where the thumb is.
    const RECT track = TrackRect(), bar = BarRect();
    const float segW = (track.right - track.left) / 2.0f, inset = kThumbInset * s;
    m_thumb.Offset(float3{std::round(track.left - bar.left + inset + t * segW),
                          std::round(track.top - bar.top + inset), 0});
    m_segDark[0].Opacity(1 - t);
    m_segLight[0].Opacity(t);
    m_segDark[1].Opacity(t);
    m_segLight[1].Opacity(1 - t);
}

bool NowPlayingPane::Tick() {
    if (!m_visible) return false;
    const auto now = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - m_lastFrame).count(), 0.0f, 0.1f);
    m_lastFrame = now;
    m_t.Step(dt);
    bool more = m_t.Moving();
    const float t = m_t.Position();
    if (t < 0.999f) more |= m_lyrics.Tick();
    if (t > 0.001f) more |= m_queue.Tick(dt);
    ApplyTransition();
    return more;
}

// ---- Input -----------------------------------------------------------------------------

bool NowPlayingPane::HitTest(POINT pt) const {
    return m_visible && PtInRect(&m_bounds, pt) != FALSE;
}

RECT NowPlayingPane::TrackRect() const {
    const RECT bar = BarRect();
    const LONG x = static_cast<LONG>(std::lround(kTrackX * m_scale)),
               h = static_cast<LONG>(std::lround(kTrackH * m_scale));
    const LONG top = (bar.top + bar.bottom - h) / 2;
    return {bar.left + x, top, std::max(bar.left + x, bar.right - x), top + h};
}

int NowPlayingPane::ButtonAt(POINT pt) const {
    const RECT track = TrackRect();
    if (!PtInRect(&track, pt)) return -1;
    return pt.x < (track.left + track.right) / 2 ? 0 : 1;
}

NowPlayingPane::Target NowPlayingPane::TargetAt(POINT pt) const {
    if (!HitTest(pt)) return Target::None;
    const RECT bar = BarRect();
    if (PtInRect(&bar, pt)) return Target::Bar;
    if (m_mode == Mode::Queue) return m_queue.HitTest(pt) ? Target::Queue : Target::None;
    return m_lyrics.HitTest(pt) ? Target::Lyrics : Target::None;
}

void NowPlayingPane::OnMouseMove(POINT pt) {
    if (m_capture == Target::Bar && m_buttonDown >= 0) {
        // Dragging the thumb: the switch (and the content with it) follows the pointer.
        const RECT track = TrackRect();
        const float segW = std::max(1.0f, (track.right - track.left) / 2.0f);
        if (!m_barDragging && std::abs(pt.x - m_barDownX) > 4 * m_scale) {
            m_barDragging = true;
            if (m_mode != Mode::Queue) {
                if (onQueueShown) onQueueShown();
                m_queue.ScrollToCard();
            }
        }
        if (m_barDragging) {
            m_t.Snap(std::clamp(m_barDownT + (pt.x - m_barDownX) / segW, 0.0f, 1.0f));
            ApplyTransition();
            StartFrames();
        }
        return;
    }
    if (m_capture == Target::Lyrics) return m_lyrics.OnMouseMove(pt);
    if (m_capture == Target::Queue) return m_queue.OnMouseMove(pt);
    const Target target = TargetAt(pt);
    if (target != m_hoverTarget) {
        if (m_hoverTarget == Target::Lyrics) m_lyrics.OnMouseLeave();
        if (m_hoverTarget == Target::Queue) m_queue.OnMouseLeave();
        m_hoverTarget = target;
    }
    if (target == Target::Lyrics) m_lyrics.OnMouseMove(pt);
    if (target == Target::Queue) m_queue.OnMouseMove(pt);
}

void NowPlayingPane::OnMouseLeave() {
    if (m_hoverTarget == Target::Lyrics) m_lyrics.OnMouseLeave();
    if (m_hoverTarget == Target::Queue) m_queue.OnMouseLeave();
    m_hoverTarget = Target::None;
}

void NowPlayingPane::OnLButtonDown(POINT pt) {
    m_capture = TargetAt(pt);
    switch (m_capture) {
    case Target::Bar:
        m_buttonDown = ButtonAt(pt);
        m_barDownX = pt.x;
        m_barDownT = std::clamp(m_t.Position(), 0.0f, 1.0f);
        m_barDragging = false;
        break;
    case Target::Lyrics:
        m_lyrics.OnLButtonDown(pt);
        break;
    case Target::Queue:
        m_queue.OnLButtonDown(pt);
        break;
    case Target::None:
        break;
    }
}

void NowPlayingPane::OnLButtonUp(POINT pt) {
    const Target target = m_capture;
    m_capture = Target::None;
    switch (target) {
    case Target::Bar:
        if (m_barDragging) {
            const bool queue = m_t.Position() > 0.5f;
            m_barDragging = false;
            SetMode(queue ? Mode::Queue : Mode::Lyrics); // settles on the nearer side
        } else if (const int b = ButtonAt(pt); b >= 0 && b == m_buttonDown) {
            SetMode(b == 0 ? Mode::Lyrics : Mode::Queue);
        }
        m_buttonDown = -1;
        break;
    case Target::Lyrics:
        m_lyrics.OnLButtonUp(pt);
        break;
    case Target::Queue:
        m_queue.OnLButtonUp(pt);
        break;
    case Target::None:
        break;
    }
}

void NowPlayingPane::OnCaptureLost() {
    if (m_capture == Target::Lyrics) m_lyrics.OnCaptureLost();
    if (m_capture == Target::Queue) m_queue.OnCaptureLost();
    m_capture = Target::None;
    m_buttonDown = -1;
    if (m_barDragging) {
        m_barDragging = false;
        SetMode(m_mode); // back to where it was
    }
}

void NowPlayingPane::OnWheel(int delta) {
    if (m_mode == Mode::Queue)
        m_queue.OnWheel(delta);
    else
        m_lyrics.OnWheel(delta);
}

void NowPlayingPane::OnRButtonUp(POINT pt, POINT screen) {
    switch (TargetAt(pt)) {
    case Target::Queue:
        m_queue.OnRButtonUp(pt, screen);
        break;
    case Target::Lyrics:
        if (onLyricsMenu) onLyricsMenu(screen);
        break;
    default:
        break;
    }
}

} // namespace glasswave::ui
