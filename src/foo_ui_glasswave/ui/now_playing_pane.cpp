#include "pch.h"

#include "ui/now_playing_pane.h"

#include "gfx/composition_host.h"
#include "ui/pane_common.h"

#include <winrt/Windows.Foundation.Collections.h>

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using Clock = std::chrono::steady_clock;

namespace {

constexpr float kButton = 40;          // DIP: bottom bar button (highlight circle)
constexpr float kButtonX[2] = {0.23f, 0.77f};  // fractions of the pane width (Apple Music's left / right)
constexpr float kButtonPlate = 0.22f;  // white alpha of the active button's circle
constexpr SpringParams kSwitchSpring{1, 260, 30};
constexpr float kLyricsRise = 24, kQueueRise = 48;  // DIP the two sides move while switching

wuc::ContainerVisual MakeRoot(const wuc::ContainerVisual& parent) {
    auto root = gfx::CompositionHost::Get().Compositor().CreateContainerVisual();
    root.IsVisible(false);
    parent.Children().InsertAtTop(root);
    return root;
}

void DrawLyricsIcon(ID2D1DeviceContext* dc, D2D1_POINT_2F c, ID2D1Brush* ink) {
    com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    const float l = c.x - 10, t = c.y - 8.5f, r = c.x + 10, b = c.y + 5.5f, rad = 4;
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
    sink->AddLine({l + 9, b});
    sink->AddLine({l + 3.5f, b + 5});
    sink->AddLine({l + 4, b});
    sink->AddLine({l + rad, b});
    arc(l, b - rad);
    sink->AddLine({l, t + rad});
    arc(l + rad, t);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    dc->DrawGeometry(path.get(), ink, 1.8f);
    for (const float x : {c.x - 3.0f, c.x + 3.2f}) {
        dc->FillEllipse(D2D1::Ellipse({x, c.y - 2.4f}, 1.8f, 1.8f), ink);
        dc->DrawLine({x + 1.5f, c.y - 2.0f}, {x - 0.8f, c.y + 1.6f}, ink, 1.4f);
    }
}

void DrawQueueIcon(ID2D1DeviceContext* dc, D2D1_POINT_2F c, ID2D1Brush* ink) {
    for (int k = -1; k <= 1; ++k) {
        const float y = c.y + k * 6.0f;
        dc->FillEllipse(D2D1::Ellipse({c.x - 8, y}, 1.6f, 1.6f), ink);
        dc->DrawLine({c.x - 4, y}, {c.x + 10, y}, ink, 1.8f);
    }
}

}  // namespace

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
    for (size_t i = 0; i < 2; ++i) {
        m_buttonPlate[i] = comp.CreateShapeVisual();
        auto circle = comp.CreateEllipseGeometry();
        auto shape = comp.CreateSpriteShape(circle);
        shape.FillBrush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(
            static_cast<uint8_t>(std::lround(kButtonPlate * 255)), 255, 255, 255)));
        m_buttonPlate[i].Shapes().Append(shape);
        m_bar.Children().InsertAtTop(m_buttonPlate[i]);
        m_buttonIcon[i] = comp.CreateSpriteVisual();
        m_bar.Children().InsertAtTop(m_buttonIcon[i]);
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

RECT NowPlayingPane::QueueRect() const { return {m_bounds.left, m_bounds.top, m_bounds.right, BarRect().top}; }

void NowPlayingPane::SetBounds(const RECT& px, float scale) {
    const bool sizeChanged = px.right - px.left != m_bounds.right - m_bounds.left ||
                             px.bottom - px.top != m_bounds.bottom - m_bounds.top || scale != m_scale;
    m_bounds = px;
    m_scale = scale;
    m_lyrics.SetBounds(LyricsRect(), scale);
    m_queue.SetBounds(QueueRect(), scale);
    if (sizeChanged) LayoutParts();
    ApplyTransition();
}

