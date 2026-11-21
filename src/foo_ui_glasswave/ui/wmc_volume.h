#pragma once

#include "gfx/composition_host.h"

// Media Center's volume overlay, from its own artwork (ehres OVERLAYS.*): the glass outline,
// the blue plate, the level in figures (the speaker with its cross while muted) and the bar
// along its foot. It fades in and out where it stands, as Media Center's does.
//
// The bubble is the mini window's: the level in figures in a small pane of Aero glass over
// the volume control's thumb, frosted, with the window frame's glare and reflections.
//
// Measurements: docs/design/player-window.md, "Volume".

namespace glasswave::ui {

class WmcVolume {
  public:
    enum class Kind {
        Panel, // 160 x 86 DIP
        Bubble // 40 x 24 DIP
    };
    static constexpr float kPanelW = 160, kPanelH = 86, kBubbleW = 40, kBubbleH = 24, kBubbleRadius = 4,
                           kPanelRadius = 5;

    WmcVolume(const winrt::Windows::UI::Composition::ContainerVisual& parent, Kind kind);
    ~WmcVolume();
    WmcVolume(const WmcVolume&) = delete;
    WmcVolume& operator=(const WmcVolume&) = delete;

    // Top left in px of the parent; the size is the kind's.
    void SetPosition(POINT topLeft, float scale);
    void SetLevel(float level, bool muted);
    void Show(bool show);
    bool Shown() const { return m_shown; }
    RECT Bounds() const;
    RECT Bar() const; // the panel's bar in px of the parent, where a press sets the level

  private:
    void Paint();
    void PaintBubble(ID2D1DeviceContext* dc);

    Kind m_kind;
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr}; // placed and faded
    winrt::Windows::UI::Composition::SpriteVisual m_glass{nullptr};   // blurred glass under it
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_glassShape{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr};
    core::Signal<>::Connection m_deviceReplaced; // its surface is empty after a device loss
    gfx::RetainedSurface m_backing;
    // The panel's lit bar, over the rest, cut at the level by its clip, which springs there.
    winrt::Windows::UI::Composition::SpriteVisual m_barOn{nullptr};
    winrt::Windows::UI::Composition::InsetClip m_barClip{nullptr};
    gfx::RetainedSurface m_barBacking;
    void PlaceLevel(bool animate);
    POINT m_at{};
    float m_scale = 0, m_level = -1;
    bool m_muted = false, m_shown = false;
    std::shared_ptr<bool> m_showing = std::make_shared<bool>(false); // for the fade's end
};

} // namespace glasswave::ui
