#pragma once
#include "core/signal.h"
#include "wmp/wmp_painter.h"
#include "media/library_model.h"
#include "stream/provider.h"

#include <map>
#include <set>

namespace glasswave::ui {

// Name (untranslated) and WMP icon (wmploc icon group) of a Music category.
std::wstring CategoryLabel(media::GroupKind kind);
int CategoryIcon(media::GroupKind kind);

// Navigation pane: the play queue (Now Playing), All Music (Favorites, the cross-source
// Recently Played / Most Played lists and the user's playlists), Local Music (the Smart
// playlists section, its categories and the CD drives), one section per streaming provider.
class NavPane {
public:
    // Category: a way to browse Local Music; playlist holds its media::GroupKind.
    enum class Target {
        None, Home, Queue, PlaylistsHeader, Playlist, SmartHeader, Category,
        LibraryAll, AllFavorites, AllRecentPlayed, AllMostPlayed, AllDownloads, LibraryLocal,
        // a provider's section (Item::provider): its root, pages, playlists and account row
        LibraryRemote, RemoteSection, RemotePlaylist,
        // Local Music's Discs group, and a CD drive in it (Item::playlist: the drive letter)
        DiscsHeader, AudioCd
    };
    struct DiscRow {
        wchar_t drive = 0;
        std::wstring title;
    };
    // What the pane shows for a provider.
    struct ProviderSection {
        stream::Provider* provider = nullptr;
        bool signedIn = false;
        std::vector<stream::NavigationSection> sections;
        std::map<stream::Id, std::vector<stream::Playlist>> playlists;
    };
    struct Item {
        Target target = Target::None;
        size_t playlist = 0;
        int level = 0;
        std::wstring text;
        wmp::paint::NavIcon icon = wmp::paint::NavIcon::None;
        bool expandable = false;
        stream::Provider* provider = nullptr;
        stream::Id remoteId;        // RemotePlaylist
        stream::Id sectionId;       // RemoteSection / RemotePlaylist parent
        stream::SectionKind sectionKind = stream::SectionKind::Tracks;
        stream::GroupFacet sectionFacet = stream::GroupFacet::Genre;
        stream::Shelf sectionShelf = stream::Shelf::Created;
        bool reorderChildren = false;
    };

    explicit NavPane(const winrt::Windows::UI::Composition::ContainerVisual& parent);
    ~NavPane();

    void Rebuild();  // playlists changed
    // The streaming providers' sections, in registration order.
    void SetProviders(std::vector<ProviderSection> sections);
    // The CD drives (with their discs' names), under Local Music > Discs.
    void SetDiscs(std::vector<DiscRow> discs);
    // The Music categories shown (WMP's "Customize navigation pane"), in order.
    void SetCategories(std::vector<media::GroupKind> kinds);
    const std::vector<media::GroupKind>& Categories() const { return m_categories; }
    // Where the Smart playlists section sits among the categories: 0 = above all of them,
    // Categories().size() = below all of them.
    void SetSmartPosition(int position);
    int SmartPosition() const { return m_smartAt; }
    void SetBounds(const RECT& px, float scale);
    void Select(Target target, size_t playlist, const stream::Provider* provider = nullptr,
                const stream::Id& remoteId = {});
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
    core::Signal<>::Connection m_deviceReplaced; // its surface is empty after a device loss
    std::vector<Item> m_items;
    std::set<std::wstring> m_collapsed;  // ExpandKey of the groups the user closed
    std::vector<ProviderSection> m_providers;
    std::vector<DiscRow> m_discs;
    Target m_selTarget = Target::LibraryLocal;
    size_t m_selPlaylist = 0;
    const stream::Provider* m_selProvider = nullptr;
    stream::Id m_selRemoteId;
    size_t m_hot = SIZE_MAX;
    size_t m_hotExpander = SIZE_MAX;  // item whose expander is under the mouse
    bool m_focused = false;
    std::vector<media::GroupKind> m_categories{media::GroupKind::Artist, media::GroupKind::Album, media::GroupKind::Genre};
    int m_smartAt = 0;
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
    static std::wstring ExpandKey(const Item& item);
    bool Expanded(const Item& item) const { return !m_collapsed.count(ExpandKey(item)); }
    bool IsProviderGroup(const std::wstring& groupKey) const;
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
