#include "pch.h"
#include "ui/nav_pane.h"
#include "gfx/d2d_util.h"
#include "core/text.h"
#include "gfx/composition_host.h"
#include "ui/art_cache.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "media/favorites.h"
#include "ui/i18n.h"
#include "media/special_playlists.h"

namespace glasswave::ui {

using core::Wide;

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
    case I::Disc: return 29560; // audio CD
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

// WMP 12's download arrow (wmploc 45221, the activity strip's last frame, 16 x 16 at x 288, y 9)
// in vectors, for every scale: measured outline through the pixel centres (stem x 4.5 - 8.5 from
// y 2.5, head x 1.5 - 11.5 at y 9.5, tip 6.5, 14.5), a one-pixel rim #1666A5 on the left to
// #2F7EB8 on the right, a fill lit on both sides (#7FCFF6 / #46B3EB | #26ACEE | #7CCEF4 /
// #94CDEE) darkening towards the tip (#1D9ADF).
void DownloadArrow(ID2D1DeviceContext* dc, D2D1_POINT_2F origin) {
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(path.put());
    path->Open(sink.put());
    const auto p = [&](float x, float y) { return D2D1_POINT_2F{origin.x + x, origin.y + y}; };
    sink->BeginFigure(p(4.5f, 2.5f), D2D1_FIGURE_BEGIN_FILLED);
    for (const auto& point : {p(8.5f, 2.5f), p(8.5f, 9.5f), p(11.5f, 9.5f), p(6.5f, 14.5f), p(1.5f, 9.5f),
                              p(4.5f, 9.5f)})
        sink->AddLine(point);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    const auto linear = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        winrt::com_ptr<ID2D1GradientStopCollection> list;
        dc->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), list.put());
        winrt::com_ptr<ID2D1LinearGradientBrush> brush;
        dc->CreateLinearGradientBrush({a, b}, list.get(), brush.put());
        return brush;
    };
    dc->FillGeometry(path.get(), linear(p(1.5f, 0), p(11.5f, 0),
                                        {{0.0f, D2D1::ColorF(0x7FCFF6)},
                                         {0.28f, D2D1::ColorF(0x46B3EB)},
                                         {0.5f, D2D1::ColorF(0x26ACEE)},
                                         {0.72f, D2D1::ColorF(0x7CCEF4)},
                                         {1.0f, D2D1::ColorF(0x94CDEE)}})
                                     .get());
    dc->FillGeometry(path.get(), linear(p(0, 9.5f), p(0, 14.5f),
                                        {{0.0f, D2D1::ColorF(0x1D9ADF, 0.0f)}, {1.0f, D2D1::ColorF(0x1D9ADF, 0.8f)}})
                                     .get());
    dc->DrawGeometry(path.get(),
                     linear(p(1.5f, 0), p(11.5f, 0), {{0.0f, D2D1::ColorF(0x1666A5)}, {1.0f, D2D1::ColorF(0x2F7EB8)}})
                         .get(),
                     1.0f);
}

wp::NavIcon SectionIcon(stream::SectionKind kind) {
    switch (kind) {
    case stream::SectionKind::Favorites: return wp::NavIcon::Playlist;
    case stream::SectionKind::Artists: return wp::NavIcon::Artist;
    case stream::SectionKind::Albums: return wp::NavIcon::Album;
    case stream::SectionKind::PlaylistShelf: return wp::NavIcon::Playlist;
    case stream::SectionKind::Tracks: return wp::NavIcon::Music;
    case stream::SectionKind::Groups: return wp::NavIcon::Genre;
    }
    return wp::NavIcon::Playlist;
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
    m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] { Paint(); });
    Rebuild();
    m_artToken = ArtCache::Get().Subscribe([this] { Paint(); });
}

NavPane::~NavPane() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    IconCache().clear();  // WIC objects go before COM does
}

int NavPane::RowHeightPx() const { return static_cast<int>(std::lround(kRowHeight * m_scale)); }

std::wstring NavPane::ItemKey(const Item& item) {
    switch (item.target) {
    case Target::Home: return L"home";
    case Target::Queue: return L"queue";
    case Target::LibraryAll: return L"all";
    case Target::LibraryLocal: return L"local";
    case Target::LibraryRemote: return std::wstring(item.provider->Key());
    case Target::AllFavorites: return L"favorites";
    case Target::AllRecentPlayed: return L"recent";
    case Target::AllMostPlayed: return L"mostplayed";
    case Target::AllDownloads: return L"downloads";
    case Target::PlaylistsHeader: return L"playlists";
    case Target::SmartHeader: return L"smart";
    case Target::DiscsHeader: return L"discs";
    case Target::RemoteSection: return item.sectionId;
    default: return {};
    }
}

