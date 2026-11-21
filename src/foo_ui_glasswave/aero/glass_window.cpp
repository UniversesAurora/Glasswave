#include "pch.h"
#include "aero/glass_window.h"
#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "gfx/composition_host.h"

namespace glasswave::aero {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

void Place(const wuc::Visual& v, const RECT& r) {
    v.Offset(float3{static_cast<float>(r.left), static_cast<float>(r.top), 0});
    v.Size(float2{static_cast<float>(std::max<LONG>(0, r.right - r.left)),
                  static_cast<float>(std::max<LONG>(0, r.bottom - r.top))});
}

} // namespace

GlassWindow::GlassWindow(HWND hwnd, const Colorization& colorization, bool atlasArt) : m_atlasArt(atlasArt) {
    // Allow this non-UWP window to use HostBackdropBrush (Windows 11 22000+, documented).
    BOOL enable = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
    // We draw the Win7 corners and border ourselves: no Win11 rounding, no accent border.
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    // The Win11 DWM shadow is replaced by ShadowFrame's Win7-measured shadow.
    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    const auto& comp = gfx::CompositionHost::Get().Compositor();
    auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(
        hwnd, FALSE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(m_target))));

    m_windowRoot = comp.CreateContainerVisual();
    m_windowRoot.RelativeSizeAdjustment(float2{1, 1});
    m_target.Root(m_windowRoot);
    m_root = comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_root);
    m_clip = comp.CreateRoundedRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clip));
    m_outline = comp.CreateSpriteVisual();
    m_outline.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_outline);

    m_glass = std::make_unique<GlassMaterial>(colorization, atlasArt);
    m_root.Children().InsertAtTop(m_glass->Root());
    // DWM's highlights down the upper left and right frame.
    for (auto& v : m_highlights) {
        v = comp.CreateSpriteVisual();
        m_root.Children().InsertAtTop(v);
    }
}

void GlassWindow::Layout(SIZE window, LONG sidePx, bool framed) {
    const float radius = framed ? metrics::kCornerRadius * m_scale : 0.0f;
    m_clip.Size(float2{static_cast<float>(window.cx), static_cast<float>(window.cy)});
    m_clip.CornerRadius(float2{radius, radius});
    m_outline.IsVisible(framed);

    // DWM's side highlights run down the left / right frame from the top and end sharply at
    // H / 4 + 20 DIP (fitted to Win7 captures: 110 px on a 360 px window at 96 DPI, 290 px on
    // a 1050 px window at 144 DPI).
    const bool show = m_atlasArt && framed && sidePx > 0;
    const SIZE size{sidePx, window.cy / 4 + static_cast<LONG>(std::lround(20 * m_scale))};
    for (auto& v : m_highlights) v.IsVisible(show);
    if (!show) return;
    Place(m_highlights[0], {0, 0, size.cx, size.cy});
    Place(m_highlights[1], {window.cx - size.cx, 0, window.cx, size.cy});
    if (size.cx != m_highlightSize.cx || size.cy != m_highlightSize.cy) {
        m_highlightSize = size;
        PaintHighlights();
    }
}

// Frame lines are one device pixel at any DPI (as DWM draws them); the corner scales with DPI.
void GlassWindow::SetScale(float scale) {
    m_scale = scale;
    m_glass->SetScale(scale);
    PaintHighlights();
    const int frameSrc = static_cast<int>(std::lround(32 * scale));
    const float inset = static_cast<float>(std::lround((metrics::kCornerRadius + 2) * scale));
    m_outline.Brush(gfx::CompositionHost::Get().CreateNineGrid(
        frameSrc, frameSrc, scale, inset, true, [&](ID2D1DeviceContext* dc) {
            paint::FrameOutline(dc, D2D1::SizeF(frameSrc / scale, frameSrc / scale), metrics::kCornerRadius);
        }));
}

void GlassWindow::SetActive(bool active) {
    m_glass->SetActive(active);
    if (m_active == active) return;
    m_active = active;
    PaintHighlights();
}

void GlassWindow::PaintHighlights() {
    const SIZE size = m_highlightSize;
    if (!m_atlasArt || size.cx <= 0 || size.cy <= 0) return;
    auto& host = gfx::CompositionHost::Get();
    const D2D1_SIZE_F dip{size.cx / m_scale, size.cy / m_scale};
    for (auto& v : m_highlights)
        host.Repaint(v, size.cx, size.cy, m_scale,
                     [&](ID2D1DeviceContext* dc) { atlas::SideHighlight(dc, m_active, dip); });
}

} // namespace glasswave::aero
