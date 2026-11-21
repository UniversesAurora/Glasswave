#include "pch.h"
#include "aero/glass_material.h"
#include "aero/aero_atlas.h"
#include "aero/aero_painter.h"
#include "gfx/color_matrix_effect.h"
#include "gfx/composition_host.h"

namespace glasswave::aero {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

// Opaque color used when transparency effects are off: the recipe applied to a mid-grey backdrop.
winrt::Windows::UI::Color SolidApproximation(const Colorization& c) {
    const auto m = BuildGlassColorMatrix(c);
    const float bg = 0.62f;
    auto ch = [&](int out) {
        float v = m[4 * 4 + out];
        for (int in = 0; in < 3; ++in) v += m[in * 4 + out] * bg;
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return winrt::Windows::UI::Color{255, ch(0), ch(1), ch(2)};
}

bool SystemTransparencyEnabled() {
    try {
        return winrt::Windows::UI::ViewManagement::UISettings().AdvancedEffectsEnabled();
    } catch (...) {
        return true;
    }
}

}  // namespace

GlassMaterial::GlassMaterial(const Colorization& colorization, bool dwmArt)
    : m_colorization(colorization), m_dwmArt(dwmArt) {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    m_root = comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment(float2{1, 1});

    m_glass = comp.CreateSpriteVisual();
    m_glass.RelativeSizeAdjustment(float2{1, 1});
    m_root.Children().InsertAtTop(m_glass);
    m_glassOld = comp.CreateSpriteVisual(); // the glass before a faded change
    m_glassOld.RelativeSizeAdjustment(float2{1, 1});
    m_glassOld.Opacity(0.0f);
    m_root.Children().InsertAtTop(m_glassOld);

    m_reflection = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_reflection);
    if (m_dwmArt) {
        // Win7 lights the top-left corner of every glass frame.
        m_glare = comp.CreateSpriteVisual();
        m_glareInactive = comp.CreateSpriteVisual();
        m_glareInactive.Opacity(0.0f);
        m_root.Children().InsertAtTop(m_glare);
        m_root.Children().InsertAtTop(m_glareInactive);
        SetScale(1.0f);
    }

    m_transparency = SystemTransparencyEnabled();
    RebuildBrushes();
    ApplyBrush();
}

void GlassMaterial::RebuildBrushes() {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    auto makeGlass = [&](const Colorization& c) -> wuc::CompositionBrush {
        auto effect = gfx::MakeColorMatrixEffect(BuildGlassColorMatrix(c),
                                                 wuc::CompositionEffectSourceParameter(L"backdrop"));
        auto factory = comp.CreateEffectFactory(effect);
        auto brush = factory.CreateBrush();
        brush.SetSourceParameter(L"backdrop", comp.CreateHostBackdropBrush());
        return brush;
    };
    const Colorization inactive = MakeInactive(m_colorization);
    m_activeBrush = makeGlass(m_colorization);
    m_inactiveBrush = makeGlass(inactive);
    m_basicActive = comp.CreateColorBrush(SolidApproximation(m_colorization));
    m_basicInactive = comp.CreateColorBrush(SolidApproximation(inactive));
}

void GlassMaterial::ApplyBrush() {
    if (m_transparency)
        m_glass.Brush(m_active ? m_activeBrush : m_inactiveBrush);
    else
        m_glass.Brush(m_active ? m_basicActive : m_basicInactive);

    const auto& c = m_active ? m_colorization : MakeInactive(m_colorization);
    if (m_dwmArt) {
        // Theme opacities of the streak texture: 90 % active, 75 % inactive; the glare 75 %.
        m_reflection.Opacity((m_active ? 0.90f : 0.75f) * (m_transparency ? 1.0f : 0.6f));
        m_glare.Opacity(m_active ? 0.75f : 0.0f);
        m_glareInactive.Opacity(m_active ? 0.0f : 0.75f);
        return;
    }
    // Streaks are painted for intensity 0.5.
    m_reflection.Opacity(std::clamp(c.reflectionIntensity / 0.5f, 0.0f, 1.0f) * (m_transparency ? 1.0f : 0.6f));
}

