#pragma once

#include "gfx/composition_host.h"

// The wall of album covers behind Windows Media Center's now-playing screen, taken further:
// square tiles edge to edge, each lit like a pillowed block (a light edge up and left, a soft
// shadow down and right), drifting slowly upwards. A swell runs across the wall: tile after
// tile sinks back a little and lets the light behind it through the seams. Tiles enter in a
// ring that spreads from the middle, each springing forward out of the depth while the whole
// wall settles upwards.
//
// Albums come from the media library; empty places show Media Center's own coloured no-cover
// tiles (ehres AUDIO.NOCOVER.*). Everything that moves is a compositor animation; a stopped
// wall costs nothing. Measurements and choices: docs/design/player-window.md.

namespace glasswave::ui {

class AlbumWall {
  public:
    explicit AlbumWall(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~AlbumWall();
    AlbumWall(const AlbumWall&) = delete;
    AlbumWall& operator=(const AlbumWall&) = delete;

    const winrt::Windows::UI::Composition::ContainerVisual& Root() const { return m_root; }
    // One track per album whose cover may appear on the wall.
    void SetAlbums(std::vector<metadb_handle_ptr> albums);
    void SetSize(SIZE px, float scale);
    void SetRunning(bool running); // drift and swell
    void Reveal();                 // the entrance

  private:
    struct Slot {
        metadb_handle_ptr track; // null: a coloured tile
        int color = 0;
        winrt::Windows::UI::Composition::CompositionDrawingSurface surface{nullptr};
        bool loaded = false;  // the cover has been painted
        bool missing = false; // no cover: stays a coloured tile
    };
    struct Tile {
        winrt::Windows::UI::Composition::ContainerVisual holder{nullptr}; // the swell
        winrt::Windows::UI::Composition::SpriteVisual visual{nullptr};    // the entrance
        size_t slot = SIZE_MAX;
    };

    void Rebuild();
    void PaintSlot(Slot& slot);
    void LoadCovers();
    size_t PickSlot(size_t row, size_t col);
    void Assign(Tile& tile, size_t slot);
    void StartDrift();
    void OnDriftEnd();
    void StartSwell(float startDelay);
    void StopSwell();
    uint32_t Random();

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr}, m_rise{nullptr}, m_drift{nullptr};
    std::vector<Slot> m_slots;
    std::vector<std::vector<Tile>> m_rows;
    std::vector<metadb_handle_ptr> m_albums;
    SIZE m_size{};
    float m_scale = 1.0f;
    int m_tile = 0; // px
    bool m_running = false, m_drifting = false, m_revealed = false, m_swelling = false;
    int m_artToken = 0;
    uint64_t m_driftGeneration = 0;
    uint32_t m_random = 0x9E3779B9u;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

} // namespace glasswave::ui
