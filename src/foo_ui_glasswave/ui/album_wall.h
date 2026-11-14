#pragma once

#include "gfx/composition_host.h"

// The wall of album covers behind Windows Media Center's now-playing screen: square tiles
// packed edge to edge, drifting slowly upwards.
//
// The grid is laid from the window's corner and its tiles have one size on a monitor, whatever
// the form of the window: a window that grows keeps the tiles it showed, and the places it
// uncovers fill with tiles falling into them. Tiles enter as in Media Center: one after another
// at random places, each falling straight down onto the wall from above and settling.
//
// The tiles float like boxes on very slow water. They never part; their rise and fall shows in
// the shadow each casts on its lower and right neighbours, which deepens and fades as the swell
// passes under them.
//
// The covers are those of the playing playlist, each album once; an album without a cover
// takes no part. They are dealt like cards: the albums (with Media Center's own coloured no-cover
// tiles, ehres AUDIO.NOCOVER.*, when there are few) are shuffled and laid one by one onto the
// places as they appear; when the deck runs out it is shuffled again. A card whose cover has
// loaded is dealt before one still loading, and one that would lie next to the same picture
// waits for a later place. The cards next in line load ahead of the drift. Covers are painted
// only while a tile shows them. Everything that moves is a compositor animation; a stopped wall
// costs nothing. Measurements and choices: docs/design/player-window.md.

namespace glasswave::ui {

class AlbumWall {
  public:
    explicit AlbumWall(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~AlbumWall();
    AlbumWall(const AlbumWall&) = delete;
    AlbumWall& operator=(const AlbumWall&) = delete;

    const winrt::Windows::UI::Composition::ContainerVisual& Root() const { return m_root; }
    // One track per album whose cover may appear on the wall, each album once.
    void SetAlbums(std::vector<metadb_handle_ptr> albums);
    // `px`: the window's size; the monitor's (px) sets the tile size and the entrance's pace.
    // Places uncovered by a larger window fill with falling tiles.
    void SetGeometry(SIZE px, SIZE monitor, float scale);
    void SetRunning(bool running); // drift and swell
    void SetPaused(bool paused);   // running at a fraction of the pace (playback paused)
    void Reveal();                 // the entrance

  private:
    struct Slot {
        metadb_handle_ptr track; // null: a coloured tile
        int color = 0;
        winrt::Windows::UI::Composition::CompositionDrawingSurface surface{nullptr};
        // The bitmap painted (null: the coloured tile). A smaller thumbnail stands in, scaled
        // up, while the tile's own size decodes; the sharp one replaces it when it arrives.
        winrt::com_ptr<IWICBitmapSource> painted;
        bool missing = false;   // no cover: leaves the deck
        bool requested = false; // its cover was asked for and loads in the background
        bool ready = false;     // its cover is decoded: it can be shown at once
    };
    struct Tile {
        winrt::Windows::UI::Composition::ContainerVisual holder{nullptr}; // the fall
        winrt::Windows::UI::Composition::SpriteVisual drop{nullptr};      // its shadow while falling
        winrt::Windows::UI::Composition::SpriteVisual visual{nullptr};    // the cover
        // What it showed before its cover arrived, fading out over it (made on first use).
        winrt::Windows::UI::Composition::SpriteVisual previous{nullptr};
        winrt::Windows::UI::Composition::SpriteVisual shade{nullptr};     // neighbours' shadow
        winrt::Windows::UI::Composition::SpriteVisual dim{nullptr};       // lower: darker
        winrt::Windows::UI::Composition::CompositionDrawingSurface shown{nullptr}; // in its brush
        size_t slot = SIZE_MAX;
        int64_t cell = INT64_MIN; // the place on the grid it shows
    };

    void Rebuild(bool fillByFalling);
    void PaintSlot(Slot& slot);
    void PaintShadows();
    void LoadCovers();
    void ReleaseHidden();
    int CoverPx() const;
    void RequestCover(Slot& slot);
    void Preload();
    void BuildDeck();
    void ShuffleIn();
    void Retire(size_t slot);
    size_t PickSlot(int col, int row);
    void Assign(Tile& tile, size_t slot);
    void CrossFade(size_t slot, const winrt::Windows::UI::Composition::CompositionDrawingSurface& before,
                   bool sharpening);
    void Drop(Tile& tile, float delaySeconds, int durationMs);
    void StartDrift();
    void OnDriftEnd();
    void AttachSwell(Tile& tile, size_t row, size_t col);
    void RunClock(bool run);
    void RaiseSwell(float delaySeconds);
    uint32_t Random();
    float Unit() { return static_cast<float>(Random() % 10000) / 10000.0f; }

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr}, m_grid{nullptr}, m_drift{nullptr};
    winrt::Windows::UI::Composition::CompositionSurfaceBrush m_shadeBrush{nullptr}, m_dropBrush{nullptr};
    std::vector<Slot> m_slots; // the albums, then the six coloured tiles
    std::vector<size_t> m_deck; // one round of cards: slots (coloured ones may repeat)
    std::vector<size_t> m_hand; // what is left of the shuffled rounds, dealt from the back
    size_t m_lastDealt = SIZE_MAX;
    std::vector<std::vector<Tile>> m_rows;
    std::unordered_map<int64_t, size_t> m_cells; // grid place -> slot, kept while seen
    SIZE m_size{};
    int m_monitorHeight = 0, m_monitorWidth = 0;
    float m_scale = 1.0f;
    int m_tile = 0;      // px
    int m_driftRows = 0; // rows the drift has carried away
    bool m_running = false, m_drifting = false, m_revealed = false, m_clockRunning = false;
    // The swell's clock: "t" in seconds while running, "amp" 0..1 (the swell rises after the
    // entrance). Every tile's shadows are expressions of it.
    winrt::Windows::UI::Composition::CompositionPropertySet m_clock{nullptr};
    float m_clockSeconds = 0;
    float m_pace = 1; // drift and clock speed (kPausedPace while paused)
    void ApplyPace();
    std::chrono::steady_clock::time_point m_clockStarted{};
    int m_artToken = 0;
    uint64_t m_driftGeneration = 0;
    uint32_t m_random = 0x9E3779B9u;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

} // namespace glasswave::ui
