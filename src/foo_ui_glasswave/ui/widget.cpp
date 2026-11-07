#include "pch.h"
#include "ui/widget.h"
#include "aero/aero_metrics.h"
#include "gfx/composition_host.h"

namespace glasswave::ui {

namespace m = aero::metrics;
using aero::paint::Layer;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

Widget::Widget(int id, Painter painter, Options options)
    : m_id(id), m_painter(std::move(painter)), m_options(std::move(options)) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    auto children = m_root.Children();
    if (m_options.haloFactor > 0 && m_options.halo) {
        m_halo = comp.CreateSpriteVisual();
        m_halo.Opacity(0.0f);
        children.InsertAtTop(m_halo);
    }
    m_normal = comp.CreateSpriteVisual();
    children.InsertAtTop(m_normal);
    if (m_options.inactiveLayer) {
        m_inactiveLayer = comp.CreateSpriteVisual();
        m_inactiveLayer.Opacity(0.0f);
        children.InsertAtTop(m_inactiveLayer);
    }
    m_hoverLayer = comp.CreateSpriteVisual();
    m_pressedLayer = comp.CreateSpriteVisual();
    m_hoverLayer.Opacity(0.0f);
    m_pressedLayer.Opacity(0.0f);
    children.InsertAtTop(m_hoverLayer);
    children.InsertAtTop(m_pressedLayer);
}

void Widget::SetBounds(const RECT& b, float scale) {
    const bool sizeChanged = (b.right - b.left) != (m_bounds.right - m_bounds.left) ||
                             (b.bottom - b.top) != (m_bounds.bottom - m_bounds.top) || scale != m_scale;
    m_bounds = b;
    m_scale = scale;
    m_root.Offset(float3{static_cast<float>(b.left), static_cast<float>(b.top), 0});
    m_root.Size(float2{static_cast<float>(b.right - b.left), static_cast<float>(b.bottom - b.top)});
    if (sizeChanged) Repaint();
}

void Widget::Invalidate() { Repaint(); }

void Widget::Repaint() {
    auto& host = gfx::CompositionHost::Get();
    const int w = m_bounds.right - m_bounds.left, h = m_bounds.bottom - m_bounds.top;
    if (w <= 0 || h <= 0) return;
    const D2D1_SIZE_F dip{w / m_scale, h / m_scale};
    std::vector<std::pair<winrt::Windows::UI::Composition::SpriteVisual, Layer>> layers{
        {m_normal, Layer::Normal}, {m_hoverLayer, Layer::Hover}, {m_pressedLayer, Layer::Pressed}};
    if (m_inactiveLayer) layers.emplace_back(m_inactiveLayer, Layer::Inactive);
    for (auto& [visual, layer] : layers)
        host.Repaint(visual, w, h, m_scale, [&](ID2D1DeviceContext* dc) { m_painter(dc, dip, layer); });
    if (m_halo) {
        const int hs = static_cast<int>(std::lround(std::max(w, h) * m_options.haloFactor));
        host.Repaint(m_halo, hs, hs, m_scale, [&](ID2D1DeviceContext* dc) { m_options.halo(dc, hs / m_scale); });
        m_halo.Offset(float3{std::round((w - hs) / 2.0f), std::round((h - hs) / 2.0f), 0});
    }
}

bool Widget::HitTest(POINT pt) const {
    if (!PtInRect(&m_bounds, pt)) return false;
    if (!m_options.circular) return true;
    const float cx = (m_bounds.left + m_bounds.right) / 2.0f, cy = (m_bounds.top + m_bounds.bottom) / 2.0f;
    const float r = (m_bounds.right - m_bounds.left) / 2.0f;
    const float dx = pt.x - cx, dy = pt.y - cy;
    return dx * dx + dy * dy <= r * r;
}

void Widget::SetHover(bool hover) {
    if (m_hover == hover) return;
    m_hover = hover;
    auto& host = gfx::CompositionHost::Get();
    host.AnimateOpacity(m_hoverLayer, hover ? 1.0f : 0.0f, hover ? m::kHoverInMs : m::kHoverOutMs);
    if (m_halo) {
        // WMC-style breathing halo: loops only while hovered, fades out after; idle = no frames
        if (hover)
            host.StartPulse(m_halo, 0.30f, 0.85f, m::kPulsePeriodMs);
        else
            host.StopPulse(m_halo, 0.0f, m::kHoverOutMs);
    }
}

void Widget::SetPressed(bool pressed) {
    if (m_pressed == pressed) return;
    m_pressed = pressed;
    gfx::CompositionHost::Get().AnimateOpacity(m_pressedLayer, pressed ? 1.0f : 0.0f, pressed ? m::kPressMs : 200);
}

void Widget::SetInactive(bool inactive) {
    if (m_inactive == inactive) return;
    m_inactive = inactive;
    auto& host = gfx::CompositionHost::Get();
    if (m_inactiveLayer) {
        host.AnimateOpacity(m_inactiveLayer, inactive ? 1.0f : 0.0f, 180);
        host.AnimateOpacity(m_normal, inactive ? 0.0f : 1.0f, 180);
    } else {
        host.AnimateOpacity(m_normal, inactive ? 0.75f : 1.0f, 180);
    }
}

}  // namespace glasswave::ui
