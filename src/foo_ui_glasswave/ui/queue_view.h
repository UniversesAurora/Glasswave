#pragma once

#include "core/signal.h"
#include "ui/pane_common.h"
#include "ui/spring.h"
#include "ui/fling.h"

// The queue side of the now-playing pane, after Apple Music's "Playing Next" sheet: one
// scrolling column of History (oldest first, with Clear), the current song's card (the
// pane header, which the pane moves along), the shuffle / repeat capsules and the songs
// up next. It opens with the card at the top; scrolling up reveals the history. Rows move
// on springs (a song that starts slides out, the rest follow), the up-next rows can be
// dragged by their handle to reorder, scrolling has inertia and an iOS rubber band.
// Composition visuals throughout: rows are drawn once, the compositor moves them.

namespace glasswave::ui {

class QueueView {
  public:
    static constexpr size_t kExplicitQueueTag = size_t{1} << (sizeof(size_t) * 8 - 1);
    static bool IsExplicitQueue(size_t token) { return (token & kExplicitQueueTag) != 0; }
    static size_t ExplicitQueueIndex(size_t token) { return token & ~kExplicitQueueTag; }
    struct Row {
        metadb_handle_ptr handle;
        std::wstring title, artist;
        std::vector<std::wstring> badges; // drawn before the artist (stream::Track::badges)
        size_t playlistIndex = SIZE_MAX; // up-next rows: index in the queue playlist
    };

    explicit QueueView(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~QueueView();
    QueueView(const QueueView&) = delete;
    QueueView& operator=(const QueueView&) = delete;

    void SetBounds(const RECT& px, float scale); // client px
    void SetVisible(bool visible);
    const winrt::Windows::UI::Composition::ContainerVisual& Root() const { return m_root; }

    void SetUpNext(std::vector<Row> rows, bool playbackChanged = false);
    void SetHistory(std::vector<Row> rows);
    // repeat: 0 off, 1 all, 2 one. radio: a station plays; shuffle is dimmed and does nothing.
    void SetShuffleRepeat(bool shuffle, int repeat, bool radio);
    // Repeat-all: how many songs the next round replays, shown under the last row (0: none).
    void SetRepeatCycle(size_t songs);
    // Bare: no place for the song card and no shuffle / repeat capsules (the host shows both).
    void SetBare(bool bare);
    void ScrollToCard(bool animate = false);         // the current song's card at the top
    float CardTop() const;                           // DIP from the view's top, as shown now

    bool Tick(float dt); // springs, inertia; returns whether frames are still needed
    bool NeedsFrames() const;

    bool HitTest(POINT pt) const;
    void OnMouseMove(POINT pt);
    void OnMouseLeave();
    void OnLButtonDown(POINT pt);
    void OnLButtonUp(POINT pt);
    void OnCaptureLost();
    void OnWheel(int delta);
    void OnRButtonUp(POINT pt, POINT screen);

    std::function<void(size_t playlistIndex)> onPlay;            // an up-next row
    std::function<void(const metadb_handle_ptr&)> onPlayHistory; // a history row
    std::function<void()> onClearHistory, onShuffle, onRepeat, onFrames;
    // The displayed slots can wrap around the end of the playlist. Items maps each slot
    // to the old playlist index of the song that should occupy it.
    std::function<void(const std::vector<size_t>& slots, const std::vector<size_t>& items)> onReorder;
    std::function<void(size_t playlistIndex, const metadb_handle_ptr& handle, POINT screen)> onRowMenu;
    std::function<void(const metadb_handle_ptr& handle, POINT screen)> onHistoryMenu;

  private:
    struct RowView;
    enum class Part {
        None,
        Row,
        Handle,
        Clear,
        Shuffle,
        Repeat
    };
    struct Hit {
        Part part = Part::None;
        int row = -1; // index into m_rows
    };

    void Layout(); // content positions, springs' targets
    void Render();
    void RenderRow(RowView& row);
    void DrawRow(RowView& row);
    void DrawCover(RowView& row);
    void DrawChrome(); // section titles and capsules
    void ReleaseRow(RowView& row);
    void SetRows(std::vector<Row> rows, bool history, bool playbackChanged = false);
    Hit HitAt(POINT pt) const;
    float MaxScroll() const;
    void KeepScrollInRange();
    void RefreshHover();
    POINT m_pointer{};          // the last pointer position over the queue
    bool m_pointerInside = false;
    float ShownScroll() const; // with the rubber band applied
    void StartFrames();

    // m_viewport is fixed at the pane's content bounds; m_root is the animated child.
    winrt::Windows::UI::Composition::ContainerVisual m_viewport{nullptr}, m_root{nullptr};
    pane::FadedLayer m_layer; // the rows, with the edge fade
    winrt::Windows::UI::Composition::SpriteVisual m_historyTitle{nullptr}, m_upNextTitle{nullptr},
        m_empty{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_shuffle{nullptr}, m_repeat{nullptr}, m_repeatFooter{nullptr};
    size_t m_repeatCycle = 0;
    float m_footerTop = 0;
    // The material under the capsules and the hovered row's plate. Outside m_layer: a layer's
    // content cannot see what lies behind the pane.
    pane::CapsuleGlass m_shuffleGlass, m_repeatGlass;
    pane::PlateGlass m_plateGlass;
    core::Signal<>::Connection m_deviceReplaced;
    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_visible = false;

    std::vector<std::unique_ptr<RowView>> m_rows;         // history rows first, then up next
    std::vector<std::unique_ptr<RowView>> m_retiringRows; // slide and fade before releasing
    size_t m_historyCount = 0;
    bool m_shuffleOn = false;
    int m_repeatMode = 0;
    bool m_radio = false; // a station plays (SetShuffleRepeat)
    int m_artToken = 0;
    bool m_coversDirty = false;

    // Content positions (DIP from the content top).
    float m_historyTop = 0, m_cardTop = 0, m_capsulesTop = 0, m_upNextTop = 0, m_contentH = 0;
    bool m_bare = false;

    // Scrolling: the content offset (DIP); beyond the ends it is shown rubber-banded.
    Spring m_scroll{0};
    bool m_followingPlayback = false; // return to the card without rubber-banding a shrinking list
    bool m_dragging = false, m_pending = false;
    POINT m_downPt{};
    float m_downScroll = 0;
    float m_velocity = 0; // DIP / s
    FlingTracker m_fling;

    // Reordering an up-next row by its handle.
    int m_reorder = -1;   // index into m_rows
    float m_grab = 0;     // DIP between the pointer and the row's top
    float m_reorderY = 0; // DIP, the dragged row's top
    int m_reorderTo = -1; // insertion slot among the up-next rows

    Hit m_hover, m_press;
};

} // namespace glasswave::ui
