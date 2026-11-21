#pragma once
#include "aero/glass_material.h"

#include <array>
#include <memory>

// The Windows 7 frame both Glasswave windows are made of. DWM is told to leave the frame to us
// (no Windows 11 rounding, accent border, backdrop or shadow; HostBackdropBrush allowed), and
// the window's composition tree starts with:
//
//   Root (unclipped)
//     Clipped: the rounded clip (radius 6 DIP), holding the glass, DWM's side highlights and
//              then whatever the window adds
//     the frame outline, above the clip: clipping an anti-aliased one-pixel line along the
//              same curve multiplies the two coverages and the dark line all but vanishes at
//              the corners while the light line inside it stays
//
// Everything here is scoped to our own window.

namespace glasswave::aero {

class GlassWindow {
  public:
    // atlasArt: Windows 7's own DWM images (aero_atlas.h) for the glass and the highlights.
    GlassWindow(HWND hwnd, const Colorization& colorization, bool atlasArt);
    GlassWindow(const GlassWindow&) = delete;
    GlassWindow& operator=(const GlassWindow&) = delete;

    const winrt::Windows::UI::Composition::ContainerVisual& Root() const { return m_windowRoot; }
    const winrt::Windows::UI::Composition::ContainerVisual& Clipped() const { return m_root; }
    GlassMaterial& Glass() { return *m_glass; }

    // The window's size in pixels. A framed window has Windows 7's rounded corners and frame
    // outline, and (with the atlas art) DWM's side highlights down a side frame `sidePx` wide;
    // an unframed one (maximized, full screen) has none of them.
    void Layout(SIZE window, LONG sidePx, bool framed);
    // Paints what depends on the scale: the outline, the glass's glare and the highlights.
    void SetScale(float scale);
    void SetActive(bool active);

  private:
    void PaintHighlights();

    bool m_atlasArt;
    bool m_active = true;
    float m_scale = 1.0f;
    SIZE m_highlightSize{};
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_windowRoot{nullptr}, m_root{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_clip{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_outline{nullptr};
    std::unique_ptr<GlassMaterial> m_glass;
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 2> m_highlights{nullptr, nullptr};
};

} // namespace glasswave::aero
