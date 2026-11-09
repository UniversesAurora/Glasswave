#pragma once

#include "ui/spring.h"

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
    struct Row {
        metadb_handle_ptr handle;
        std::wstring title, artist;
        size_t playlistIndex = SIZE_MAX;  // up-next rows: index in the queue playlist
    };

    explicit QueueView(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~QueueView();
    QueueView(const QueueView&) = delete;
    QueueView& operator=(const QueueView&) = delete;

    void SetBounds(const RECT& px, float scale);  // client px
    void SetVisible(bool visible);
    const winrt::Windows::UI::Composition::ContainerVisual& Root() const { return m_root; }

    void SetUpNext(std::vector<Row> rows);
    void SetHistory(std::vector<Row> rows);
    void SetShuffleRepeat(bool shuffle, int repeat);  // repeat: 0 off, 1 all, 2 one
    void ScrollToCard();                              // the current song's card at the top
    float CardTop() const;                            // DIP from the view's top, as shown now

    bool Tick(float dt);  // springs, inertia; returns whether frames are still needed
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
    std::function<void(const metadb_handle_ptr&)> onPlayHistory;  // a history row
    std::function<void()> onClearHistory, onShuffle, onRepeat, onFrames;
    std::function<void(size_t from, size_t to)> onMove;          // queue playlist indexes
    std::function<void(size_t playlistIndex, POINT screen)> onRowMenu;

private:
    struct RowView;
    enum class Part { None, Row, Handle, Clear, Shuffle, Repeat };
    struct Hit {
        Part part = Part::None;
        int row = -1;  // index into m_rows
    };

    void Layout();  // content positions, springs' targets
    void Render();
    void RenderRow(RowView& row);
    void DrawRow(RowView& row);
    void DrawCover(RowView& row);
    void DrawChrome();  // section titles and capsules
    void ReleaseRow(RowView& row);
    void SetRows(std::vector<Row> rows, bool history);
    Hit HitAt(POINT pt) const;
    float MaxScroll() const;
    float ShownScroll() const;  // with the rubber band applied
    void StartFrames();

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::LayerVisual m_layer{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_maskSurface{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_historyTitle{nullptr}, m_upNextTitle{nullptr}, m_empty{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_shuffle{nullptr}, m_repeat{nullptr};
    winrt::com_ptr<ID2D1DeviceContext> m_dc;
    RECT m_bounds{};
    float m_scale = 1.0f;
    bool m_visible = false;

    std::vector<std::unique_ptr<RowView>> m_rows;  // history rows first, then up next
    size_t m_historyCount = 0;
    bool m_shuffleOn = false;
    int m_repeatMode = 0;
    int m_artToken = 0;
    bool m_coversDirty = false;

    // Content positions (DIP from the content top).
    float m_historyTop = 0, m_cardTop = 0, m_capsulesTop = 0, m_upNextTop = 0, m_contentH = 0;

    // Scrolling: the content offset (DIP); beyond the ends it is shown rubber-banded.
    Spring m_scroll{0};
    bool m_dragging = false, m_pending = false;
    POINT m_downPt{};
    float m_downScroll = 0;
    float m_velocity = 0;  // DIP / s
    std::vector<std::pair<std::chrono::steady_clock::time_point, float>> m_trail;

    // Reordering an up-next row by its handle.
    int m_reorder = -1;     // index into m_rows
    float m_grab = 0;       // DIP between the pointer and the row's top
    float m_reorderY = 0;   // DIP, the dragged row's top
    int m_reorderTo = -1;   // insertion slot among the up-next rows

    Hit m_hover, m_press;
};

}  // namespace glasswave::ui
