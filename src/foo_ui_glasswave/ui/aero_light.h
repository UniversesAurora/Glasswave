#pragma once

#include "gfx/composition_host.h"

// Windows Media Center's living background: a deep blue ground with soft layers of light
// and thin shards that drift and breathe, like sunlight reaching into deep water. The
// artwork is Media Center's own (ehres COMMON.BACKGROUND.*, ADR 0007); where the user's
// files lack it a plain blue gradient stands in.
//
// Every motion is a looping compositor animation; SetRunning(false) pauses them all.
// Measurements and choices: docs/design/player-window.md.

namespace glasswave::ui {

class AeroLight {
  public:
    enum class Kind {
        Ground, // the whole background, opaque
        Veil    // only the moving light, to lay over other content
    };

    AeroLight(const winrt::Windows::UI::Composition::ContainerVisual& parent, Kind kind, bool atBottom);
    ~AeroLight();
    AeroLight(const AeroLight&) = delete;
    AeroLight& operator=(const AeroLight&) = delete;

    const winrt::Windows::UI::Composition::ContainerVisual& Root() const { return m_root; }
    void SetSize(SIZE px);
    void SetRunning(bool running);

  private:
    struct Piece {
        winrt::Windows::UI::Composition::SpriteVisual visual{nullptr};
        float x = 0, y = 0, w = 1, h = 1; // placement in fractions of the size
        float sway = 0;                   // horizontal travel, fraction of the width
        float swaySeconds = 0;
        float low = 1, high = 1, breathSeconds = 0, delay = 0;
        float angle = 0; // degrees
    };

    void Add(const wchar_t* resource, Piece piece);
    void Layout();
    void Animate(Piece& piece);

    Kind m_kind;
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_ground{nullptr};
    std::vector<Piece> m_pieces;
    SIZE m_size{};
    bool m_running = false, m_started = false;
};

} // namespace glasswave::ui
