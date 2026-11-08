#pragma once
#include "wmp/wmp_painter.h"
#include "media/library_model.h"

namespace glasswave::ui {

// Name (untranslated) and WMP icon (wmploc icon group) of a Music category.
std::wstring CategoryLabel(media::GroupKind kind);
int CategoryIcon(media::GroupKind kind);

// Navigation pane: the play queue (Now Playing), All Music (Favorites, the user's playlists
// and the smart playlists), the local Music categories (Artist / Album / Genre) and the
// NetEase Cloud Music library.
class NavPane {
public:
    // Category: a way to browse Local Music; playlist holds its media::GroupKind.
    enum class Target {
        None, Queue, PlaylistsHeader, Playlist, SmartHeader, Category,
        LibraryAll, AllFavorites, LibraryLocal, LibraryNetease,
        NeteaseCreatedHeader, NeteaseCollectedHeader, NeteaseRemotePlaylist,
        NeteaseRecommended, NeteaseLiked, NeteaseArtists, NeteaseAlbums, NeteaseLogin
    };
    struct RemotePlaylist {
        int64_t id = 0;
        std::wstring name;
    };
    struct Item {
        Target target = Target::None;
        size_t playlist = 0;
        int level = 0;
        std::wstring text;
        wmp::paint::NavIcon icon = wmp::paint::NavIcon::None;
        bool expandable = false;
        int64_t remoteId = 0;
    };

    explicit NavPane(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~NavPane();

    void Rebuild();  // playlists changed
    // The NetEase account shown under its library node (logged out: a sign-in entry).
    void SetNeteaseState(bool loggedIn, std::wstring name);
    void SetNeteasePlaylists(std::vector<RemotePlaylist> created, std::vector<RemotePlaylist> collected);
    // The Music categories shown (WMP's "Customize navigation pane"), in order.
    void SetCategories(std::vector<media::GroupKind> kinds);
    const std::vector<media::GroupKind>& Categories() const { return m_categories; }
    void SetBounds(const RECT& px, float scale);
    void Select(Target target, size_t playlist, int64_t remoteId = 0);
    // Selection is blue while the pane has the focus, grey otherwise (Win7 tree view).
    void SetFocused(bool focused);
    const RECT& Bounds() const { return m_bounds; }

    bool HitTest(POINT pt) const { return PtInRect(&m_bounds, pt) != FALSE; }
    // Returns the clicked item (nullptr if none / only toggled a group).
    const Item* OnLButtonDown(POINT pt);
    // Released without dragging: the clicked item. A completed drag is applied through the
    // move callback and returns nullptr.
    const Item* OnLButtonUp(POINT pt);
    void CancelDrag();
    bool Dragging() const { return m_dragging; }
    const Item* ItemAt(POINT pt) const;
    void OnMouseMove(POINT pt);
    void OnMouseLeave();
    void OnWheel(int delta);
    using MoveCallback = std::function<void(const Item& dragged, const Item& target, bool after)>;
    void SetMoveCallback(MoveCallback callback) { m_move = std::move(callback); }
    // Same-level static sections (libraries, local/NetEase subsections) can be reordered;
    // the serialized key order is persisted by the window.
    using OrderCallback = std::function<void(std::wstring)>;
    void SetOrderCallback(OrderCallback callback) { m_order = std::move(callback); }
    void SetStaticOrder(std::wstring_view serialized);

private:
    int RowHeightPx() const;
    void Paint();

    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr};
    std::vector<Item> m_items;
    bool m_allExpanded = true, m_localExpanded = true, m_neteaseExpanded = true;
    bool m_playlistsExpanded = true, m_smartExpanded = true;
    bool m_neteaseLoggedIn = false;
    std::wstring m_neteaseName;
    std::vector<RemotePlaylist> m_createdPlaylists, m_collectedPlaylists;
    bool m_createdExpanded = true, m_collectedExpanded = true;
    Target m_selTarget = Target::LibraryLocal;
    size_t m_selPlaylist = 0;
    int64_t m_selRemoteId = 0;
    size_t m_hot = SIZE_MAX;
    size_t m_hotExpander = SIZE_MAX;  // item whose expander is under the mouse
    bool m_focused = false;
    std::vector<media::GroupKind> m_categories{media::GroupKind::Artist, media::GroupKind::Album, media::GroupKind::Genre};
    size_t m_top = 0;
    int m_artToken = 0;
    MoveCallback m_move;
    OrderCallback m_order;
    std::map<std::wstring, std::vector<std::wstring>> m_staticOrder;
    size_t m_press = SIZE_MAX;
    POINT m_pressPt{};
    POINT m_dragPt{};
    bool m_pressPending = false;
    bool m_dragging = false;
    size_t m_drop = SIZE_MAX;
    bool m_dropAfter = false;
    bool SameSiblingGroup(size_t a, size_t b) const;
    bool MovableItem(size_t index) const;
    static std::wstring ItemKey(const Item& item);
    bool IsStaticItem(size_t index) const;
    std::wstring GroupKeyOf(size_t index) const;
    bool ApplyStaticMove(const Item& dragged, const Item& target, bool after, const std::wstring& group);
    void ApplyStaticOrder();
    std::wstring SerializeOrder() const;
    void UpdateDropTarget(POINT pt);
    RECT m_bounds{};
    float m_scale = 1.0f;
};

}  // namespace glasswave::ui
