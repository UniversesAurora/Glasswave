#include "pch.h"
#include "ui/nav_pane.h"
#include "gfx/composition_host.h"
#include "netease/catalog.h"
#include "ui/art_cache.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "media/favorites.h"
#include "ui/i18n.h"
#include "media/special_playlists.h"

namespace glasswave::ui {

namespace wp = wmp::paint;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {
constexpr float kRowHeight = 22.0f;
constexpr float kIndent = 16.0f;

// WMP 12 navigation icons: wmploc icon groups (see docs/design/win7-asset-map.md).
int WmpIconId(wmp::paint::NavIcon icon, bool autoPlaylist) {
    using I = wmp::paint::NavIcon;
    switch (icon) {
    case I::Library: return 29527;
    case I::Playlist: return autoPlaylist ? 29572 : 29522;
    case I::NowPlaying: return 29522;
    case I::Music: return 29511;
    case I::Artist: return 29514;
    case I::Album: return 29510;
    case I::Genre: return 29517;
    default: return 0;
    }
}

// Icon as a WIC bitmap at `px` pixels, cached; nullptr when the art is missing.
std::map<std::pair<int, int>, winrt::com_ptr<IWICBitmapSource>>& IconCache() {
    static std::map<std::pair<int, int>, winrt::com_ptr<IWICBitmapSource>> cache;
    return cache;
}

IWICBitmapSource* WmpIcon(int id, int px) {
    auto& cache = IconCache();
    const auto key = std::make_pair(id, px);
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    winrt::com_ptr<IWICBitmapSource> result;
    if (HICON h = win7::Resources::Get().Icon(win7::Module::Wmploc, id, px)) {
        auto* wic = gfx::CompositionHost::Get().Wic();
        winrt::com_ptr<IWICBitmap> bmp;
        winrt::com_ptr<IWICFormatConverter> conv;
        if (SUCCEEDED(wic->CreateBitmapFromHICON(h, bmp.put())) && SUCCEEDED(wic->CreateFormatConverter(conv.put())) &&
            SUCCEEDED(conv->Initialize(bmp.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeMedianCut)))
            result = conv;
        DestroyIcon(h);
    }
    cache[key] = result;
    return result.get();
}

bool DrawWmpIcon(ID2D1DeviceContext* dc, D2D1_POINT_2F center, wmp::paint::NavIcon icon, bool autoPlaylist, float scale,
                 int overrideId = 0) {
    const int id = overrideId ? overrideId : WmpIconId(icon, autoPlaylist);
    if (!id) return false;
    const int px = static_cast<int>(std::lround(16 * scale));
    IWICBitmapSource* src = WmpIcon(id, px);
    if (!src) return false;
    winrt::com_ptr<ID2D1Bitmap> bmp;
    if (FAILED(dc->CreateBitmapFromWicBitmap(src, nullptr, bmp.put()))) return false;
    const float x = std::round((center.x - 8) * scale) / scale, y = std::round((center.y - 8) * scale) / scale;
    dc->DrawBitmap(bmp.get(), D2D1::RectF(x, y, x + px / scale, y + px / scale), 1.0f,
                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return true;
}
}  // namespace

// Label and WMP icon (wmploc icon group) of each Music category.
std::wstring CategoryLabel(media::GroupKind kind) {
    static const wchar_t* const kLabels[] = {L"Artist", L"Album",  L"Genre", L"Year", L"Rating", L"Contributing artist",
                                             L"Composer", L"Folder"};
    return kLabels[std::min(static_cast<size_t>(kind), std::size(kLabels) - 1)];
}

int CategoryIcon(media::GroupKind kind) {
    static const int kIcons[] = {29514, 29510, 29517, 29530, 29526, 29537, 29515, 29519};
    return kIcons[std::min(static_cast<size_t>(kind), std::size(kIcons) - 1)];
}

NavPane::NavPane(const winrt::Windows::UI::Composition::ContainerVisual& parent) {
    m_visual = gfx::CompositionHost::Get().Compositor().CreateSpriteVisual();
    parent.Children().InsertAtTop(m_visual);
    Rebuild();
    m_artToken = ArtCache::Get().Subscribe([this] { Paint(); });
}

NavPane::~NavPane() {
    ArtCache::Get().Unsubscribe(m_artToken);
    IconCache().clear();  // WIC objects go before COM does
}

int NavPane::RowHeightPx() const { return static_cast<int>(std::lround(kRowHeight * m_scale)); }

std::wstring NavPane::ItemKey(const Item& item) {
    switch (item.target) {
    case Target::Queue: return L"queue";
    case Target::LibraryAll: return L"all";
    case Target::LibraryLocal: return L"local";
    case Target::LibraryNetease: return L"netease";
    case Target::AllFavorites: return L"favorites";
    case Target::AllRecentPlayed: return L"recent";
    case Target::AllMostPlayed: return L"mostplayed";
    case Target::PlaylistsHeader: return L"playlists";
    case Target::SmartHeader: return L"smart";
    case Target::NeteaseLiked: return L"liked";
    case Target::NeteaseArtists: return L"artists";
    case Target::NeteaseAlbums: return L"albums";
    case Target::NeteaseCreatedHeader: return L"created";
    case Target::NeteaseCollectedHeader: return L"collected";
    case Target::NeteaseRecommended: return L"recommended";
    case Target::NeteaseLogin: return L"account";
    default: return {};
    }
}

std::wstring NavPane::GroupKeyOf(size_t index) const {
    if (index >= m_items.size()) return {};
    const Item& item = m_items[index];
    if (item.level == 0) return L"root";
    for (size_t i = index; i > 0; --i)
        if (m_items[i - 1].level < item.level) return ItemKey(m_items[i - 1]);
    return {};
}

bool NavPane::IsStaticItem(size_t index) const {
    return index < m_items.size() && m_items[index].level <= 1 && !ItemKey(m_items[index]).empty();
}

void NavPane::SetStaticOrder(std::wstring_view serialized) {
    m_staticOrder.clear();
    size_t start = 0;
    while (start <= serialized.size()) {
        const size_t end = serialized.find(L';', start);
        const std::wstring part(serialized.substr(
            start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
        const size_t eq = part.find(L'=');
        if (eq != std::wstring::npos && eq > 0) {
            std::vector<std::wstring> keys;
            size_t pos = eq + 1;
            while (pos <= part.size()) {
                const size_t comma = part.find(L',', pos);
                std::wstring key = part.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
                if (!key.empty()) keys.push_back(std::move(key));
                if (comma == std::wstring::npos) break;
                pos = comma + 1;
            }
            m_staticOrder[part.substr(0, eq)] = std::move(keys);
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    Rebuild();
}

std::wstring NavPane::SerializeOrder() const {
    std::wstring out;
    for (const auto& [group, keys] : m_staticOrder) {
        if (keys.empty()) continue;
        if (!out.empty()) out += L';';
        out += group;
        out += L'=';
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) out += L',';
            out += keys[i];
        }
    }
    return out;
}

void NavPane::ApplyStaticOrder() {
    if (m_items.empty()) return;
    struct Block {
        std::wstring key;
        size_t original = 0;
        std::vector<Item> items;
    };
    auto sortBlocks = [](std::vector<Block>& blocks, const std::vector<std::wstring>& order) {
        auto rank = [&](const Block& block) {
            auto it = std::find(order.begin(), order.end(), block.key);
            return it == order.end() ? order.size() + block.original
                                     : static_cast<size_t>(it - order.begin());
        };
        std::stable_sort(blocks.begin(), blocks.end(),
                         [&](const Block& a, const Block& b) { return rank(a) < rank(b); });
    };
    std::vector<Block> roots;
    for (auto& item : m_items) {
        if (item.level == 0) roots.push_back({ItemKey(item), roots.size(), {}});
        if (!roots.empty()) roots.back().items.push_back(std::move(item));
    }
    auto rootOrder = m_staticOrder.find(L"root");
    if (rootOrder != m_staticOrder.end()) sortBlocks(roots, rootOrder->second);
    std::vector<Item> ordered;
    ordered.reserve(m_items.size());
    for (auto& root : roots) {
        if (root.items.empty()) continue;
        ordered.push_back(std::move(root.items.front()));
        const std::wstring rootKey = root.key;
        std::vector<Block> children;
        for (size_t i = 1; i < root.items.size(); ++i) {
            Item& item = root.items[i];
            if (item.level == 1) children.push_back({ItemKey(item), children.size(), {}});
            if (!children.empty()) children.back().items.push_back(std::move(item));
        }
        // Local's children follow the category order and SmartPosition; never re-sort them here.
        auto childOrder = rootKey == L"local" ? m_staticOrder.end() : m_staticOrder.find(rootKey);
        if (childOrder != m_staticOrder.end()) sortBlocks(children, childOrder->second);
        for (auto& child : children)
            for (auto& item : child.items) ordered.push_back(std::move(item));
    }
    m_items = std::move(ordered);
}

bool NavPane::ApplyStaticMove(const Item& dragged, const Item& target, bool after,
                              const std::wstring& group) {
    const std::wstring from = ItemKey(dragged);
    const std::wstring to = ItemKey(target);
    if (from.empty() || to.empty() || from == to) return false;
    auto& order = m_staticOrder[group];
    // Take the current visible order of any sibling not in the stored one yet: a group can
    // gain members (the playlists moved under All Music) after its order was first saved.
    for (size_t i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items[i];
        const bool inGroup = group == L"root" ? item.level == 0
                                              : item.level == 1 && GroupKeyOf(i) == group;
        if (!inGroup) continue;
        const std::wstring key = ItemKey(item);
        if (!key.empty() && std::find(order.begin(), order.end(), key) == order.end())
            order.push_back(key);
    }
    auto fromIt = std::find(order.begin(), order.end(), from);
    if (fromIt == order.end()) return false;
    order.erase(fromIt);
    auto toIt = std::find(order.begin(), order.end(), to);
    if (toIt == order.end()) return false;
    const size_t index = static_cast<size_t>(toIt - order.begin()) + (after ? 1 : 0);
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(index), from);
    if (m_order) m_order(SerializeOrder());
    Rebuild();
    return true;
}

void NavPane::Rebuild() {
    m_items.clear();
    auto pm = playlist_manager::get();
    // The play queue on top, like the "Up Next" / "Playing" list of current players.
    m_items.push_back({Target::Queue, media::QueuePlaylist(false), 0, L"Now Playing", wp::NavIcon::NowPlaying, false});
    // Playlists hold local and NetEase tracks alike, so they belong to All Music; the NetEase
    // library shows its own cloud playlists below.
    m_items.push_back({Target::LibraryAll, 0, 0, L"All Music", wp::NavIcon::Library, true});
    if (m_allExpanded) {
        m_items.push_back({Target::AllFavorites, 0, 1, L"Favorites", wp::NavIcon::Playlist, false});
        m_items.push_back({Target::AllRecentPlayed, 0, 1, L"Recently Played", wp::NavIcon::Playlist, false});
        m_items.push_back({Target::AllMostPlayed, 0, 1, L"Most Played", wp::NavIcon::Playlist, false});
        m_items.push_back({Target::PlaylistsHeader, 0, 1, L"Playlists", wp::NavIcon::Playlist, true});
        if (m_playlistsExpanded) {
            pfc::string8 name;
            for (t_size i = 0, n = pm->get_playlist_count(); i < n; ++i) {
                if (media::IsSpecial(i)) continue;  // queue, Favorites and smart lists have their places
                pm->playlist_get_name(i, name);
                m_items.push_back({Target::Playlist, i, 2, Wide(name), wp::NavIcon::Playlist, false});
            }
        }
    }
    // Local Music: the Smart playlists section and the categories directly, without the old
    // Music level. The section sits at m_smartAt among the categories and can be dragged.
    m_items.push_back({Target::LibraryLocal, 0, 0, L"Local Music", wp::NavIcon::Music, true});
    if (m_localExpanded) {
        const int count = static_cast<int>(m_categories.size());
        const int at = std::clamp(m_smartAt, 0, count);
        for (int i = 0; i <= count; ++i) {
            if (i == at) {
                m_items.push_back({Target::SmartHeader, 0, 1, L"Smart playlists", wp::NavIcon::Playlist, true});
                if (m_smartExpanded) {
                    if (const t_size fav = media::FavoritesPlaylist(false); fav != SIZE_MAX)
                        m_items.push_back({Target::Playlist, fav, 2, L"Favorites", wp::NavIcon::Playlist, false});
                    pfc::string8 smartName;  // every auto playlist, presets and the user's own
                    for (t_size p = 0, n = pm->get_playlist_count(); p < n; ++p)
                        if (media::IsAutoPlaylist(p)) {
                            pm->playlist_get_name(p, smartName);
                            m_items.push_back({Target::Playlist, p, 2, Wide(smartName), wp::NavIcon::Playlist, false});
                        }
                }
            }
            if (i < count) {
                const auto kind = m_categories[static_cast<size_t>(i)];
                m_items.push_back(
                    {Target::Category, static_cast<size_t>(kind), 1, CategoryLabel(kind), wp::NavIcon::Artist, false});
            }
        }
    }
    m_items.push_back({Target::LibraryNetease, 0, 0, L"NetEase Cloud Music", wp::NavIcon::Library, true});
    if (m_neteaseExpanded) {
        if (m_neteaseLoggedIn) {
            m_items.push_back({Target::NeteaseLiked, 0, 1, L"Favorites", wp::NavIcon::Playlist, false});
            m_items.push_back({Target::NeteaseArtists, 0, 1, L"Artists", wp::NavIcon::Artist, false});
            m_items.push_back({Target::NeteaseAlbums, 0, 1, L"Albums", wp::NavIcon::Album, false});
            m_items.push_back(
                {Target::NeteaseCreatedHeader, 0, 1, L"Created playlists", wp::NavIcon::Playlist, true});
            if (m_createdExpanded) {
                for (const auto& playlist : m_createdPlaylists)
                    m_items.push_back({Target::NeteaseRemotePlaylist, 0, 2,
                                       playlist.name.empty() ? L"Unnamed playlist" : playlist.name,
                                       wp::NavIcon::Playlist, false, playlist.id});
            }
            m_items.push_back(
                {Target::NeteaseCollectedHeader, 0, 1, L"Collected playlists", wp::NavIcon::Playlist, true});
            if (m_collectedExpanded) {
                for (const auto& playlist : m_collectedPlaylists)
                    m_items.push_back({Target::NeteaseRemotePlaylist, 0, 2,
                                       playlist.name.empty() ? L"Unnamed playlist" : playlist.name,
                                       wp::NavIcon::Playlist, false, playlist.id});
            }
            m_items.push_back({Target::NeteaseRecommended, 0, 1, L"Recommended", wp::NavIcon::Playlist, false});
        }
        const std::wstring account = m_neteaseLoggedIn
                                         ? (m_neteaseName.empty() ? std::wstring(L"NetEase account") : m_neteaseName)
                                         : std::wstring(L"Sign in to NetEase Cloud Music...");
        m_items.push_back({Target::NeteaseLogin, 0, 1, account, wp::NavIcon::Playlist, false});
    }
    ApplyStaticOrder();
    m_hot = SIZE_MAX;
    Paint();
}

void NavPane::SetNeteasePlaylists(std::vector<RemotePlaylist> created, std::vector<RemotePlaylist> collected) {
    m_createdPlaylists = std::move(created);
    m_collectedPlaylists = std::move(collected);
    Rebuild();
}

void NavPane::SetNeteaseState(bool loggedIn, std::wstring name) {
    if (m_neteaseLoggedIn == loggedIn && m_neteaseName == name) return;
    m_neteaseLoggedIn = loggedIn;
    m_neteaseName = std::move(name);
    Rebuild();
}

void NavPane::SetBounds(const RECT& px, float scale) {
    const bool changed = (px.right - px.left) != (m_bounds.right - m_bounds.left) ||
                         (px.bottom - px.top) != (m_bounds.bottom - m_bounds.top) || scale != m_scale;
    m_bounds = px;
    m_scale = scale;
    m_visual.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    if (changed) Paint();
}

void NavPane::SetCategories(std::vector<media::GroupKind> kinds) {
    m_categories = std::move(kinds);
    m_smartAt = std::clamp(m_smartAt, 0, static_cast<int>(m_categories.size()));
    Rebuild();
}

void NavPane::SetSmartPosition(int position) {
    position = std::clamp(position, 0, static_cast<int>(m_categories.size()));
    if (position == m_smartAt) return;
    m_smartAt = position;
    m_staticOrder.erase(L"local");  // the categories and this position define the order now
    Rebuild();
}

void NavPane::SetFocused(bool focused) {
    if (focused == m_focused) return;
    m_focused = focused;
    Paint();
}

void NavPane::Select(Target target, size_t playlist, int64_t remoteId) {
    m_selTarget = target;
    m_selPlaylist = playlist;
    m_selRemoteId = remoteId;
    Paint();
}

const NavPane::Item* NavPane::ItemAt(POINT pt) const {
    if (!HitTest(pt)) return nullptr;
    const size_t i = m_top + static_cast<size_t>((pt.y - m_bounds.top - std::lround(6 * m_scale)) /
                                                 std::max(RowHeightPx(), 1));
    return (pt.y - m_bounds.top) >= std::lround(6 * m_scale) && i < m_items.size() ? &m_items[i] : nullptr;
}

const NavPane::Item* NavPane::OnLButtonDown(POINT pt) {
    m_press = SIZE_MAX;
    m_pressPending = false;
    m_dragging = false;
    m_drop = SIZE_MAX;
    m_dropAfter = false;
    const Item* item = ItemAt(pt);
    if (!item) return nullptr;
    // Clicking the disclosure triangle toggles; clicking the label also selects.
    const float x = (pt.x - m_bounds.left) / m_scale;
    if (item->expandable && x < 16 + item->level * kIndent) {
        if (item->target == Target::PlaylistsHeader) m_playlistsExpanded = !m_playlistsExpanded;
        if (item->target == Target::SmartHeader) m_smartExpanded = !m_smartExpanded;
        if (item->target == Target::LibraryAll) m_allExpanded = !m_allExpanded;
        if (item->target == Target::LibraryLocal) m_localExpanded = !m_localExpanded;
        if (item->target == Target::LibraryNetease) m_neteaseExpanded = !m_neteaseExpanded;
        if (item->target == Target::NeteaseCreatedHeader) m_createdExpanded = !m_createdExpanded;
        if (item->target == Target::NeteaseCollectedHeader) m_collectedExpanded = !m_collectedExpanded;
        Rebuild();
        return nullptr;
    }
    m_press = static_cast<size_t>(item - m_items.data());
    m_pressPt = pt;
    m_pressPending = true;
    return item;
}

const NavPane::Item* NavPane::OnLButtonUp(POINT pt) {
    (void)pt;
    const Item* result = nullptr;
    if (m_dragging) {
        if (m_drop < m_items.size() && m_drop != m_press && m_press < m_items.size()) {
            const Item from = m_items[m_press];
            const Item to = m_items[m_drop];
            const bool after = m_dropAfter;
            const bool staticMove = IsStaticItem(m_press) && IsStaticItem(m_drop);
            const std::wstring group = staticMove
                                           ? (from.level == 0 ? std::wstring(L"root") : GroupKeyOf(m_press))
                                           : std::wstring();
            m_press = SIZE_MAX;
            m_pressPending = false;
            m_dragging = false;
            m_drop = SIZE_MAX;
            m_dropAfter = false;
            Paint();
            if (staticMove) ApplyStaticMove(from, to, after, group);
            else if (m_move) m_move(from, to, after);
            return nullptr;
        }
    } else if (m_pressPending && m_press < m_items.size()) {
        result = &m_items[m_press];
    }
    m_press = SIZE_MAX;
    m_pressPending = false;
    m_dragging = false;
    m_drop = SIZE_MAX;
    m_dropAfter = false;
    Paint();
    return result;
}

void NavPane::CancelDrag() {
    if (!m_pressPending && !m_dragging) return;
    m_press = SIZE_MAX;
    m_pressPending = false;
    m_dragging = false;
    m_drop = SIZE_MAX;
    m_dropAfter = false;
    Paint();
}

bool NavPane::MovableItem(size_t index) const {
    if (index >= m_items.size()) return false;
    const Item& item = m_items[index];
    if (item.level == 0) return !ItemKey(item).empty();
    if (item.level == 1) {
        const std::wstring group = GroupKeyOf(index);
        return group == L"all" || group == L"local" || group == L"netease";
    }
    if (item.level != 2) return false;
    for (size_t i = index; i > 0; --i) {
        if (m_items[i - 1].level >= 2) continue;
        const Target parent = m_items[i - 1].target;
        return parent == Target::NeteaseCreatedHeader || parent == Target::NeteaseCollectedHeader ||
               parent == Target::PlaylistsHeader || parent == Target::SmartHeader;
    }
    return false;
}

bool NavPane::SameSiblingGroup(size_t a, size_t b) const {
    if (a >= m_items.size() || b >= m_items.size() || a == b) return false;
    const Item& first = m_items[a];
    const Item& second = m_items[b];
    if (first.level == 0 && second.level == 0) return true;
    if (first.level == 1 && second.level == 1) {
        auto parentOf = [this](size_t index) {
            for (size_t i = index; i > 0; --i)
                if (m_items[i - 1].level == 0) return i - 1;
            return SIZE_MAX;
        };
        const size_t pa = parentOf(a);
        const size_t pb = parentOf(b);
        if (pa == SIZE_MAX || pb == SIZE_MAX || pa != pb) return false;
        // Libraries with several subsections: All Music, Local Music and NetEase.
        const std::wstring group = ItemKey(m_items[pa]);
        if (group == L"local")  // the Smart playlists section interleaves with the categories
            return (first.target == Target::Category || first.target == Target::SmartHeader) &&
                   (second.target == Target::Category || second.target == Target::SmartHeader);
        return group == L"all" || group == L"netease";
    }
    if (first.level != 2 || second.level != 2) return false;
    auto parentOf = [this](size_t index) {
        for (size_t i = index; i > 0; --i)
            if (m_items[i - 1].level < 2) return i - 1;
        return SIZE_MAX;
    };
    const size_t pa = parentOf(a);
    const size_t pb = parentOf(b);
    if (pa == SIZE_MAX || pb == SIZE_MAX || m_items[pa].target != m_items[pb].target) return false;
    // Groups whose order can be persisted: the two NetEase playlist sections, the regular
    // playlists under All Music and the smart playlists under Local Music.
    const Target parent = m_items[pa].target;
    return parent == Target::NeteaseCreatedHeader || parent == Target::NeteaseCollectedHeader ||
           parent == Target::PlaylistsHeader || parent == Target::SmartHeader;
}

void NavPane::UpdateDropTarget(POINT pt) {
    const Item* item = ItemAt(pt);
    const size_t drop = item ? static_cast<size_t>(item - m_items.data()) : SIZE_MAX;
    if (drop == SIZE_MAX || !SameSiblingGroup(m_press, drop)) {
        if (m_drop != SIZE_MAX) {
            m_drop = SIZE_MAX;
            Paint();
        }
        return;
    }
    const int rowH = RowHeightPx();
    const int top = m_bounds.top + std::lround(6 * m_scale) + static_cast<int>(drop - m_top) * rowH;
    const bool after = pt.y >= top + rowH / 2;
    if (m_drop != drop || m_dropAfter != after) {
        m_drop = drop;
        m_dropAfter = after;
        Paint();
    }
}

void NavPane::OnMouseMove(POINT pt) {
    if (m_pressPending) {
        if (!m_dragging) {
            const int moved = std::abs(pt.x - m_pressPt.x) + std::abs(pt.y - m_pressPt.y);
            if (moved > std::lround(4 * m_scale) && MovableItem(m_press)) {
                m_dragging = true;
                m_dragPt = pt;
            }
        }
        if (m_dragging) {
            m_dragPt = pt;
            const int rowH = RowHeightPx();
            const int header = std::lround(6 * m_scale);
            const size_t visible =
                std::max<size_t>(1, (m_bounds.bottom - m_bounds.top - header) / std::max(rowH, 1));
            if (pt.y < m_bounds.top + header + rowH && m_top > 0) {
                --m_top;
            } else if (pt.y > m_bounds.bottom - rowH && m_top + visible < m_items.size()) {
                ++m_top;
            }
            UpdateDropTarget(pt);
            Paint();
            return;
        }
    }
    const Item* item = ItemAt(pt);
    const size_t hot = item ? static_cast<size_t>(item - m_items.data()) : SIZE_MAX;
    // The expander lights up on its own when the mouse is over it.
    const float x = (pt.x - m_bounds.left) / m_scale;
    const size_t expander = item && item->expandable && x < 16 + item->level * kIndent ? hot : SIZE_MAX;
    if (hot != m_hot || expander != m_hotExpander) {
        m_hot = hot;
        m_hotExpander = expander;
        Paint();
    }
}

void NavPane::OnMouseLeave() {
    if (m_hot != SIZE_MAX) {
        m_hot = SIZE_MAX;
        m_hotExpander = SIZE_MAX;
        Paint();
    }
}

void NavPane::OnWheel(int delta) {
    const size_t visible = static_cast<size_t>((m_bounds.bottom - m_bounds.top) / std::max(RowHeightPx(), 1));
    const size_t maxTop = m_items.size() > visible ? m_items.size() - visible : 0;
    const int lines = 3 * (-delta / WHEEL_DELTA);
    m_top = lines < 0 ? (m_top > static_cast<size_t>(-lines) ? m_top + lines : 0) : std::min(m_top + lines, maxTop);
    Paint();
}

void NavPane::Paint() {
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = std::max<int>(m_bounds.bottom - m_bounds.top, 1);
    const float s = m_scale;
    host.Repaint(m_visual, w, h, s, [&](ID2D1DeviceContext* dc) {
        const float width = w / s;
        const float rowH = RowHeightPx() / s;
        winrt::com_ptr<ID2D1SolidColorBrush> ink, sep;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x1E395B), ink.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0xD5DFE9), sep.put());
        // Divider against the list
        const float L = std::max(1.0f, std::floor(s)) / s;
        dc->FillRectangle({width - L, 0, width, h / s}, sep.get());
        auto drawRowContent = [&](const Item& it, float x, float y) {
            const bool autoPlaylist = it.target == Target::Playlist && media::IsAutoPlaylist(it.playlist);
            // Favorites: star; queue: WMP's playlist icon with the orbit; smart header: auto playlist.
            const int special = (it.target == Target::Playlist && it.text == L"Favorites") ||
                                        it.target == Target::AllFavorites || it.target == Target::NeteaseLiked
                                    ? 29526
                                : it.target == Target::Queue        ? 29513
                                : it.target == Target::AllRecentPlayed ? 575   // clock with a music note
                                : it.target == Target::AllMostPlayed   ? 550   // pie chart
                                : it.target == Target::SmartHeader     ? 29572
                                : it.target == Target::Category ? CategoryIcon(static_cast<media::GroupKind>(it.playlist))
                                                                : 0;
            bool drewCover = false;
            if (it.target == Target::NeteaseRemotePlaylist && it.remoteId) {
                const auto track = netease::Catalog::Get().PlaylistHandle(it.remoteId);
                const std::wstring key = L"ncm:playlist:" + std::to_wstring(it.remoteId);
                winrt::com_ptr<IWICBitmapSource> cover;
                const int coverPx = static_cast<int>(std::lround(16 * m_scale));
                if (ArtCache::Get().Find(key, track, coverPx, &cover) == ArtCache::State::Ready && cover) {
                    winrt::com_ptr<ID2D1Bitmap> bitmap;
                    if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(cover.get(), nullptr, bitmap.put()))) {
                        const float side = 16.0f;
                        const float ix = std::round((x + 8) * s) / s;
                        const float iy = std::round((y + (rowH - side) / 2) * s) / s;
                        dc->DrawBitmap(bitmap.get(), D2D1::RectF(ix, iy, ix + side, iy + side), 1.0f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        drewCover = true;
                    }
                }
            }
            if (!drewCover && !DrawWmpIcon(dc, {x + 16, y + rowH / 2}, it.icon, autoPlaylist, s, special))
                wp::NavGlyph(dc, {x + 16, y + rowH / 2}, it.icon);
            auto layout = MakeText(Tr(it.text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, width - x - 34, rowH);
            dc->DrawTextLayout({x + 26, y}, layout.get(), ink.get());
        };
        float y = 6;
        for (size_t i = m_top; i < m_items.size(); ++i, y += rowH) {
            const Item& it = m_items[i];
            const bool selected =
                it.target == m_selTarget &&
                (it.target == Target::Playlist || it.target == Target::Category
                     ? it.playlist == m_selPlaylist
                     : it.target == Target::NeteaseRemotePlaylist ? it.remoteId == m_selRemoteId : true);
            const bool hot = i == m_hot;
            if (selected || hot)
                wp::RowPlate(dc, {4, y, width - 6, y + rowH},
                             selected ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected) : wp::RowState::Hot,
                             m_focused);
            const float x = 8 + it.level * kIndent;
            if (it.expandable) {
                const bool expanded = it.target == Target::PlaylistsHeader ? m_playlistsExpanded
                                      : it.target == Target::SmartHeader   ? m_smartExpanded
                                      : it.target == Target::LibraryAll    ? m_allExpanded
                                      : it.target == Target::LibraryLocal  ? m_localExpanded
                                      : it.target == Target::LibraryNetease ? m_neteaseExpanded
                                      : it.target == Target::NeteaseCreatedHeader ? m_createdExpanded
                                      : it.target == Target::NeteaseCollectedHeader ? m_collectedExpanded
                                                                           : false;
                // Win7 tree expander as vectors (the 16 px theme glyph went soft at 200 %).
                wp::Disclosure(dc, {x + 2, y + rowH / 2}, expanded, i == m_hotExpander);
            }
            drawRowContent(it, x, y);
        }
        if (m_dragging && m_press < m_items.size()) {
            // The drag preview is the whole row, drawn semi-transparent under the cursor.
            const Item& it = m_items[m_press];
            const float gx = 4;
            float gy = (m_dragPt.y - m_bounds.top) / s - rowH / 2;
            gy = std::clamp(gy, 0.0f, std::max(0.0f, h / s - rowH));
            winrt::com_ptr<ID2D1Layer> layer;
            if (SUCCEEDED(dc->CreateLayer(nullptr, layer.put()))) {
                D2D1_LAYER_PARAMETERS params{};
                params.contentBounds = D2D1::InfiniteRect();
                params.opacity = 0.72f;
                params.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
                params.maskTransform = D2D1::IdentityMatrix();
                dc->PushLayer(params, layer.get());
                winrt::com_ptr<ID2D1SolidColorBrush> fill, edge;
                dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), fill.put());
                dc->CreateSolidColorBrush(D2D1::ColorF(0x7DA2CE), edge.put());
                const D2D1_ROUNDED_RECT r{{gx, gy, width - 6, gy + rowH}, 3, 3};
                dc->FillRoundedRectangle(r, fill.get());
                dc->DrawRoundedRectangle(r, edge.get(), 1.0f);
                drawRowContent(it, gx + 4 + it.level * kIndent, gy);
                dc->PopLayer();
            }
        }
        if (m_dragging && m_drop != SIZE_MAX && m_drop >= m_top && m_drop < m_items.size()) {
            const float lineY = 6 + static_cast<float>(m_drop - m_top) * rowH + (m_dropAfter ? rowH : 0);
            if (lineY >= 6 && lineY <= h / s) {
                winrt::com_ptr<ID2D1SolidColorBrush> mark;
                dc->CreateSolidColorBrush(D2D1::ColorF(0x3C7FB1), mark.put());
                dc->FillRectangle({4, lineY - 1, width - 6, lineY + 1}, mark.get());
            }
        }
    });
}

}  // namespace glasswave::ui
