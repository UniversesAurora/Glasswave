#pragma once

#include "lyrics/lyrics_service.h"
#include "ui/spring.h"

// The lyrics pane: Apple Music's lyrics view (iOS 17+/macOS 14+) on the window's glass.
// Lines move on springs with a staggered start, lines away from the current one are scaled
// down and blurred, word-timed lines fill word by word with a soft edge and each word floats
// up as it is sung (long words glow), long instrumental gaps show breathing dots. Scrolling
// or dragging takes the lines out of the automatic alignment and sharpens them; clicking a
// line seeks to it.
//
// Rendering: every line is a composition visual with its own surface, drawn once with
// Direct2D; the compositor moves, scales, fades and blurs it on the GPU (blur through an
// effect brush, the edge fade through a layer mask). Per frame the UI thread only updates
// visual properties and redraws the line whose words are filling.
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

    // Advances the animations to now and updates the visuals. Returns whether another frame
    // is needed.
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
    void Render();                       // per frame: visual properties, live line surfaces
    void RenderLine(LineView& line, double now, float areaH);
    void DrawLineImage(LineView& line, double now, bool live);
    void DrawLiveText(ID2D1DeviceContext* dc, LineView& line, double now);
    void PrepareGlow(LineView& line);  // command lists for emphasized words (outside a draw)
    void ReleaseLine(LineView& line);  // drops its visuals and surface (far off screen)
    void RenderInterlude(double now);
    void PaintHeader();
    void PaintMessage();
    void LayoutChrome();  // tint, header, message, lyrics layer and its fade mask
    int LineAt(POINT pt) const;
    double Now() const;  // playback position, seconds
    void StartFrames();
    float ContentTop() const;  // DIP: top of the lyrics area below the header
    float AnchorY() const;     // DIP: where the focused line's top sits
    float FontSize() const;
    float ImagePad() const;    // DIP around a line's text in its surface (blur, float, glow)

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_tint{nullptr}, m_header{nullptr}, m_message{nullptr};
    winrt::Windows::UI::Composition::LayerVisual m_layer{nullptr};  // the lines, with the edge fade
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_maskSurface{nullptr};
    winrt::Windows::UI::Composition::CompositionEffectFactory m_blurFactory{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_dots{nullptr};
    std::array<winrt::Windows::UI::Composition::CompositionColorBrush, 3> m_dotBrush{nullptr, nullptr, nullptr};
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
    float m_typicalLine = 0;  // DIP, median line block height (alignment)
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
    float m_dim = 0.24f;       // current inactive alpha (eases to the browsing level and back)
    std::chrono::steady_clock::time_point m_interactionEnd{};
    float m_velocity = 0;      // DIP / s, drag inertia
    std::vector<std::pair<std::chrono::steady_clock::time_point, float>> m_dragTrail;
    bool m_hoverInside = false;
};

}  // namespace glasswave::ui