void NowPlayingPane::LayoutParts() {
    const float w = static_cast<float>(std::max<LONG>(0, m_bounds.right - m_bounds.left));
    const float h = static_cast<float>(std::max<LONG>(0, m_bounds.bottom - m_bounds.top));
    m_tint.Offset(float3{static_cast<float>(m_bounds.left), static_cast<float>(m_bounds.top), 0});
    m_tint.Size(float2{w, h});
    const RECT bar = BarRect();
    m_bar.Offset(float3{static_cast<float>(bar.left), static_cast<float>(bar.top), 0});
    m_bar.Size(float2{w, static_cast<float>(bar.bottom - bar.top)});
    const float d = kButton * m_scale, cy = (bar.bottom - bar.top) / 2.0f;
    for (size_t i = 0; i < 2; ++i) {
        const float cx = std::round(w * kButtonX[i]);
        m_buttonPlate[i].Offset(float3{cx - d / 2, std::round(cy - d / 2), 0});
        m_buttonPlate[i].Size(float2{d, d});
        auto circle = m_buttonPlate[i].Shapes().GetAt(0).as<wuc::CompositionSpriteShape>().Geometry().as<wuc::CompositionEllipseGeometry>();
        circle.Center(float2{d / 2, d / 2});
        circle.Radius(float2{d / 2, d / 2});
        m_buttonIcon[i].Offset(float3{cx - d / 2, std::round(cy - d / 2), 0});
    }
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
    const int d = static_cast<int>(std::ceil(kButton * m_scale));
    for (size_t i = 0; i < 2; ++i) {
        host.Repaint(m_buttonIcon[i], d, d, m_scale, [&](ID2D1DeviceContext* dc) {
            auto ink = pane::White(dc, 0.95f);
            const D2D1_POINT_2F c{kButton / 2, kButton / 2};
            if (i == 0) DrawLyricsIcon(dc, c, ink.get());
            else DrawQueueIcon(dc, c, ink.get());
        });
        m_buttonIcon[i].Size(float2{static_cast<float>(d), static_cast<float>(d)});
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
    if (mode == m_mode) return;
    m_mode = mode;
    if (mode == Mode::Queue) {
        if (onQueueShown) onQueueShown();
        m_queue.ScrollToCard();
    }
    m_t.SetTarget(mode == Mode::Queue ? 1.0f : 0.0f);
    m_lastFrame = Clock::now();
    ApplyTransition();
    StartFrames();
}

void NowPlayingPane::SetTrack(const std::wstring& title, const std::wstring& artist, const com_ptr<IWICBitmapSource>& art) {
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
    const RECT lr = LyricsRect(), qr = QueueRect();
    m_lyrics.Root().Opacity(lyricsAlpha);
    m_lyrics.Root().Offset(float3{static_cast<float>(lr.left), std::round(lr.top - t * kLyricsRise * s), 0});
    m_queue.Root().Opacity(queueAlpha);
    m_queue.Root().Offset(float3{static_cast<float>(qr.left), std::round(qr.top + (1 - t) * kQueueRise * s), 0});
    const float cardY = qr.top + m_queue.CardTop() * s + (1 - t) * kQueueRise * s;
    const float headerY = static_cast<float>(m_bounds.top) * (1 - t) + cardY * t;
    m_header.Offset(float3{static_cast<float>(m_bounds.left), std::round(headerY), 0});
    m_buttonPlate[0].Opacity(1 - t);
    m_buttonPlate[1].Opacity(t);
    m_buttonIcon[0].Opacity(0.6f + 0.4f * (1 - t));
    m_buttonIcon[1].Opacity(0.6f + 0.4f * t);
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

bool NowPlayingPane::HitTest(POINT pt) const { return m_visible && PtInRect(&m_bounds, pt) != FALSE; }

int NowPlayingPane::ButtonAt(POINT pt) const {
    const RECT bar = BarRect();
    if (!PtInRect(&bar, pt)) return -1;
    const float w = static_cast<float>(m_bounds.right - m_bounds.left);
    const float cy = (bar.top + bar.bottom) / 2.0f, r = kButton * m_scale / 2 + 6 * m_scale;
    for (int i = 0; i < 2; ++i) {
        const float cx = m_bounds.left + w * kButtonX[i];
        if (std::abs(pt.x - cx) <= r && std::abs(pt.y - cy) <= r) return i;
    }
    return -1;
}

NowPlayingPane::Target NowPlayingPane::TargetAt(POINT pt) const {
    if (!HitTest(pt)) return Target::None;
    const RECT bar = BarRect();
    if (PtInRect(&bar, pt)) return Target::Bar;
    if (m_mode == Mode::Queue) return m_queue.HitTest(pt) ? Target::Queue : Target::None;
    return m_lyrics.HitTest(pt) ? Target::Lyrics : Target::None;
}

void NowPlayingPane::OnMouseMove(POINT pt) {
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
    case Target::Bar: m_buttonDown = ButtonAt(pt); break;
    case Target::Lyrics: m_lyrics.OnLButtonDown(pt); break;
    case Target::Queue: m_queue.OnLButtonDown(pt); break;
    case Target::None: break;
    }
}

void NowPlayingPane::OnLButtonUp(POINT pt) {
    const Target target = m_capture;
    m_capture = Target::None;
    switch (target) {
    case Target::Bar:
        if (const int b = ButtonAt(pt); b >= 0 && b == m_buttonDown) SetMode(b == 0 ? Mode::Lyrics : Mode::Queue);
        m_buttonDown = -1;
        break;
    case Target::Lyrics: m_lyrics.OnLButtonUp(pt); break;
    case Target::Queue: m_queue.OnLButtonUp(pt); break;
    case Target::None: break;
    }
}

void NowPlayingPane::OnCaptureLost() {
    if (m_capture == Target::Lyrics) m_lyrics.OnCaptureLost();
    if (m_capture == Target::Queue) m_queue.OnCaptureLost();
    m_capture = Target::None;
    m_buttonDown = -1;
}

void NowPlayingPane::OnWheel(int delta) {
    if (m_mode == Mode::Queue) m_queue.OnWheel(delta);
    else m_lyrics.OnWheel(delta);
}

void NowPlayingPane::OnRButtonUp(POINT pt, POINT screen) {
    switch (TargetAt(pt)) {
    case Target::Queue: m_queue.OnRButtonUp(pt, screen); break;
    case Target::Lyrics:
        if (onLyricsMenu) onLyricsMenu(screen);
        break;
    default: break;
    }
}

}  // namespace glasswave::ui
