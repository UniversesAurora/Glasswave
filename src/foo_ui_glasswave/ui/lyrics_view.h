#pragma once

#include "lyrics/lyrics_service.h"
#include "ui/spring.h"

// The lyrics pane: Apple Music's lyrics view (iOS 17+/macOS 14+) drawn with Direct2D on the
// window's glass. Lines move on springs with a staggered start, lines away from the current
// one are scaled down and blurred, word-timed lines fill word by word with a soft edge and
// each word floats up as it is sung (long words glow), long instrumental gaps show breathing
// dots. Scrolling or dragging takes the lines out of the automatic alignment and sharpens
// them; clicking a line seeks to it.
//
// Tuning (spring constants, blur levels, scale, fade width, timings) follows the behaviour
// documented in docs/design/lyrics-view.md.

namespace glasswave::ui {

class LyricsView {
public:
    explicit LyricsView(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~LyricsView();
    LyricsView(const LyricsView&) = delete;
    LyricsView& operator=(const LyricsView&) = delete;

    void SetBounds(const RECT& px, float scale);
    void SetVisible(bool visible);
    bool Visible() const { return m_visible; }

    // The playing song for the header; `art` may be null.
    void SetTrack(const std::wstring& title, const std::wstring& artist,
                  const winrt::com_ptr<IWICBitmapSource>& art);
    void SetResult(const lyrics::Result& result);
    void SetPlaying(bool playing, bool paused);

    // Advances the animations to now and repaints. Returns whether another frame is needed.
    bool Tick();
    bool NeedsFrames() const;

    // Mouse input in client pixels; the window routes events here while the point is inside.
    bool HitTest(POINT pt) const;
    void OnMouseMove(POINT pt);
    void OnMouseLeave();
    void OnLButtonDown(POINT pt);
    void OnLButtonUp(POINT pt);
    void OnCaptureLost();
    void OnWheel(int delta);
    bool Dragging() const { return m_drag != Drag::None; }

    std::function<void(double seconds)> onSeek;
    std::function<void()> onFrames;  // animation started: the window should call Tick each frame

    struct Run;  // a captured glyph run (public for the run collector)

private:
    struct LineView;
    enum class Drag { None, Pending, Scroll };

    void Relayout();
    void BuildLine(LineView& line, float width);
    void UpdateTargets(bool stagger, bool snap, bool slow);
    void Paint();
    void PaintHeader(ID2D1DeviceContext* dc);
    void PaintLines(ID2D1DeviceContext* dc);
    void PaintLine(ID2D1DeviceContext* dc, LineView& line, double now);
    void PaintInterlude(ID2D1DeviceContext* dc, double now);
    void PaintMessage(ID2D1DeviceContext* dc, const std::wstring& text);
    void EnsureCommandList(ID2D1DeviceContext* dc, LineView& line);
    int LineAt(POINT pt) const;
    double Now() const;  // playback position, seconds
    void StartFrames();
    void EndInteraction();
    float ContentTop() const;     // DIP: top of the lyrics area below the header
    float AnchorY() const;        // DIP: where the focused line's top sits
    float FontSize() const;

    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_surface{nullptr};
    SIZE m_surfaceSize{};
    winrt::com_ptr<ID2D1DeviceContext> m_dc;
    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_visible = false;

    std::wstring m_title, m_artist;
    winrt::com_ptr<IWICBitmapSource> m_art;
    winrt::com_ptr<ID2D1Bitmap> m_artBitmap;  // created lazily on m_dc
    lyrics::Status m_status = lyrics::Status::None;
    std::shared_ptr<const lyrics::Lyrics> m_lyrics;
    std::vector<std::unique_ptr<LineView>> m_lines;
    float m_layoutWidth = 0;
    bool m_playing = false, m_paused = false;

    // Timeline
    int m_focus = -1;        // last line that has started (the alignment target)
    int m_interlude = -1;    // focused interlude: dots after this line (-1 before the first), -2 none
    double m_interludeStart = 0, m_interludeEnd = 0;
    double m_lastNow = 0;
    std::chrono::steady_clock::time_point m_lastFrame{};
    Spring m_interludeY{0};
    float m_interludeShow = 0;

    // Interaction
    int m_hover = -1, m_pressed = -1;
    Drag m_drag = Drag::None;
    POINT m_dragStart{};
    float m_dragOffset0 = 0;
    float m_scroll = 0;        // DIP added to every target while the user browses
    float m_frozenFocal = 0;   // focal offset when browsing began
    bool m_browsing = false;   // the user scrolled: no auto alignment, no blur
    std::chrono::steady_clock::time_point m_interactionEnd{};
    float m_velocity = 0;      // DIP / s, drag inertia
    std::vector<std::pair<std::chrono::steady_clock::time_point, float>> m_dragTrail;
    bool m_hoverInside = false;
};

}  // namespace glasswave::ui