void GlassMaterial::SetScale(float scale) {
    m_scale = scale;
    if (!m_dwmArt) return;
    auto& host = gfx::CompositionHost::Get();
    const int w = static_cast<int>(atlas::kGlarePixels.width), h = static_cast<int>(atlas::kGlarePixels.height);
    const D2D1_SIZE_F dip{w / scale, h / scale};
    host.Repaint(m_glare, w, h, scale, [&](ID2D1DeviceContext* dc) { atlas::Glare(dc, true, dip); });
    host.Repaint(m_glareInactive, w, h, scale, [&](ID2D1DeviceContext* dc) { atlas::Glare(dc, false, dip); });
}

void GlassMaterial::SetActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    ApplyBrush();
}

void GlassMaterial::SetColorization(const Colorization& colorization, int fadeMs) {
    const auto same = [](const Colorization& a, const Colorization& b) {
        return a.color == b.color && a.afterglow == b.afterglow && a.colorBalance == b.colorBalance &&
               a.afterglowBalance == b.afterglowBalance && a.blurBalance == b.blurBalance &&
               a.reflectionIntensity == b.reflectionIntensity;
    };
    if (same(colorization, m_colorization)) return;
    if (fadeMs > 0 && m_glass.Brush()) {
        m_glassOld.Brush(m_glass.Brush());
        m_glassOld.Opacity(1.0f);
        gfx::CompositionHost::Get().AnimateOpacity(m_glassOld, 0.0f, fadeMs);
    }
    m_colorization = colorization;
    RebuildBrushes();
    ApplyBrush();
}

void GlassMaterial::RefreshTransparencyPolicy() {
    const bool t = SystemTransparencyEnabled();
    if (t == m_transparency) return;
    m_transparency = t;
    ApplyBrush();
}

void GlassMaterial::UpdateReflectionAnchor(HWND hwnd) {
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    UpdateReflectionAnchor(hwnd, wr);
}

void GlassMaterial::UpdateReflectionAnchor(HWND hwnd, const RECT& windowRect) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromRect(&windowRect, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const RECT mon = mi.rcMonitor;
    const int w = mon.right - mon.left, h = mon.bottom - mon.top;

    if (!EqualRect(&mon, &m_reflectionMonitor)) {
        m_reflectionMonitor = mon;
        // The streaks are soft: paint at 1/4 resolution and stretch to save video memory.
        auto& host = gfx::CompositionHost::Get();
        // Win7's texture is 802x604 and soft: keep it at that size and stretch it over the
        // monitor like DWM does. The procedural streaks are painted at 1/4 resolution.
        const int sw = m_dwmArt ? 802 : std::max(w / 4, 64), sh = m_dwmArt ? 604 : std::max(h / 4, 64);
        auto surface = host.CreateSurface(sw, sh);
        host.Draw(surface, 1.0f, [&](ID2D1DeviceContext* dc) {
            const auto size = D2D1::SizeF(static_cast<float>(sw), static_cast<float>(sh));
            if (m_dwmArt) atlas::Streaks(dc, size);
            else paint::Reflection(dc, size);
        });
        auto brush = host.Compositor().CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::Fill);
        m_reflection.Brush(brush);
        m_reflection.Size(float2{static_cast<float>(w), static_cast<float>(h)});
    }

    // The client area starts at the window origin plus the current window-to-client inset
    // (zero unless maximized), which a move does not change.
    POINT client{0, 0};
    ClientToScreen(hwnd, &client);
    RECT current{};
    GetWindowRect(hwnd, &current);
    const LONG ox = windowRect.left + (client.x - current.left), oy = windowRect.top + (client.y - current.top);
    m_reflection.Offset(float3{static_cast<float>(mon.left - ox), static_cast<float>(mon.top - oy), 0});
}

}  // namespace glasswave::aero
