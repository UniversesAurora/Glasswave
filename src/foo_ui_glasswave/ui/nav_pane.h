#pragma once
#include "wmp/wmp_painter.h"

namespace glasswave::ui {

// Navigation pane: the play queue (Now Playing), the user's playlists, smart playlists
// (Favorites and foobar2000 auto playlists) and Music (Artist / Album / Genre).
class NavPane {
public:
    enum class Target { None, Queue, PlaylistsHeader, Playlist, SmartHeader, Music, Artists, Albums, Genres };
    struct Item {
        Target target = Target::None;
        size_t playlist = 0;
        int level = 0;
        std::wstring text;
        wmp::paint::NavIcon icon = wmp::paint::NavIcon::None;
        bool expandable = false;
    };

    explicit NavPane(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~NavPane();

    void Rebuild();  // playlists changed
    void SetBounds(const RECT& px, float scale);
    void Select(Target target, size_t playlist);
    const RECT& Bounds() const { return m_bounds; }

    bool HitTest(POINT pt) const { return PtInRect(&m_bounds, pt) != FALSE; }
    // Returns the clicked item (nullptr if none / only toggled a group).
    const Item* OnLButtonDown(POINT pt);
    const Item* ItemAt(POINT pt) const;
    void OnMouseMove(POINT pt);
    void OnMouseLeave();
    void OnWheel(int delta);

private:
    int RowHeightPx() const;
    void Paint();

    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr};
    std::vector<Item> m_items;
    bool m_playlistsExpanded = true, m_smartExpanded = true, m_musicExpanded = true;
    Target m_selTarget = Target::Music;
    size_t m_selPlaylist = 0;
    size_t m_hot = SIZE_MAX;
    size_t m_top = 0;
    RECT m_bounds{};
    float m_scale = 1.0f;
};

}  // namespace glasswave::ui
