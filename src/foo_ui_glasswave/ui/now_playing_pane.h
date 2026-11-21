#pragma once

#include "core/signal.h"
#include "lyrics/lyrics_service.h"
#include "ui/lyrics_view.h"
#include "ui/pane_common.h"
#include "ui/queue_view.h"
#include "ui/spring.h"

// The pane on the glass right of the client area, after Apple Music's now-playing sheet:
// the current song's header, then either the lyrics or the queue, switched by a segmented
// control at the bottom (an iOS tab switch: a light thumb slides between Lyrics and Up Next;
// it can be dragged, and the content follows it). Switching cross-fades the two with a short
// rise; the header stays and, in the queue, scrolls along as the queue's song card.

namespace glasswave::ui {

class AeroLight;

class NowPlayingPane {
  public:
    enum class Mode {
        Lyrics,
        Queue
    };

    explicit NowPlayingPane(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~NowPlayingPane();
    NowPlayingPane(const NowPlayingPane&) = delete;
    NowPlayingPane& operator=(const NowPlayingPane&) = delete;

    // Moves the pane under another container (the player window borrows it).
    void Reparent(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    void SetBounds(const RECT& px, float scale);
    void SetVisible(bool visible);
    // Embedded in the player window: no tint of its own and no switch at the bottom (the
    // host shades the background and has its own lyrics / queue buttons). Bare also drops
    // the song header and the queue's card and capsules, where the host shows them itself.
    void SetEmbedded(bool embedded, bool bare = false);
    // DIP: the least height that keeps the side showing usable (the lyrics: the tallest line
    // whole under the header).
    float MinHeight() const;
    // The lyrics' size, fonts or translation side changed (settings::LyricStyleGeneration).
    void RestyleLyrics();
    bool Visible() const { return m_visible; }
    void SetMode(Mode mode);
    Mode CurrentMode() const { return m_mode; }

    void SetTrack(const std::wstring& title, const std::wstring& artist,
                  const winrt::com_ptr<IWICBitmapSource>& art);
    // The playing cover's colour (ui/cover_tint.h) under the lyrics; none: the window's tint.
    void SetCoverTint(std::optional<uint32_t> rgb);
    void SetActive(bool active); // the window's focus: an inactive one shows no colour
    void SetResult(const lyrics::Result& result) { m_lyrics.SetResult(result); }
    void SetPlaying(bool playing, bool paused) { m_lyrics.SetPlaying(playing, paused); }
    void OnPlaybackTime() { m_lyrics.Resync(); } // the position moved: a seek wakes an idle view
    QueueView& Queue() { return m_queue; }

    bool Tick(); // per frame; returns whether frames are still needed

    bool HitTest(POINT pt) const;
    void OnMouseMove(POINT pt);
    void OnMouseLeave();
    void OnLButtonDown(POINT pt);
    void OnLButtonUp(POINT pt);
    void OnCaptureLost();
    void OnWheel(int delta);
    void OnRButtonUp(POINT pt, POINT screen);

    std::function<void(double seconds)> onSeek;
    std::function<void()> onFrames;
    std::function<void()> onQueueShown; // the queue is about to show: refresh its data
    std::function<void(POINT screen)> onLyricsMenu;

  private:
    enum class Target {
        None,
        Bar,
        Lyrics,
        Queue
    };
    Target TargetAt(POINT pt) const;
    void LayoutGeometry();
    void LayoutParts();
    bool PaintHeader();
    void PaintBar();
    void ApplyTransition();
    void StartFrames();
    RECT LyricsRect() const;
    RECT QueueRect() const;
    RECT BarRect() const;
    int ButtonAt(POINT pt) const; // segment under the point: 0 lyrics, 1 queue, -1 none
    RECT TrackRect() const;       // the segmented control, client px

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_tint{nullptr}, m_header{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_coverTint{nullptr};
    winrt::Windows::UI::Composition::CompositionColorBrush m_coverBrush{nullptr};
    std::optional<uint32_t> m_coverRgb;
    static constexpr float kCoverTintAlpha = 0.34f;
    // In the library window's sidebar: Media Center's moving light over the tint, as behind the
    // mini player and the full screen (owner, 2026-11-13). Lent to the player, the pane has none.
    std::unique_ptr<AeroLight> m_light;
    void UpdateLight();
    winrt::Windows::UI::Composition::ContainerVisual m_bar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_track{nullptr}, m_thumb{nullptr};
    pane::CapsuleGlass m_trackGlass, m_thumbGlass; // the material under the switch and its thumb
    // Each segment's icon and label twice: white on the track, dark on the thumb.
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 2> m_segLight{nullptr, nullptr},
        m_segDark{nullptr, nullptr};
    winrt::com_ptr<ID2D1DeviceContext> m_dc;
    core::Signal<>::Connection m_deviceReplaced; // the context belongs to the rendering device
    LyricsView m_lyrics;
    QueueView m_queue;
    RECT m_bounds{};
    float m_scale = 1.0f;
    // The size and scale the header and the switch were last painted at. A paint that failed
    // or was skipped (the pane moved between windows, a draw could not begin) leaves them
    // behind m_bounds; they are painted again at the next chance instead of staying stale.
    SIZE m_painted{};
    float m_paintedScale = 0;
    int m_paintRetries = 0;
    bool PartsStale() const {
        return m_painted.cx != m_bounds.right - m_bounds.left || m_painted.cy != m_bounds.bottom - m_bounds.top ||
               m_paintedScale != m_scale;
    }
    bool m_visible = false, m_embedded = false, m_bare = false;
    Mode m_mode = Mode::Lyrics;
    Spring m_t{0}; // 0 lyrics .. 1 queue
    std::chrono::steady_clock::time_point m_lastFrame{};

    std::wstring m_title, m_artist;
    winrt::com_ptr<IWICBitmapSource> m_art;

    Target m_capture = Target::None; // where the button went down
    Target m_hoverTarget = Target::None;
    int m_buttonDown = -1;
    bool m_barDragging = false; // the thumb is being dragged
    LONG m_barDownX = 0;
    float m_barDownT = 0;
};

} // namespace glasswave::ui
