#pragma once
#include "aero/aero_painter.h"

// An interactive control made of pre-painted layers:
//   [halo] normal / inactive -> hover -> pressed
// State changes only animate layer opacity in the compositor (no repaint);
// layers are repainted only when size, DPI or content (e.g. play/pause) changes.
// Layers are always shown 1:1 (visuals are never scaled) so hairlines stay crisp.

namespace glasswave::ui {

class Widget {
public:
    using Painter = std::function<void(ID2D1DeviceContext*, D2D1_SIZE_F, aero::paint::Layer)>;
    using HaloPainter = std::function<void(ID2D1DeviceContext*, float)>;

    struct Options {
        bool circular = false;       // circular hit testing
        bool inactiveLayer = false;  // separate "window inactive" look (caption buttons)
        float haloFactor = 0.0f;     // > 0: add a halo layer of size * haloFactor
        bool haloPulse = true;       // breathe while hovered (WMC); false: plain fade (WMP)
        HaloPainter halo;
        // Rectangular hover glow below the control, extending `glowOutset` (DIP: left, top,
        // right, bottom) beyond it; painted with the glow's full size.
        std::function<void(ID2D1DeviceContext*, D2D1_SIZE_F)> glow;
        D2D1_RECT_F glowOutset{};
        bool glowOutsetInPixels = false;  // outset given in device pixels (DWM nine-grid borders)
    };

    Widget(int id, Painter painter, Options options);

    int Id() const { return m_id; }
    const winrt::Windows::UI::Composition::ContainerVisual& Visual() const { return m_root; }
    const RECT& Bounds() const { return m_bounds; }

    void SetBounds(const RECT& boundsPx, float scale);
    void Invalidate();  // repaint all layers after a content change
    bool HitTest(POINT pt) const;

    void SetHover(bool hover);
    void SetPressed(bool pressed);
    void SetInactive(bool inactive);  // window lost activation

    std::function<void()> onClick;
    // Optional: a disabled widget shows no hover / pressed feedback and ignores clicks.
    std::function<bool()> enabled;
    bool IsEnabled() const { return !enabled || enabled(); }

private:
    void Repaint();

    int m_id;
    Painter m_painter;
    Options m_options;
    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_hover = false, m_pressed = false, m_inactive = false;

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_halo{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_glow{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_normal{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_inactiveLayer{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_hoverLayer{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_pressedLayer{nullptr};
};

}  // namespace glasswave::ui