// Expanded state is remembered per group; a provider's groups are qualified by its key.
std::wstring NavPane::ExpandKey(const Item& item) {
    const std::wstring key = ItemKey(item);
    return item.provider && item.target != Target::LibraryRemote ? std::wstring(item.provider->Key()) + L'/' + key : key;
}

bool NavPane::IsProviderGroup(const std::wstring& groupKey) const {
    return std::any_of(m_providers.begin(), m_providers.end(),
                       [&groupKey](const ProviderSection& s) { return s.provider->Key() == groupKey; });
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
    // Home above everything, where a streaming source has a home page (owner, 2026-11-15).
    if (std::any_of(stream::Providers().begin(), stream::Providers().end(),
                    [](const stream::Provider* p) { return p->Can(stream::Capability::Home); }))
        m_items.push_back({Target::Home, 0, 0, L"Home", wp::NavIcon::Library, false});
    // The play queue on top, like the "Up Next" / "Playing" list of current players.
    m_items.push_back({Target::Queue, media::QueuePlaylist(false), 0, L"Now Playing", wp::NavIcon::NowPlaying, false});
    // Playlists hold local and provider tracks alike, so they belong to All Music; each provider
    // library shows its own cloud playlists below.
    m_items.push_back({Target::LibraryAll, 0, 0, L"All Music", wp::NavIcon::Library, true});
    if (Expanded(m_items.back())) {
        m_items.push_back({Target::AllFavorites, 0, 1, L"Favorites", wp::NavIcon::Playlist, false});
        m_items.push_back({Target::AllRecentPlayed, 0, 1, L"Recently Played", wp::NavIcon::Playlist, false});
        m_items.push_back({Target::AllMostPlayed, 0, 1, L"Most Played", wp::NavIcon::Playlist, false});
        m_items.push_back({Target::AllDownloads, 0, 1, L"Downloads", wp::NavIcon::Playlist, false});
        m_items.push_back({Target::PlaylistsHeader, 0, 1, L"Playlists", wp::NavIcon::Playlist, true});
        if (Expanded(m_items.back())) {
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
    if (Expanded(m_items.back())) {
        const int count = static_cast<int>(m_categories.size());
        const int at = std::clamp(m_smartAt, 0, count);
        for (int i = 0; i <= count; ++i) {
            if (i == at) {
                m_items.push_back({Target::SmartHeader, 0, 1, L"Smart playlists", wp::NavIcon::Playlist, true});
                if (Expanded(m_items.back())) {
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
        if (!m_discs.empty()) {
            m_items.push_back({Target::DiscsHeader, 0, 1, L"Discs", wp::NavIcon::Disc, true});
            if (Expanded(m_items.back()))
                for (const auto& disc : m_discs)
                    m_items.push_back(
                        {Target::AudioCd, static_cast<size_t>(disc.drive), 2, disc.title, wp::NavIcon::Disc, false});
        }
    }
    for (const auto& section : m_providers) {
        stream::Provider* provider = section.provider;
        auto add = [&](Target target, int level, std::wstring text, wp::NavIcon icon, bool expandable) {
            Item item{target, 0, level, std::move(text), icon, expandable, provider};
            m_items.push_back(std::move(item));
            return Expanded(m_items.back());
        };
        if (!add(Target::LibraryRemote, 0, provider->Name(), wp::NavIcon::Library, true)) continue;
        if (!section.signedIn) continue;
        std::set<stream::Id> seenSections;
        for (const auto& navigation : section.sections) {
            if (navigation.id.empty() || !seenSections.emplace(navigation.id).second) continue;
            Item item{Target::RemoteSection, 0, 1, navigation.label, SectionIcon(navigation.kind),
                      navigation.kind == stream::SectionKind::PlaylistShelf, provider};
            item.sectionId = navigation.id;
            item.sectionKind = navigation.kind;
            item.sectionFacet = navigation.facet;
            item.sectionShelf = navigation.shelf;
            item.reorderChildren = navigation.reorderChildren;
            m_items.push_back(std::move(item));
            if (navigation.kind != stream::SectionKind::PlaylistShelf || !Expanded(m_items.back())) continue;
            const auto found = section.playlists.find(navigation.id);
            if (found == section.playlists.end()) continue;
            for (const auto& playlist : found->second) {
                Item child{Target::RemotePlaylist,
                           0,
                           2,
                           playlist.name.empty() ? provider->Text(L"Unnamed playlist")
                                                 : playlist.name,
                           wp::NavIcon::Playlist,
                           false,
                           provider,
                           playlist.id};
                child.sectionId = navigation.id;
                child.sectionKind = navigation.kind;
                child.sectionShelf = navigation.shelf;
                child.reorderChildren = navigation.reorderChildren;
                child.hasCover = !playlist.coverUrl.empty();
                m_items.push_back(std::move(child));
            }
        }
        // Signing in and out is on the preferences page (and the root's context menu).
    }
    ApplyStaticOrder();
    m_hot = SIZE_MAX;
    m_top = std::min(m_top, m_items.empty() ? size_t{0} : m_items.size() - 1);  // a group may have closed
    Paint();
}

void NavPane::SetProviders(std::vector<ProviderSection> sections) {
    // Pages report changes often; rebuild (and repaint the playlist covers) only when the
    // rows would differ.
    auto sameRows = [](const std::vector<stream::Playlist>& a, const std::vector<stream::Playlist>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const stream::Playlist& x, const stream::Playlist& y) {
                              return x.id == y.id && x.name == y.name &&
                                     x.coverUrl.empty() == y.coverUrl.empty();
                          });
    };
    auto sameSections = [](const std::vector<stream::NavigationSection>& a,
                           const std::vector<stream::NavigationSection>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const stream::NavigationSection& x, const stream::NavigationSection& y) {
                              return x.id == y.id && x.label == y.label && x.kind == y.kind &&
                                     x.shelf == y.shelf && x.facet == y.facet &&
                                     x.reorderChildren == y.reorderChildren;
                          });
    };
    auto same = [&](const ProviderSection& a, const ProviderSection& b) {
        if (a.provider != b.provider || a.signedIn != b.signedIn || !sameSections(a.sections, b.sections) ||
            a.playlists.size() != b.playlists.size())
            return false;
        for (const auto& [id, rows] : a.playlists) {
            const auto found = b.playlists.find(id);
            if (found == b.playlists.end() || !sameRows(rows, found->second)) return false;
        }
        return true;
    };
    if (std::equal(sections.begin(), sections.end(), m_providers.begin(), m_providers.end(), same)) return;
    m_providers = std::move(sections);
    Rebuild();
}

void NavPane::SetDiscs(std::vector<DiscRow> discs) {
    const bool same = std::equal(discs.begin(), discs.end(), m_discs.begin(), m_discs.end(),
                                 [](const DiscRow& a, const DiscRow& b) { return a.drive == b.drive && a.title == b.title; });
    if (same) return;
    m_discs = std::move(discs);
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

void NavPane::Select(Target target, size_t playlist, const stream::Provider* provider, const stream::Id& remoteId) {
    m_selTarget = target;
    m_selPlaylist = playlist;
    m_selProvider = provider;
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
        const std::wstring key = ExpandKey(*item);
        if (!m_collapsed.erase(key)) m_collapsed.insert(key);
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
    if (item.target == Target::DiscsHeader || item.target == Target::AudioCd) return false; // Local Music's last group
    if (item.level == 0) return !ItemKey(item).empty();
    if (item.level == 1) {
        const std::wstring group = GroupKeyOf(index);
        return group == L"all" || group == L"local" || IsProviderGroup(group);
    }
    if (item.level != 2) return false;
    for (size_t i = index; i > 0; --i) {
        if (m_items[i - 1].level >= 2) continue;
        const Item& parent = m_items[i - 1];
        if (parent.target == Target::RemoteSection) {
            return parent.sectionKind == stream::SectionKind::PlaylistShelf && parent.reorderChildren &&
                   parent.provider && parent.provider->Can(stream::Capability::ReorderPlaylists);
        }
        return parent.target == Target::PlaylistsHeader || parent.target == Target::SmartHeader;
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
        // Libraries with several subsections: All Music, Local Music and each provider.
        const std::wstring group = ItemKey(m_items[pa]);
        if (group == L"local")  // the Smart playlists section interleaves with the categories
            return (first.target == Target::Category || first.target == Target::SmartHeader) &&
                   (second.target == Target::Category || second.target == Target::SmartHeader);
        return group == L"all" || IsProviderGroup(group);
    }
    if (first.level != 2 || second.level != 2) return false;
    auto parentOf = [this](size_t index) {
        for (size_t i = index; i > 0; --i)
            if (m_items[i - 1].level < 2) return i - 1;
        return SIZE_MAX;
    };
    const size_t pa = parentOf(a);
    const size_t pb = parentOf(b);
    if (pa == SIZE_MAX || pb == SIZE_MAX || pa != pb) return false;
    // Playlist children are movable only when their own section explicitly permits it.
    return MovableItem(a) && MovableItem(b);
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
    // Rows start 6 DIP down; keep as much room under the last one.
    const long room = m_bounds.bottom - m_bounds.top - 2 * std::lround(6 * m_scale);
    const size_t visible = static_cast<size_t>(std::max(room, 0l) / std::max(RowHeightPx(), 1));
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
        const float L = gfx::Hairline(s);
        dc->FillRectangle({width - L, 0, width, h / s}, sep.get());
        auto drawRowContent = [&](const Item& it, float x, float y) {
            const bool autoPlaylist = it.target == Target::Playlist && media::IsAutoPlaylist(it.playlist);
            // Favorites: star; queue: WMP's playlist icon with the orbit; smart header: auto playlist.
            const int special = (it.target == Target::Playlist && it.text == L"Favorites") ||
                                        it.target == Target::AllFavorites ||
                                        (it.target == Target::RemoteSection &&
                                         it.sectionKind == stream::SectionKind::Favorites)
                                    ? 29526
                                : it.target == Target::Queue        ? 29513
                                : it.target == Target::Home         ? 29635 // WMP's online globe with a note
                                : it.target == Target::AllRecentPlayed ? 575   // clock with a music note
                                : it.target == Target::AllMostPlayed   ? 550   // pie chart
                                : it.target == Target::SmartHeader     ? 29572
                                : it.target == Target::Category ? CategoryIcon(static_cast<media::GroupKind>(it.playlist))
                                : it.target == Target::RemoteSection && it.sectionKind == stream::SectionKind::Groups &&
                                        it.sectionFacet == stream::GroupFacet::Composer
                                    ? CategoryIcon(media::GroupKind::Composer) // WMP's composer icon
                                    : 0;
            bool drewCover = false;
            if (it.target == Target::RemotePlaylist && !it.remoteId.empty() && it.hasCover) {
                const auto track = it.provider->ArtItem(stream::ArtKind::Playlist, it.remoteId);
                const std::wstring key = it.provider->ArtKey(stream::ArtKind::Playlist, it.remoteId);
                winrt::com_ptr<IWICBitmapSource> cover;
                const int coverPx = static_cast<int>(std::lround(16 * m_scale));
                ArtCache::Get().Find(key, track, coverPx, &cover); // a stand-in shows too
                if (cover) {
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
            // Downloads: WMP's download activity arrow at rest, in vectors (sharp at any scale).
            if (!drewCover && it.target == Target::AllDownloads) {
                DownloadArrow(dc, {std::round((x + 8 + 1.5f) * s) / s, std::round((y + rowH / 2 - 8) * s) / s});
                drewCover = true;
            }
            if (!drewCover && !DrawWmpIcon(dc, {x + 16, y + rowH / 2}, it.icon, autoPlaylist, s, special))
                wp::NavGlyph(dc, {x + 16, y + rowH / 2}, it.icon);
            auto layout = MakeText(Tr(it.text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, width - x - 34, rowH);
            dc->DrawTextLayout({x + 26, y}, layout.get(), ink.get());
        };
        float y = 6;
        const float maxY = h / s;
        for (size_t i = m_top; i < m_items.size() && y < maxY; ++i, y += rowH) {
            const Item& it = m_items[i];
            const bool selected =
                it.target == m_selTarget &&
                (it.target == Target::Playlist || it.target == Target::Category || it.target == Target::AudioCd
                     ? it.playlist == m_selPlaylist
                     : it.target == Target::RemoteSection
                           ? it.sectionId == m_selRemoteId
                           : it.target == Target::RemotePlaylist
                                 ? it.remoteId == m_selRemoteId
                           : true) &&
                (!it.provider || it.provider == m_selProvider);
            const bool hot = i == m_hot;
            if (selected || hot)
                wp::RowPlate(dc, {4, y, width - 6, y + rowH},
                             selected ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected) : wp::RowState::Hot,
                             m_focused);
            const float x = 8 + it.level * kIndent;
            if (it.expandable) {
                const bool expanded = Expanded(it);
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
