#pragma once

#include "lyrics/lyrics_service.h"
#include "ui/lyrics_view.h"
#include "ui/queue_view.h"
#include "ui/spring.h"

// The pane on the glass right of the client area, after Apple Music's now-playing sheet:
// the current song's header, then either the lyrics or the queue, switched by a segmented
// control at the bottom (an iOS tab switch: a light thumb slides between Lyrics and Up Next;
// it can be dragged, and the content follows it). Switching cross-fades the two with a short
// rise; the header stays and, in the queue, scrolls along as the queue's song card.

namespace glasswave::ui {

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
    // host shades the background and has its own lyrics / queue buttons).
    void SetEmbedded(bool embedded);
    bool Visible() const { return m_visible; }
    void SetMode(Mode mode);
    Mode CurrentMode() const { return m_mode; }

    void SetTrack(const std::wstring& title, const std::wstring& artist,
                  const winrt::com_ptr<IWICBitmapSource>& art);
    void SetResult(const lyrics::Result& result) { m_lyrics.SetResult(result); }
    void SetPlaying(bool playing, bool paused) { m_lyrics.SetPlaying(playing, paused); }
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
    void PaintHeader();
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
    winrt::Windows::UI::Composition::ContainerVisual m_bar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_track{nullptr}, m_thumb{nullptr};
    // Each segment's icon and label twice: white on the track, dark on the thumb.
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 2> m_segLight{nullptr, nullptr},
        m_segDark{nullptr, nullptr};
    winrt::com_ptr<ID2D1DeviceContext> m_dc;
    LyricsView m_lyrics;
    QueueView m_queue;
    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_visible = false, m_embedded = false;
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
