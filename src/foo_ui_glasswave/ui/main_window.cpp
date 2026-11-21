#include "pch.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <dbt.h>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "ui/main_window.h"
#include "download/downloads.h"
#include "ui/downloads_page.h"
#include "ui/home_page.h"
#include "media/radio.h"
#include "media/transport.h"
#include "core/log.h"
#include "core/text.h"
#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "gfx/composition_host.h"
#include "media/source_identity.h"
#include "ui/main_menu.h"
#include "ui/shell_glue.h"
#include "ui/taskbar.h"
#include "ui/win7_menu.h"
#include "ui/i18n.h"
#include "media/favorites.h"
#include "cd/playback.h"
#include "cd/discs.h"
#include "media/play_stats.h"
#include "media/special_playlists.h"
#include "media/queue_order.h"
#include "media/playback_scheduler.h"
#include "stream/playback_source.h"
#include "stream/provider.h"
#include "ui/art_cache.h"
#include "ui/cover_tint.h"
#include "ui/pane_common.h"
#include "ui/text_prompt.h"
#include "ui/lyrics_search.h"
#include "ui/lyrics_tools.h"
#include "gfx/effects.h"
#include "lyrics/scripts.h"
#include "app/settings.h"
#include "ui/remote_pages.h"
#include "ui/recent_page.h"
#include "ui/search_page.h"
#include "media/recent_lists.h"
#include "ui/search_popup.h"
#include "ui/song_actions.h"
#include "ui/song_table.h"
#include "ui/text.h"
#include "wmp/wmp_painter.h"
#include "wmp/wmp_transport.h"
#include "win7/win7_resources.h"

namespace glasswave::ui {

using core::Wide;

namespace m = aero::metrics;
namespace paint = aero::paint;
namespace wp = wmp::paint;
namespace wt = wmp::transport;
namespace wuc = winrt::Windows::UI::Composition;
using paint::Layer;
using paint::TransportGlyph;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.MainWindow";
constexpr wchar_t kAppName[] = L"Glasswave";
constexpr UINT_PTR kLibraryTimer = 1;
constexpr UINT_PTR kVolumeTipTimer = 2;
constexpr UINT_PTR kScrollAnimTimer = 3;
constexpr UINT_PTR kScrollRepeatTimer = 4;
constexpr UINT_PTR kPaneRevealTimer = 5;
constexpr UINT_PTR kDetailTipTimer = 6;
constexpr UINT_PTR kSuggestTimer = 7; // main_window_search.cpp: typing paused, ask for suggestions
constexpr UINT_PTR kArtHoldTimer = 8;  // a new song's cover kept it waiting long enough
constexpr UINT_PTR kWindowOpenTimer = 9;  // the opening animation ended: the shadow joins
constexpr UINT_PTR kWindowCloseTimer = 10; // the closing animation ended: foobar2000 exits
// Windows 7's window opening and closing (DWM): the window grows to its size from a little smaller
// while it fades in, and shrinks a little as it fades out. Estimated from the owner's memory of
// it, not measured yet (docs/design/visual-spec.md "Window open and close").
constexpr float kOpenFrom = 0.92f, kCloseTo = 0.94f;
constexpr int kOpenMs = 250, kCloseMs = 180;
constexpr UINT kArtHoldMs = 1500;
constexpr float kTabBand = 44.0f;     // DIP: the page's tabs over the list (main_window_search.cpp)
constexpr int kPaneRevealMs = 300;
constexpr UINT kFrameMessage = WM_APP + 0x32; // FrameClock: the lyrics pane draws a frame
constexpr float kLyricsPaneWidth = 440.0f;    // DIP (new UI, no Win7 measurement)
// Text rows of the transport band (title + artist and the lyric line on the left, stream
// format and output device on the right): 12 DIP Segoe UI on 16 DIP rows, both pairs
// centred on the band like the time between them.
constexpr float kInfoRow = 16.0f;
constexpr float kNavPaneWidth = 190.0f;
constexpr size_t kMenuItemLimit = 300; // breadcrumb menus list at most this many items

enum WidgetId {
    kCapMin = 1,
    kCapMax,
    kCapClose,
    kBack,
    kForward,
    kOrganize,
    kAddFiles,
    kAddFolder,
    kNewPlaylist,
    kSeek,
    kShuffle,
    kRepeat,
    kStop,
    kPrev,
    kNext,
    kPlay,
    kMute,
    kVolume,
    kViewOptions,
    kDetailAction,
    kDetailBriefToggle,
    kRefresh,
    kDownloadAll,
    kLocalPlay, // Play on a local album's or playlist's command bar
    kLyricsToggle,
    kVolArrow, // a narrow window's arrow beside the speaker: the volume flyout
    kCmdMore,  // the command bar's » for the buttons that no longer fit
};
bool IsCaptionWidget(int id) {
    return id >= kCapMin && id <= kCapClose;
}

// Split buttons: arrow half width, and the view options button's icon half (DIP).
constexpr float kSplitArrow = 16, kViewOptionsSplit = 26;

// Detail band above a provider's artist, album and playlist pages: a fixed band above the list (new UI, no
// Win7 measurement).
constexpr float kDetailHeaderHeight = 104, kDetailArt = 80, kDetailActionWidth = 88, kDetailActionHeight = 26;
// The subject's facts as Windows 7's details pane lists properties: "Label: value" rows of the
// band's 12 DIP type, in up to three columns of at least this width, filled downwards.
constexpr float kDetailFactRowH = 18, kDetailFactColumnMinW = 220, kDetailFactGap = 6, kDetailFactColumnGap = 18;
constexpr float kDetailTextTop = 62; // below the name and the stats line
constexpr float kDetailStatsLine = 20; // the stats line's share of it (none: the facts move up)
constexpr float kDetailBriefMaxHeight = 260.0f, kDetailScrollWidth = 17.0f;

// {0C9E7B42-1F3A-4D5E-8B6C-7A2F9E1D4C83}
cfg_var_modern::cfg_struct_t<WINDOWPLACEMENT> cfg_placement(
    {0x0c9e7b42, 0x1f3a, 0x4d5e, {0x8b, 0x6c, 0x7a, 0x2f, 0x9e, 0x1d, 0x4c, 0x83}});

// WMP view option chosen per library view (media::ViewStyle values).
// {6A1F3C58-2B7E-4D90-8C15-3E9A7B2D4F61} .. {6A1F3C5B-...}
cfg_var_modern::cfg_int cfg_viewSongs(
    {0x6a1f3c58, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));
cfg_var_modern::cfg_int cfg_viewAlbums(
    {0x6a1f3c59, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));
cfg_var_modern::cfg_int cfg_viewArtists(
    {0x6a1f3c5a, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));
cfg_var_modern::cfg_int cfg_viewGenres(
    {0x6a1f3c5b, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));
// Playlists: Details (WMP's default for a playlist) or Expanded tile.
cfg_var_modern::cfg_int cfg_viewPlaylist(
    {0x6a1f3c5e, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));
// The other Music categories (Year, Rating, Composer, ...) share one view option.
cfg_var_modern::cfg_int cfg_viewCategories(
    {0x6a1f3c5c, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));
// All Music > Favorites; a provider's playlists, songs and saved albums.
// {6A1F3C5F-2B7E-4D90-8C15-3E9A7B2D4F61} .. {6A1F3C62-...}
cfg_var_modern::cfg_int cfg_viewAllFavorites(
    {0x6a1f3c5f, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));
// All Music > Recently Played / Most Played: the cross-source play lists.
cfg_var_modern::cfg_int cfg_viewPlayStats(
    {0x6a1f3c63, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));
cfg_var_modern::cfg_int cfg_viewRemotePlaylists(
    {0x6a1f3c60, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));
cfg_var_modern::cfg_int cfg_viewRemoteSongs(
    {0x6a1f3c61, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));
// The search page's categories: songs (and lyrics), and artists, albums and playlists as tiles.
cfg_var_modern::cfg_int cfg_viewSearchSongs(
    {0x6a1f3c70, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));
cfg_var_modern::cfg_int cfg_viewSearchTiles(
    {0x6a1f3c71, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));
// Saved albums: cover tiles, or the saved albums' songs grouped by album.
cfg_var_modern::cfg_int cfg_viewRemoteAlbums(
    {0x6a1f3c62, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));
// Artist page: album tiles, or all album songs grouped by album.
// {6A1F3C66-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_int cfg_viewRemoteArtist(
    {0x6a1f3c66, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));
// Music categories shown in the navigation pane, in order: media::GroupKind values as digits.
// {6A1F3C5D-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_string cfg_navCategories(
    {0x6a1f3c5d, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, "012");
// Static navigation sections, in order: root and the library child groups.
// {6A1F3C67-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_string cfg_navOrder(
    {0x6a1f3c67, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, "");
// Where the Smart playlists section sits among the Local Music categories (0 = first).
// {6A1F3C64-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_int cfg_navSmartAt(
    {0x6a1f3c64, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, 0);
// The page and top row shown when the window closed.
// {6A1F3C65-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_string cfg_lastView(
    {0x6a1f3c65, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, "");
// The lyrics pane is open.
// {6A1F3C68-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_int cfg_lyricsOpen(
    {0x6a1f3c68, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, 0);

// The lyrics button's glyph: a speech bubble with a quotation mark (as in Apple Music).
void DrawLyricsGlyph(ID2D1DeviceContext* dc, D2D1_SIZE_F s, ID2D1Brush* brush) {
    com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    const float cx = std::round(s.width / 2), cy = std::round(s.height / 2);
    const float l = cx - 7.5f, t = cy - 6.5f, r = cx + 7.5f, b = cy + 4.0f, rad = 3;
    com_ptr<ID2D1PathGeometry> path;
    if (FAILED(f->CreatePathGeometry(path.put()))) return;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;
    const auto arc = [&](float x, float y) {
        sink->AddArc({{x, y}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    };
    sink->BeginFigure({l + rad, t}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({r - rad, t});
    arc(r, t + rad);
    sink->AddLine({r, b - rad});
    arc(r - rad, b);
    sink->AddLine({l + 6.5f, b});
    sink->AddLine({l + 2.5f, b + 3.5f}); // tail
    sink->AddLine({l + rad, b});
    arc(l, b - rad);
    sink->AddLine({l, t + rad});
    arc(l + rad, t);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    dc->DrawGeometry(path.get(), brush, 1.3f);
    for (const float x : {cx - 2.2f, cx + 2.4f}) {
        dc->FillEllipse(D2D1::Ellipse({x, cy - 1.9f}, 1.35f, 1.35f), brush);
        dc->DrawLine({x + 1.1f, cy - 1.6f}, {x - 0.6f, cy + 1.2f}, brush, 1.1f);
    }
}

std::vector<media::GroupKind> LoadNavCategories() {
    std::vector<media::GroupKind> out;
    for (char c : std::string(cfg_navCategories.get().c_str()))
        if (c >= '0' && c < '0' + static_cast<int>(media::kGroupKinds)) {
            const auto kind = static_cast<media::GroupKind>(c - '0');
            if (std::find(out.begin(), out.end(), kind) == out.end()) out.push_back(kind);
        }
    return out;
}

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f,
                        a);
}

std::wstring CleanConfigField(std::wstring text) {
    for (auto& c : text)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return text;
}

std::vector<std::wstring> SplitConfigFields(const std::wstring& text) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    while (true) {
        const size_t tab = text.find(L'\t', start);
        fields.push_back(text.substr(start, tab == std::wstring::npos ? std::wstring::npos : tab - start));
        if (tab == std::wstring::npos) break;
        start = tab + 1;
    }
    return fields;
}

std::wstring LyricsSettingsSignature() {
    std::wstring signature;
    auto add = [&signature](std::wstring_view value) {
        signature += value;
        signature.push_back(L'\x1f');
    };
    for (const auto& field : lyrics::scripts::SettingFields()) {
        if (field.kind != settings::Field::Kind::Choice && field.kind != settings::Field::Kind::Text)
            continue;
        add(field.label);
        add(field.get ? std::to_wstring(field.get()) : field.getText());
    }
    // Reloading a script replaces its registered object and clears cached automatic results,
    // even when its visible name and settings stay the same.
    for (const auto& source : lyrics::Searchers()) {
        add(source->Name());
        add(std::to_wstring(reinterpret_cast<std::uintptr_t>(source.get())));
    }
    return signature;
}

std::optional<stream::NavigationSection> FindProviderSection(const stream::Provider& provider,
                                                             const stream::Id& id) {
    if (id.empty()) return std::nullopt;
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(),
                                    [&id](const auto& section) { return section.id == id; });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

std::optional<stream::NavigationSection> FindProviderSection(
    const stream::Provider& provider, stream::SectionKind kind,
    std::optional<stream::Shelf> shelf = std::nullopt) {
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(), [kind, shelf](const auto& section) {
        return section.kind == kind &&
               (!shelf || (section.kind == stream::SectionKind::PlaylistShelf && section.shelf == *shelf));
    });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

stream::NavigationSection CreatedPlaylistSection(const stream::Provider& provider) {
    if (const auto section =
            FindProviderSection(provider, stream::SectionKind::PlaylistShelf, stream::Shelf::Created))
        return *section;
    return {L"created", L"Created playlists", stream::SectionKind::PlaylistShelf, stream::Shelf::Created,
            false};
}

// The section a page stands in. A section's own pages are in it; an album, an artist or a
// playlist only when it is really there (saved, followed, the user's): otherwise it stands right
// under its source (owner, 2026-11-21: an artist page read 唱片集 > ..., a playlist from Home
// "my playlists"). Where a page was opened from is View::from's business, not this one's.
std::optional<stream::NavigationSection> SectionForView(const stream::Provider& provider, const View& view) {
    switch (view.kind) {
    case View::Kind::RemoteArtist:
    case View::Kind::RemoteTopTracks: {
        const bool followed = provider.ArtistPreferenceState(view.remoteId).value_or([&] {
            const auto artists = provider.FollowedArtists();
            return std::any_of(artists.begin(), artists.end(),
                               [&view](const stream::Artist& a) { return a.id == view.remoteId; });
        }());
        return followed ? FindProviderSection(provider, stream::SectionKind::Artists) : std::nullopt;
    }
    case View::Kind::RemoteAlbum:
        return provider.IsAlbumSaved(view.remoteId) ? FindProviderSection(provider, stream::SectionKind::Albums)
                                                    : std::nullopt;
    case View::Kind::RemotePlaylist:
        return provider.SectionOfPlaylist(view.remoteId);
    default:
        break;
    }
    if (const auto section = FindProviderSection(provider, view.sectionId)) return section;
    switch (view.kind) {
    case View::Kind::RemoteFavorites:
        return FindProviderSection(provider, stream::SectionKind::Favorites);
    case View::Kind::RemoteArtists:
        return FindProviderSection(provider, stream::SectionKind::Artists);
    case View::Kind::RemoteAlbums:
        return FindProviderSection(provider, stream::SectionKind::Albums);
    case View::Kind::RemoteShelf:
        return FindProviderSection(provider, stream::SectionKind::PlaylistShelf, view.shelf);
    case View::Kind::RemoteSectionTracks:
        return FindProviderSection(provider, stream::SectionKind::Tracks);
    case View::Kind::RemoteGroups:
    case View::Kind::RemoteGroupTracks:
        return FindProviderSection(provider, stream::SectionKind::Groups);
    default:
        return std::nullopt;
    }
}

View ProviderLandingView(const stream::Provider& provider) {
    const auto sections = provider.Sections();
    for (const auto& section : sections)
        if (!section.id.empty()) return SectionView(provider, section);
    return RemoteView(provider, View::Kind::RemoteFavorites);
}

// A section's own pages need it; an album, an artist or a playlist stands without one (right
// under its source when it is not in the library, SectionForView).
bool SectionBackedView(View::Kind kind) {
    switch (kind) {
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteShelf:
    case View::Kind::RemoteArtists:
    case View::Kind::RemoteAlbums:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
    case View::Kind::RemoteGroupTracks:
        return true;
    default:
        return false;
    }
}

View ProviderGroupsFallback(const stream::Provider& provider, const stream::Id& preferred) {
    if (const auto section = FindProviderSection(provider, preferred);
        section && section->kind == stream::SectionKind::Groups)
        return SectionView(provider, *section);
    if (const auto section = FindProviderSection(provider, stream::SectionKind::Groups))
        return SectionView(provider, *section);
    return ProviderLandingView(provider);
}

// Repairs a view against the provider's current section snapshot. Group ids that came from
// localized service data are not stable, so a language/account transition falls back to the
// provider's Groups page instead of opening a track page with an obsolete group id.
View NormalizeRemoteView(const stream::Provider& provider, View view, bool discardGroupTracks) {
    if (!view.IsRemote() || view.provider != provider.Key()) return view;
    if (view.kind == View::Kind::Search) return view; // its sectionId is the tab, not a section
    const bool sectionValid =
        !view.sectionId.empty() && FindProviderSection(provider, view.sectionId).has_value();
    if (!sectionValid) view.sectionId.clear();
    if (view.kind == View::Kind::RemoteGroupTracks &&
        (discardGroupTracks || !sectionValid || view.groupId.empty()))
        return ProviderGroupsFallback(provider, view.sectionId);
    if (sectionValid || view.sectionId.empty()) {
        if (const auto section = SectionForView(provider, view)) view.sectionId = section->id;
    }
    if (SectionBackedView(view.kind) && view.sectionId.empty()) return ProviderLandingView(provider);
    if (const auto section = FindProviderSection(provider, view.sectionId)) {
        switch (view.kind) {
        case View::Kind::RemoteFavorites:
        case View::Kind::RemoteShelf:
        case View::Kind::RemoteArtists:
        case View::Kind::RemoteAlbums:
        case View::Kind::RemoteSectionTracks:
        case View::Kind::RemoteGroups:
            view.name = section->label;
            break;
        default:
            break;
        }
    }
    if (view.kind == View::Kind::RemotePlaylist) {
        if (const auto playlist = provider.FindPlaylist(view.remoteId); playlist && !playlist->name.empty())
            view.name = playlist->name;
    } else if (view.kind == View::Kind::RemoteAlbum) {
        if (const auto album = provider.FindAlbum(view.remoteId); album && !album->name.empty())
            view.name = album->name;
    }
    if (view.kind == View::Kind::RemoteArtist || view.kind == View::Kind::RemoteTopTracks) {
        for (const auto& artist : provider.FollowedArtists())
            if (artist.id == view.remoteId && !artist.name.empty()) {
                view.name = artist.name;
                break;
            }
    }
    return view;
}

wuc::CompositionNineGridBrush NineGrid(int wPx, int hPx, float scale, float insetPx, bool hollow,
                                       const std::function<void(ID2D1DeviceContext*)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    auto surface = host.CreateSurface(wPx, hPx);
    host.Draw(surface, scale, painter);
    auto source = host.Compositor().CreateSurfaceBrush(surface);
    source.Stretch(wuc::CompositionStretch::Fill);
    auto brush = host.Compositor().CreateNineGridBrush();
    brush.Source(source);
    brush.SetInsets(insetPx);
    brush.IsCenterHollow(hollow);
    return brush;
}

// A horizontally stretchable strip: painted once a few pixels wide, stretched to any width.
wuc::CompositionSurfaceBrush Strip(int hPx, float scale,
                                   const std::function<void(ID2D1DeviceContext*, float)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    auto surface = host.CreateSurface(4, hPx);
    host.Draw(surface, scale, [&](ID2D1DeviceContext* dc) { painter(dc, hPx / scale); });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    return brush;
}

com_ptr<ID2D1LinearGradientBrush> VGrad(ID2D1DeviceContext* dc, float y0, float y1,
                                        std::initializer_list<std::pair<float, uint32_t>> stops) {
    std::vector<D2D1_GRADIENT_STOP> v;
    for (auto& [p, c] : stops)
        v.push_back({p, Hex(c)});
    com_ptr<ID2D1GradientStopCollection> coll;
    winrt::check_hresult(
        dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), coll.put()));
    com_ptr<ID2D1LinearGradientBrush> b;
    winrt::check_hresult(dc->CreateLinearGradientBrush({{0, y0}, {0, y1}}, coll.get(), b.put()));
    return b;
}

void FillLine(ID2D1DeviceContext* dc, float y, float h, uint32_t color) {
    com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(Hex(color), b.put());
    dc->FillRectangle({-1, y, 100, y + h}, b.get());
}

void Place(const wuc::Visual& v, const RECT& r) {
    v.Offset(float3{static_cast<float>(r.left), static_cast<float>(r.top), 0});
    v.Size(float2{static_cast<float>(std::max<LONG>(r.right - r.left, 0)),
                  static_cast<float>(std::max<LONG>(r.bottom - r.top, 0))});
}

RECT CenteredRect(int cx, int cy, int w, int h) {
    return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
}

// Windows Media Player's own icon from the user's wmploc.dll (icon group 102), else
// foobar2000's.
constexpr int kWmpIcon = 102;
HICON CreateAppIcon(int sizePx) {
    if (HICON icon = win7::Resources::Get().Icon(win7::Module::Wmploc, kWmpIcon, sizePx)) return icon;
    return ui_control::get()->load_main_icon(sizePx, sizePx);
}

// WMP's large icon frames carry uneven transparent margins (the 64 px one shows 50x46 px,
// low and left), so the taskbar drew it small and off-centre. This builds a big icon
// from the 256 px frame cropped to its visible part, centred and scaled to `sizePx`.
HICON CreateCenteredAppIcon(int sizePx) {
    HICON big = win7::Resources::Get().Icon(win7::Module::Wmploc, kWmpIcon, 256);
    if (!big) return CreateAppIcon(sizePx);
    auto* wic = gfx::CompositionHost::Get().Wic();
    com_ptr<IWICBitmap> src;
    HICON result = nullptr;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(big, src.put()))) {
        UINT w = 0, h = 0;
        src->GetSize(&w, &h);
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        src->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data());
        UINT l = w, t = h, r = 0, b = 0;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > 8) {
                    l = std::min(l, x), t = std::min(t, y), r = std::max(r, x), b = std::max(b, y);
                }
        if (r > l && b > t) {
            com_ptr<IWICBitmapClipper> clip;
            com_ptr<IWICBitmapScaler> scaler;
            const UINT cw = r - l + 1, ch = b - t + 1, side = std::max(cw, ch);
            const UINT dw = std::max<UINT>(1, sizePx * cw / side), dh = std::max<UINT>(1, sizePx * ch / side);
            const WICRect rc{static_cast<INT>(l), static_cast<INT>(t), static_cast<INT>(cw),
                             static_cast<INT>(ch)};
            if (SUCCEEDED(wic->CreateBitmapClipper(clip.put())) &&
                SUCCEEDED(clip->Initialize(src.get(), &rc)) &&
                SUCCEEDED(wic->CreateBitmapScaler(scaler.put())) &&
                SUCCEEDED(
                    scaler->Initialize(clip.get(), dw, dh, WICBitmapInterpolationModeHighQualityCubic))) {
                std::vector<BYTE> scaled(static_cast<size_t>(dw) * dh * 4);
                scaler->CopyPixels(nullptr, dw * 4, static_cast<UINT>(scaled.size()), scaled.data());
                // Into a centred square 32-bpp DIB (BGRA, straight alpha as icons expect).
                BITMAPV5HEADER bh{};
                bh.bV5Size = sizeof(bh);
                bh.bV5Width = sizePx;
                bh.bV5Height = -sizePx;
                bh.bV5Planes = 1;
                bh.bV5BitCount = 32;
                bh.bV5Compression = BI_BITFIELDS;
                bh.bV5RedMask = 0x00FF0000, bh.bV5GreenMask = 0x0000FF00, bh.bV5BlueMask = 0x000000FF,
                bh.bV5AlphaMask = 0xFF000000;
                void* bits = nullptr;
                HDC screen = GetDC(nullptr);
                HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS,
                                                 &bits, nullptr, 0);
                ReleaseDC(nullptr, screen);
                if (color) {
                    auto* out = static_cast<BYTE*>(bits);
                    std::memset(out, 0, static_cast<size_t>(sizePx) * sizePx * 4);
                    const UINT ox = (sizePx - dw) / 2, oy = (sizePx - dh) / 2;
                    for (UINT y = 0; y < dh; ++y)
                        std::memcpy(out + ((static_cast<size_t>(y) + oy) * sizePx + ox) * 4,
                                    scaled.data() + static_cast<size_t>(y) * dw * 4,
                                    static_cast<size_t>(dw) * 4);
                    HBITMAP mask = CreateBitmap(sizePx, sizePx, 1, 1, nullptr);
                    ICONINFO ii{TRUE, 0, 0, mask, color};
                    result = CreateIconIndirect(&ii);
                    DeleteObject(mask);
                    DeleteObject(color);
                }
            }
        }
    }
    DestroyIcon(big);
    return result ? result : CreateAppIcon(sizePx);
}

com_ptr<IWICBitmapSource> LoadAppIcon(int sizePx) {
    HICON icon = CreateAppIcon(sizePx);
    if (!icon) return nullptr;
    auto* wic = gfx::CompositionHost::Get().Wic();
    com_ptr<IWICBitmap> bmp;
    com_ptr<IWICFormatConverter> conv;
    com_ptr<IWICBitmapSource> result;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, bmp.put())) &&
        SUCCEEDED(wic->CreateFormatConverter(conv.put())) &&
        SUCCEEDED(conv->Initialize(bmp.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                   0, WICBitmapPaletteTypeMedianCut)))
        result = conv;
    DestroyIcon(icon);
    return result;
}

// Volume slider position <-> foobar2000 volume in dB (-100..0); 40*log10 feels even.
float SliderFromDb(float db) {
    return db <= -99.0f ? 0.0f : std::clamp(std::pow(10.0f, db / 40.0f), 0.0f, 1.0f);
}
float DbFromSlider(float v) {
    return v <= 0.01f ? -100.0f : std::max(-100.0f, 40.0f * std::log10(v));
}

bool KeyDown(int vk) {
    return (GetKeyState(vk) & 0x8000) != 0;
}

// Glasswave owns predictable shuffle/repeat scheduling; the core owns decoding and output.
// {8D2F6A13-4B7C-4E19-9F3A-2C6E8B1D5A70} / {8D2F6A14-...}
cfg_var_modern::cfg_int cfg_shuffle(
    {0x8d2f6a13, 0x4b7c, 0x4e19, {0x9f, 0x3a, 0x2c, 0x6e, 0x8b, 0x1d, 0x5a, 0x70}}, 0);
cfg_var_modern::cfg_int cfg_repeat(
    {0x8d2f6a14, 0x4b7c, 0x4e19, {0x9f, 0x3a, 0x2c, 0x6e, 0x8b, 0x1d, 0x5a, 0x70}}, 0);

} // namespace

// ---- Core callbacks ----------------------------------------------------------------------

struct MainWindow::PlayCallback : play_callback_impl_base {
    explicit PlayCallback(MainWindow& w)
        : play_callback_impl_base(flag_on_playback_starting | flag_on_playback_new_track |
                                  flag_on_playback_stop | flag_on_playback_pause | flag_on_playback_edited |
                                  flag_on_playback_seek | flag_on_playback_time |
                                  flag_on_playback_dynamic_info | flag_on_playback_dynamic_info_track |
                                  flag_on_volume_change),
          window(w) {}

    void on_playback_starting(play_control::t_track_command command, bool) override {
        window.OnSwitching(true);
        window.PreviewStartingTrack(command);
        window.OnPlaybackStateChanged();
    }
    void on_playback_new_track(metadb_handle_ptr track) override {
        if (track.is_valid() && !stream::CanPlaySource(track->get_path())) return;
        window.OnSwitching(false, track);
        media::PlayStats::Get().Record(track);
        window.RefreshPlayCounts();
        window.OnTrackChanged(true);
        window.OnPlaybackStateChanged();
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason == play_control::stop_reason_starting_another) {
            window.OnSwitching(true);
            return;
        }
        window.OnSwitching(false);
        window.OnPlaybackStateChanged();
        window.ClearShownTrack(); // stopped: nothing plays, nothing is shown as playing
        window.OnTrackChanged();
    }
    void on_playback_pause(bool) override { window.OnPlaybackStateChanged(); }
    void on_playback_seek(double) override { window.OnPlaybackTime(); }
    void on_playback_time(double) override { window.OnPlaybackTime(); }
    void on_playback_edited(metadb_handle_ptr track) override {
        window.OnTrackChanged();
        window.RequestLyrics(track, false);
    }
    void on_playback_dynamic_info(const file_info&) override { window.OnTrackChanged(); }
    void on_playback_dynamic_info_track(const file_info&) override { window.OnTrackChanged(); }
    void on_volume_change(float) override { window.OnVolumeChanged(); }

    MainWindow& window;
};

struct MainWindow::PlaylistCallback : playlist_callback_impl_base {
    explicit PlaylistCallback(MainWindow& w)
        : playlist_callback_impl_base(static_cast<t_uint32>(flag_all)), window(w) {}

    void on_items_added(t_size p, t_size, metadb_handle_list_cref, const pfc::bit_array&) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_reordered(t_size p, const t_size*, t_size) override { window.OnPlaylistContentChanged(p); }
    void on_items_removed(t_size p, const pfc::bit_array&, t_size, t_size) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_modified(t_size p, const pfc::bit_array&) override { window.OnPlaylistContentChanged(p); }
    void on_items_replaced(t_size p, const pfc::bit_array&,
                           const pfc::list_base_const_t<t_on_items_replaced_entry>&) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_selection_change(t_size p, const pfc::bit_array&, const pfc::bit_array&) override {
        window.OnPlaylistSelectionChanged(p);
    }
    void on_item_focus_change(t_size p, t_size, t_size) override { window.OnPlaylistSelectionChanged(p); }
    // Views hold playlist indices; every change of the set moves them along.
    void on_playlist_created(t_size p, const char*, t_size) override {
        window.OnPlaylistsMoved([p](size_t i) { return i >= p ? i + 1 : i; });
    }
    void on_playlist_locked(t_size, bool) override { window.OnPlaylistLockChanged(); }
    void on_playlists_reorder(const t_size* order, t_size count) override {
        std::vector<size_t> moved(count);
        for (t_size to = 0; to < count; ++to)
            moved[order[to]] = to; // order[new] = old
        window.OnPlaylistsMoved([&moved](size_t i) { return i < moved.size() ? moved[i] : i; });
    }
    void on_playlists_removed(const pfc::bit_array& mask, t_size oldCount, t_size) override {
        std::vector<size_t> moved(oldCount, SIZE_MAX);
        for (t_size i = 0, next = 0; i < oldCount; ++i)
            if (!mask[i]) moved[i] = next++;
        window.OnPlaylistsMoved([&moved](size_t i) { return i < moved.size() ? moved[i] : SIZE_MAX; });
    }
    void on_playlist_renamed(t_size, const char*, t_size) override { window.OnPlaylistsChanged(); }
    void on_playback_order_changed(t_size) override { window.OnPlaybackOrderChanged(); }

    MainWindow& window;
};

struct MainWindow::LibraryCallback : library_callback_v2_dynamic_impl_base {
    explicit LibraryCallback(MainWindow& w) : window(w) {}
    void on_items_added(metadb_handle_list_cref) override { window.ScheduleLibraryReload(); }
    void on_items_removed(metadb_handle_list_cref) override { window.ScheduleLibraryReload(); }
    void on_items_modified_v2(metadb_handle_list_cref, metadb_io_callback_v2_data&) override {
        window.ScheduleLibraryReload();
    }
    void on_library_initialized() override { window.ScheduleLibraryReload(); }
    MainWindow& window;
};

// ---- Creation / destruction ----------------------------------------------------------

// Provider notifications, delivered on the main thread while the window exists.
struct MainWindow::ProviderListener : stream::Provider::Listener {
    explicit ProviderListener(MainWindow& owner) : window(owner) {
        for (auto* provider : stream::Providers())
            provider->AddListener(this);
    }
    ~ProviderListener() override {
        for (auto* provider : stream::Providers())
            provider->RemoveListener(this);
    }
    void OnAccountObserved(stream::Provider& provider) override {
        window.OnProviderAccountObserved(provider);
    }
    void OnAccountChanged(stream::Provider& provider) override { window.OnProviderAccount(provider); }
    void OnLibraryChanged(stream::Provider& provider) override { window.OnProviderLibrary(provider); }
    void OnFavoritesChanged(stream::Provider& provider) override { window.OnProviderFavorites(provider); }
    void OnLanguageChanged(stream::Provider& provider) override { window.OnProviderLanguage(provider); }
    void OnDisplayChanged(stream::Provider& provider) override { window.OnProviderDisplay(provider); }
    MainWindow& window;
};

namespace {
MainWindow* g_instance = nullptr;

class CoreQueueViewEvents : public playback_queue_callback {
  public:
    void on_changed(t_change_origin) override {
        if (auto* window = MainWindow::Instance()) window->OnCoreQueueChanged();
    }
};
service_factory_single_t<CoreQueueViewEvents> g_coreQueueViewEvents;

// View > Lyrics: shows or hides the lyrics pane (bindable to a keyboard shortcut).
class LyricsCommand : public mainmenu_commands {
  public:
    // {6A1F3C69-2B7E-4D90-8C15-3E9A7B2D4F61}
    // {6A1F3C69-2B7E-4D90-8C15-3E9A7B2D4F61}, {6A1F3C6A-2B7E-4D90-8C15-3E9A7B2D4F61}
    static constexpr GUID kLyrics{
        0x6a1f3c69, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    static constexpr GUID kUpNext{
        0x6a1f3c6a, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    static constexpr GUID kMini{
        0x6a1f3c6b, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    static constexpr GUID kFullScreen{
        0x6a1f3c6c, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    t_uint32 get_command_count() override { return 4; }
    GUID get_command(t_uint32 index) override {
        return index == 0 ? kLyrics : index == 1 ? kUpNext : index == 2 ? kMini : kFullScreen;
    }
    void get_name(t_uint32 index, pfc::string_base& out) override {
        static const char* const names[] = {"Lyrics", "Up Next", "Mini player", "Full screen"};
        out = names[index < 4 ? index : 0];
    }
    bool get_description(t_uint32 index, pfc::string_base& out) override {
        static const char* const texts[] = {"Shows or hides the Glasswave lyrics pane.",
                                            "Shows the queue and history in the Glasswave pane.",
                                            "Switches Glasswave to its mini player.",
                                            "Shows what is playing on the whole screen."};
        out = texts[index < 4 ? index : 0];
        return true;
    }
    GUID get_parent() override { return mainmenu_groups::view; }
    bool get_display(t_uint32 index, pfc::string_base& text, t_uint32& flags) override {
        const bool on = g_instance && (index == 0   ? g_instance->LyricsPaneOpen()
                                       : index == 1 ? g_instance->QueuePaneShown()
                                                    : g_instance->PlayerShown(index == 3));
        flags = on ? flag_checked : 0;
        get_name(index, text);
        return g_instance != nullptr;
    }
    void execute(t_uint32 index, service_ptr_t<service_base>) override {
        if (!g_instance) return;
        if (index == 0)
            g_instance->ToggleLyricsPane();
        else if (index == 1)
            g_instance->ToggleQueuePane();
        else
            g_instance->ShowPlayer(index == 3);
    }
};
mainmenu_commands_factory_t<LyricsCommand> g_lyricsCommand;
} // namespace

MainWindow* MainWindow::Instance() {
    return g_instance;
}

MainWindow::MainWindow() {
    g_instance = this;
    stream::SetPlaybackOpeningListener([](const std::string& path) {
        if (g_instance) g_instance->OnTrackOpening(path);
    });
}

MainWindow::~MainWindow() {
    if (g_instance == this) {
        g_instance = nullptr;
        stream::SetPlaybackOpeningListener({});
    }
    if (m_hwnd) DestroyWindow(m_hwnd);
}

HWND MainWindow::Create(user_interface::HookProc_t hook) {
    stream::StartProviders(); // the UI can come up before foobar2000's initquit services
    m_hook = hook;
    const HINSTANCE inst = core_api::get_my_instance();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ui_control::get()->get_main_icon();
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc); // failing because it already exists is fine

    const float sys = GetDpiForSystem() / 96.0f;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = static_cast<int>(1000 * sys), h = static_cast<int>(680 * sys);
    const int x = work.left + (work.right - work.left - w) / 2,
              y = work.top + (work.bottom - work.top - h) / 2;

    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, kClassName, kAppName,
                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, this);
    if (!m_hwnd) throw exception_win32(GetLastError());

    int show = SW_SHOWNORMAL;
    WINDOWPLACEMENT wpl = cfg_placement.get();
    if (wpl.length == sizeof(WINDOWPLACEMENT)) {
        show = (wpl.showCmd == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        wpl.showCmd = SW_HIDE;
        wpl.flags = 0;
        SetWindowPlacement(m_hwnd, &wpl);
    }
    const bool animate = WindowAnimations();
    if (animate) {
        m_windowRoot.Opacity(0.0f); // the first frame is already the animation's
        m_shadow.SetSuppressed(true);
    }
    ShowWindow(m_hwnd, show);
    if (animate) AnimateWindow(true);
    m_shadow.Sync();
    return m_hwnd;
}

bool MainWindow::WindowAnimations() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

// The window's content grows (or shrinks) about its middle while it fades; the shadow window
// is out of the way meanwhile and comes back once the window stands at its size.
void MainWindow::AnimateWindow(bool open) {
    if (!m_windowRoot) return;
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_windowRoot.CenterPoint(float3{client.right / 2.0f, client.bottom / 2.0f, 0});
    const auto ease = open ? comp.CreateCubicBezierEasingFunction({0.1f, 0.9f}, {0.2f, 1.0f})
                           : comp.CreateCubicBezierEasingFunction({0.4f, 0.0f}, {1.0f, 1.0f});
    const auto duration = std::chrono::milliseconds(open ? kOpenMs : kCloseMs);
    auto scale = comp.CreateVector3KeyFrameAnimation();
    const float from = open ? kOpenFrom : 1.0f, to = open ? 1.0f : kCloseTo;
    scale.InsertKeyFrame(0.0f, float3{from, from, 1});
    scale.InsertKeyFrame(1.0f, float3{to, to, 1}, ease);
    scale.Duration(duration);
    m_windowRoot.StartAnimation(L"Scale", scale);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, open ? 0.0f : 1.0f);
    fade.InsertKeyFrame(1.0f, open ? 1.0f : 0.0f, ease);
    fade.Duration(duration);
    m_windowRoot.StartAnimation(L"Opacity", fade);
    if (!open) {
        m_shadow.SetSuppressed(true);
        m_shadow.Sync();
    }
    SetTimer(m_hwnd, open ? kWindowOpenTimer : kWindowCloseTimer, open ? kOpenMs : kCloseMs, nullptr);
}

void MainWindow::Destroy() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (self->m_hook) {
        LRESULT ret = 0;
        if (self->m_hook(hwnd, msg, wp, lp, &ret)) return ret;
    }
    try {
        return self->WndProc(msg, wp, lp);
    } catch (const std::exception& e) {
        console::complain("Glasswave", e.what());
        return DefWindowProcW(hwnd, msg, wp, lp);
    } catch (const winrt::hresult_error& e) {
        console::complain("Glasswave", pfc::stringcvt::string_utf8_from_wide(e.message().c_str()));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---- Composition visual tree ---------------------------------------------------------

void MainWindow::InitComposition() {
    // Allow this non-UWP window to use HostBackdropBrush (Windows 11 22000+, documented).
    BOOL enable = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
    // We draw the Win7 corners and border ourselves: no Win11 rounding, no accent border.
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    // The Win11 DWM shadow is replaced by ShadowFrame's Win7-measured shadow.
    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(m_hwnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(
        m_hwnd, FALSE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
            winrt::put_abi(m_target))));

    // The frame outline sits above the rounded clip, not inside it: clipping an anti-aliased
    // one-pixel line along the same curve multiplies the two coverages and the dark line
    // all but vanishes at the corners while the light line inside it stays.
    m_windowRoot = comp.CreateContainerVisual();
    m_windowRoot.RelativeSizeAdjustment(float2{1, 1});
    m_target.Root(m_windowRoot);
    m_root = comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_root);

    // Win7 windows have all four corners rounded (radius 6 at 96 DPI).
    m_clipGeometry = comp.CreateRoundedRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clipGeometry));

    m_glass = std::make_unique<aero::GlassMaterial>(WindowColorization(), aero::atlas::Available());
    auto children = m_root.Children();
    children.InsertAtTop(m_glass->Root());
    // The content base stops at the library boundary. The shared edge also surrounds
    // the now-playing pane, whose glass must remain visible through its dark tint.
    m_clientBackground = comp.CreateSpriteVisual();
    m_clientBackground.Brush(
        comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(255, 0xF1, 0xF7, 0xFD)));
    children.InsertAtTop(m_clientBackground);

    m_frameOutline = comp.CreateSpriteVisual();
    m_frameOutline.RelativeSizeAdjustment(float2{1, 1});
    for (auto* v : {&m_clientEdge, &m_navBar, &m_cmdBar, &m_contentTop, &m_transportBand, &m_capsule,
                    &m_cluster, &m_divider, &m_clientGlow, &m_title, &m_nowPlaying, &m_breadcrumb,
                    &m_navGroove, &m_time, &m_streamVisual})
        *v = comp.CreateSpriteVisual();
    m_crumbBar = std::make_unique<Breadcrumb>([this] { StartScrollAnimation(); });
    m_crumbBar->SetVisual(m_breadcrumb);
    for (auto& v : m_sideHighlight) {
        v = comp.CreateSpriteVisual();
        children.InsertAtTop(v);
    }
    m_windowRoot.Children().InsertAtTop(m_frameOutline);
    for (const auto& v : {m_clientEdge, m_navBar, m_navGroove, m_cmdBar, m_contentTop, m_transportBand})
        children.InsertAtTop(v);
    // Navigation pane, list and search box live in their own layer above the bars.
    m_contentLayer = comp.CreateContainerVisual();
    children.InsertAtTop(m_contentLayer);
    // WMP 12's library background (light blue with soft diagonal streaks) behind the list,
    // scaled to cover it; painted once, the compositor scales it on resize.
    m_libraryBg = comp.CreateSpriteVisual();
    if (win7::Resources::Get().Has(win7::Module::Wmploc, L"LIBRARY_BACKGROUND7.PNG")) {
        const D2D1_SIZE_U bs = win7::Resources::Get().Size(win7::Module::Wmploc, L"LIBRARY_BACKGROUND7.PNG");
        auto surface = host.CreateSurface(static_cast<int>(bs.width), static_cast<int>(bs.height));
        host.Draw(surface, 1.0f, [&](ID2D1DeviceContext* dc) {
            win7::DrawStretched(
                dc, win7::Module::Wmploc, L"LIBRARY_BACKGROUND7.PNG",
                D2D1::RectF(0, 0, static_cast<float>(bs.width), static_cast<float>(bs.height)));
        });
        auto brush = comp.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::UniformToFill);
        brush.HorizontalAlignmentRatio(0.0f);
        brush.VerticalAlignmentRatio(0.0f);
        m_libraryBg.Brush(brush);
    }
    m_contentLayer.Children().InsertAtBottom(m_libraryBg);
    for (const auto& v : {m_capsule, m_cluster, m_divider, m_clientGlow, m_title, m_nowPlaying, m_breadcrumb,
                          m_time, m_streamVisual})
        children.InsertAtTop(v);
    m_npLyric = comp.CreateSpriteVisual();
    children.InsertAtTop(m_npLyric);
    m_win7Art = wt::Available();
    m_aeroArt = aero::atlas::Available();
    m_divider.IsVisible(!m_win7Art); // part of the original capsule bitmap
    m_cluster.IsVisible(m_win7Art);

    m_detailHeader = comp.CreateSpriteVisual();
    m_detailHeader.IsVisible(false);
    m_contentLayer.Children().InsertAtTop(m_detailHeader);
    m_detailArtToken = ArtCache::Get().Subscribe([this] {
        if (m_detail.visible) PaintDetailHeader();
        RefreshNowPlayingArt();
    });
    m_radioToken = media::radio::Subscribe([this] { OnRadioChanged(); });
    m_nav = std::make_unique<NavPane>(m_contentLayer);
    m_nav->SetCategories(LoadNavCategories());
    m_nav->SetSmartPosition(static_cast<int>(cfg_navSmartAt.get()));
    m_nav->SetMoveCallback([this](const NavPane::Item& dragged, const NavPane::Item& target, bool after) {
        OnNavMove(dragged, target, after);
    });
    m_nav->SetStaticOrder(pfc::stringcvt::string_wide_from_utf8(cfg_navOrder.get()).get_ptr());
    m_nav->SetOrderCallback([](std::wstring text) {
        cfg_navOrder.set(pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr());
    });
    m_providerListener = std::make_unique<ProviderListener>(*this);
    RefreshProviderSections();
    m_discsToken = cd::AddDiscsListener([this] { OnDiscsChanged(); });
    OnDiscsChanged();
    SetContextActionProvider([this](metadb_handle_list_cref items) { return BuildContextActions(items); });
    m_list = std::make_unique<ListView>(m_contentLayer);
    m_list->onLink = [this](size_t group, size_t line) { OnAlbumLink(group, line); };
    m_list->onNearEnd = [this] {
        if (m_page) m_page->NearEnd();
    };
    m_list->onEmptyContext = [this](POINT screen) { ShowPageContextMenu(screen); };
    m_list->onSourceChanged = [this] { ApplyPendingScroll(); };
    m_list->onPresentationChanged = [this] {
        if (m_page) m_page->OnPresentationChanged();
    };
    m_list->onScrollAnim = [this] { StartScrollAnimation(); };
    m_list->onScrollHold = [this] { StartScrollRepeat(); };
    m_search = std::make_unique<SearchBox>(m_contentLayer);
    m_search->onChange = [this](const std::wstring& text) { OnSearchTextChanged(text); };
    m_search->onTokenRemoved = [this] { SetSearchScope({}); };
    // A playlist's songs played from its page: the playlist goes to Recently Played's Playlists.
    media::SetPlayQueueListener([this] {
        if (m_view.kind == View::Kind::RemotePlaylist)
            media::RecordRecentList(m_view.provider, m_view.remoteId, m_view.name);
        else if (m_view.kind == View::Kind::Playlist && !media::IsQueue(m_view.playlist))
            media::RecordRecentList({}, m_view.name, m_view.name);
    });
    m_search->onCompose = [this] { OnSearchTextChanged(m_search->Text()); };
    m_paneSlot = gfx::CompositionHost::Get().Compositor().CreateContainerVisual();
    m_paneSlot.RelativeSizeAdjustment(float2{1, 1});
    m_paneSlot.Comment(L"pane slot");
    m_root.Children().InsertAtTop(m_paneSlot);
    m_pane = std::make_unique<NowPlayingPane>(m_paneSlot);
    m_pane->onFrames = [this] { StartLyricsFrames(); };
    m_pane->onSeek = [](double seconds) {
        // Like Apple Music: playback continues from the clicked line.
        auto pc = playback_control::get();
        if (!pc->is_playing()) return;
        pc->playback_seek(seconds);
        if (pc->is_paused()) pc->pause(false);
    };
    m_pane->onLyricsMenu = [this](POINT screen) { ShowLyricsMenu(screen); };
    m_pane->onQueueShown = [this] { RefreshPaneQueue(); };
    ConnectPaneQueue();
    m_lyricsOpen = cfg_lyricsOpen.get() != 0;
    m_lyricsSettingsSignature = LyricsSettingsSignature();
}

void MainWindow::CreateWidgets() {
    auto add = [&](int id, Widget::Painter painter, Widget::Options opts) -> Widget* {
        auto w = std::make_unique<Widget>(id, std::move(painter), std::move(opts));
        m_root.Children().InsertAtTop(w->Visual());
        m_widgets.push_back(std::move(w));
        return m_widgets.back().get();
    };

    // Caption button cluster
    // Caption button cluster: Win7's own DWM images when aero.msstyles is present.
    const bool dwmArt = m_aeroArt;
    auto captionOptions = [&](bool close) {
        Widget::Options o;
        o.inactiveLayer = true;
        if (dwmArt) {
            o.glow = [close](ID2D1DeviceContext* dc, D2D1_SIZE_F s) {
                aero::atlas::CaptionGlow(dc, s, close);
            };
            o.glowOutset = aero::atlas::GlowOutset(close);
            o.glowOutsetInPixels = true;
        }
        return o;
    };
    add(
        kCapMin,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Minimize,
                                           paint::CaptionGlyph::Minimize, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Minimize, l, true, false);
        },
        captionOptions(false))
        ->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(
        kCapMax,
        [this, dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            const auto glyph =
                IsWindowMaximized() ? paint::CaptionGlyph::Restore : paint::CaptionGlyph::Maximize;
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Maximize, glyph, l);
            else
                paint::CaptionButton(dc, s, glyph, l, false, false);
        },
        captionOptions(false))
        ->onClick = [this] { ShowWindow(m_hwnd, IsWindowMaximized() ? SW_RESTORE : SW_MAXIMIZE); };
    add(
        kCapClose,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Close, paint::CaptionGlyph::Close, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Close, l, false, true);
        },
        captionOptions(true))
        ->onClick = [this] { PostMessageW(m_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); };

    // Navigation bar
    Widget::Options round;
    round.circular = true;
    auto* back = add(
        kBack,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art)
                wt::NavOrb(dc, s, l, true, m_historyPos > 0);
            else
                wp::NavArrow(dc, s, l, true, m_historyPos > 0);
        },
        round);
    back->onClick = [this] { GoBack(); };
    back->enabled = [this] { return m_historyPos > 0; };
    auto* forward = add(
        kForward,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool enabled = m_historyPos + 1 < m_history.size();
            if (m_win7Art)
                wt::NavOrb(dc, s, l, false, enabled);
            else
                wp::NavArrow(dc, s, l, false, enabled);
        },
        round);
    forward->onClick = [this] { GoForward(); };
    forward->enabled = [this] { return m_historyPos + 1 < m_history.size(); };

    // Command bar
    add(kOrganize, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Organize", true); },
        {})
        ->onClick = [this] {
        // Pressed while its menu is up, like WMP's drop-down buttons.
        Widget* w = WidgetById(kOrganize);
        if (w) w->SetPressed(true);
        ShowOrganizeMenu();
        if (w) w->SetPressed(false);
    };
    add(kAddFiles, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add files", false); },
        {})
        ->onClick = [this] {
        ActivateAddTarget();
        standard_commands::main_add_files();
    };
    add(kAddFolder,
        [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add folder", false); }, {})
        ->onClick = [this] {
        ActivateAddTarget();
        standard_commands::main_add_directory();
    };
    // The page's command at the command bar's right (an album's or playlist's Collect); Refresh
    // left the bar (F5 and the page's menu still refresh).
    add(kRefresh,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            wp::CommandButton(dc, s, l, L"", false); // plate only
            const std::wstring label = m_detail.saveLabel;
            auto layout = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height,
                                   DWRITE_TEXT_ALIGNMENT_CENTER);
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({0, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        if (m_page) m_page->SaveAction();
    };
    if (Widget* save = WidgetById(kRefresh)) save->enabled = [this] { return !m_detail.saveBusy; };
    // Play on a local album's or playlist's command bar (they have no detail band): the green
    // triangle and the label, as the band's Play draws them.
    add(kLocalPlay,
        [](auto* dc, D2D1_SIZE_F s, Layer l) {
            wp::CommandButton(dc, s, l, L"", false); // plate only
            auto layout = MakeText(Tr(L"Play"), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height);
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const float icon = 14.0f, gap = 5.0f;
            const float x = std::round((s.width - tm.width - icon - gap) / 2);
            const float top = std::round((s.height - icon) / 2);
            wp::GreenPlay(dc, {x, top, x + icon, top + icon});
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({x + icon + gap, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        if (!m_source) return;
        metadb_handle_list items; // the list as shown, filtered and sorted
        for (size_t row = 0; row < m_source->Count(); ++row)
            if (const auto track = m_source->Item(row); track.is_valid()) items.add_item(track);
        if (items.get_count()) media::PlayQueue(items, 0); // a playlist is recorded as played
    };

    // Download all, right of Collect: the page's songs go to Downloads.
    add(kDownloadAll,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            wp::CommandButton(dc, s, l, L"", false); // plate only
            auto layout = MakeText(m_detail.downloadLabel, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height,
                                   DWRITE_TEXT_ALIGNMENT_CENTER);
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({0, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        if (m_page) m_page->DownloadAction();
    };
    // Split buttons (WMP 12): one control; the main part acts at once, the arrow part opens a
    // menu. m_splitPart says which half the mouse is over.
    add(kViewOptions,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const float split = kViewOptionsSplit;
            if (!wp::SplitButtonPlate(dc, s, l, split, m_splitPart, m_splitMenu == kViewOptions))
                wp::CommandButton(dc, s, l, L"", true);
            wp::ViewOptionsGlyph(dc, {split, s.height},
                                 m_source ? m_source->GetViewStyle() : media::ViewStyle::Details);
        },
        {})
        ->onClick = [this] {
        if (m_splitPart == 1)
            RunSplitMenu(kViewOptions, [this] { ShowViewMenu(); });
        else
            CycleViewStyle();
    };
    add(kNewPlaylist,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (!wp::SplitButtonPlate(dc, s, l, m_newPlaylistSplit, m_splitPart, m_splitMenu == kNewPlaylist))
                wp::CommandButton(dc, s, l, L"", true);
            wp::CommandButton(dc, {m_newPlaylistSplit, s.height}, Layer::Normal, L"Create playlist",
                              false); // text only
        },
        {})
        ->onClick = [this] {
        if (m_splitPart == 1)
            RunSplitMenu(kNewPlaylist, [this] {
                POINT pt{m_layout.newPlaylist.left, m_layout.newPlaylist.bottom};
                ClientToScreen(m_hwnd, &pt);
                auto add = AddPlaylistActions();
                ShowActionMenu(m_hwnd, pt, add.front().children); // new playlist / smart / presets
            });
        else
            CreatePlaylist(false);
    };

    // Detail band: Follow / Following, or Play with a green triangle, centred in the button.
    add(kDetailAction,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const std::wstring label = m_detail.actionLabel.empty() ? Tr(L"Collect") : m_detail.actionLabel;
            wp::CommandButton(dc, s, l, L"", false); // plate only
            auto layout = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height);
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const bool play = m_detail.action == DetailInfo::Action::Play;
            const float icon = play ? 14.0f : 0.0f, gap = play ? 5.0f : 0.0f;
            const float x = std::round((s.width - tm.width - icon - gap) / 2);
            const float iconTop = std::round((s.height - icon) / 2);
            if (play) wp::GreenPlay(dc, {x, iconTop, x + icon, iconTop + icon});
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({x + icon + gap, 0}, layout.get(), ink.get()); // the layout centres it vertically
        },
        {})
        ->onClick = [this] { OnDetailAction(); };
    if (Widget* action = WidgetById(kDetailAction))
        action->enabled = [this] {
            return m_detail.action != DetailInfo::Action::None && !m_detail.actionBusy;
        };

    // The description's Show more / Show less link.
    add(kDetailBriefToggle,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const std::wstring label = Tr(m_detailBriefExpanded ? L"Show less" : L"Show more");
            auto layout = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height);
            layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(label.size())});
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(l == Layer::Normal ? 0x0066CC : 0x1E5BA8), ink.put());
            dc->DrawTextLayout({0, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        m_detailBriefExpanded = !m_detailBriefExpanded;
        m_detailBriefScroll = 0;
        RepaintBreadcrumb();
        RepaintDetailHeader();
    };

    // Seek bar (the band's top rule, as in WMP 12)
    add(kSeek,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const float progress =
                m_seeking ? m_seekPreview : (m_length > 0 ? static_cast<float>(m_position / m_length) : 0.0f);
            wp::SeekBar(dc, s, progress, l == Layer::Hover || l == Layer::Pressed, m_length > 0,
                        (m_layout.transport.top - m_layout.seek.top) / m_scale);
        },
        {});

    // Transport controls
    // A station playing (radio mode) has no shuffle: its "recommend less" takes the place where
    // it has one (NetEase's 不感兴趣), else the button is greyed.
    auto* shuffle = add(
        kShuffle,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (media::radio::Active()) {
                if (media::radio::CanDislike()) {
                    wt::Dislike(dc, l);
                    return;
                }
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), 0.35f),
                              nullptr);
                if (m_win7Art)
                    wt::Toggle(dc, Layer::Normal, true, false);
                else
                    paint::IconButton(dc, s, Layer::Normal, TransportGlyph::Shuffle, false, false);
                dc->PopLayer();
                return;
            }
            if (m_win7Art)
                wt::Toggle(dc, l, true, m_shuffle);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Shuffle, true, m_shuffle);
        },
        {});
    shuffle->onClick = [this] {
        const auto modes = media::transport::ShufflePressed({m_shuffle, static_cast<int>(m_repeat)});
        if (modes.shuffle != m_shuffle) SetShuffleRepeat(modes.shuffle, static_cast<Repeat>(modes.repeat));
    };
    shuffle->enabled = [] { return !media::radio::Active() || media::radio::CanDislike(); };
    // Repeat cycles off -> all -> one; "one" is the original icon with a small "1".
    add(kRepeat,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool on = m_repeat != Repeat::Off;
            if (m_win7Art)
                wt::Toggle(dc, l, false, on);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Repeat, true, on);
            // A station repeats one song or none: its button is repeat-one's, lit or not.
            if (m_repeat == Repeat::One || media::radio::Active()) {
                auto t = MakeText(L"1", 8.0f, DWRITE_FONT_WEIGHT_BOLD, 8, 10);
                com_ptr<ID2D1SolidColorBrush> halo, ink;
                dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), halo.put());
                dc->CreateSolidColorBrush(Hex(0x1C5DB2), ink.put());
                const D2D1_POINT_2F at{s.width - 8.5f, s.height - 12.0f};
                for (float dx = -0.75f; dx <= 0.75f; dx += 0.75f)
                    for (float dy = -0.75f; dy <= 0.75f; dy += 0.75f)
                        dc->DrawTextLayout({at.x + dx, at.y + dy}, t.get(), halo.get());
                dc->DrawTextLayout(at, t.get(), ink.get());
            }
        },
        {})
        ->onClick = [this] { CycleRepeat(); };
    add(kStop,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art)
                wt::ClusterButton(dc, ClusterRegion(m_layout.stop), wt::Button::Stop, l, m_playing);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Stop, m_playing, false);
        },
        {})
        ->onClick = [] { media::transport::Stop(); };
    // Like WMP 12, stop / previous / next are disabled while nothing plays: no hover, no click.
    add(kPrev,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool enabled = m_playing && media::radio::CanGoBack();
            if (m_win7Art)
                wt::ClusterButton(dc, ClusterRegion(m_layout.prev), wt::Button::Prev, l, enabled);
            else
                paint::Segment(dc, s, true, l, TransportGlyph::Previous, enabled);
        },
        {})
        ->onClick = [] { media::transport::Previous(); };
    add(kNext,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art)
                wt::ClusterButton(dc, ClusterRegion(m_layout.next), wt::Button::Next, l, m_playing);
            else
                paint::Segment(dc, s, false, l, TransportGlyph::Next, true);
        },
        {})
        ->onClick = [] { media::transport::Next(); };
    for (int id : {kStop, kNext})
        WidgetById(id)->enabled = [this] { return m_playing; };
    WidgetById(kPrev)->enabled = [this] { return m_playing && media::radio::CanGoBack(); };

    Widget::Options orb;
    orb.circular = true;
    if (m_win7Art) {
        // WMP 12: the orb brightens and a glow ring fades in around it (no pulsing).
        orb.haloFactor = wt::kGlowFactor;
        orb.haloPulse = false;
        orb.halo = [](ID2D1DeviceContext* dc, float size) { wt::GlowRing(dc, size); };
    } else {
        orb.haloFactor = 1.6f;
        orb.halo = [](ID2D1DeviceContext* dc, float size) { paint::OrbHalo(dc, size); };
    }
    add(
        kPlay,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool showPause = m_playing && !m_paused;
            if (m_win7Art)
                wt::Orb(dc, l, showPause);
            else
                paint::PlayOrb(dc, s.width, l, showPause ? TransportGlyph::Pause : TransportGlyph::Play);
        },
        orb)
        ->onClick = [this] {
        media::transport::PlayPause([this] {
            if (!m_list || !m_source || m_source->Focus() == SIZE_MAX || m_source->Focus() >= m_source->Count())
                return false;
            m_source->Activate(m_source->Focus()); // like WMP: play what is selected
            return true;
        });
    };

    add(kMute,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art && m_layout.transportLevel >= 2) {
                // Folded: the speaker's half of WMP 12's split button, as in the mini player.
                // Lit (4628) or pressed (4627) by itself over the plain plate both halves share
                // (4630, m_splitPlate), which shows while either half is under the pointer.
                if (l != Layer::Normal)
                    win7::DrawPartTo(dc, win7::Module::Wmploc, l == Layer::Pressed ? L"#4627" : L"#4628",
                                     {0, 0, 25, 25}, {0, 0, s.width, s.height});
                const int speaker = m_muted            ? wt::kMuted
                                    : m_volume <= 0.0f ? wt::kVolume0
                                    : m_volume < 0.67f ? wt::kVolume1
                                                       : wt::kVolume2;
                win7::DrawStretched(dc, win7::Module::Wmploc, L"#" + std::to_wstring(speaker),
                                    {0, 0, s.width, s.height});
            } else if (m_win7Art)
                wt::Mute(dc, l, m_muted, m_volume);
            else
                paint::IconButton(dc, s, l, m_muted ? TransportGlyph::Mute : TransportGlyph::Volume, true,
                                  false);
        },
        {})
        ->onClick = [] { playback_control::get()->volume_mute_toggle(); };
    add(kVolume,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art)
                wt::Volume(dc, l, m_volume);
            else
                paint::VolumeSlider(dc, s, m_volume, l);
        },
        {});

    // Lyrics pane toggle, bottom-right of the transport band.
    add(kLyricsToggle,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            paint::ButtonPlate(dc, s, l, m_lyricsOpen);
            DrawLyricsGlyph(
                dc, s, paint::TransportGlyphBrush(dc, s.height / 2 - 7, s.height / 2 + 7, true, l).get());
        },
        {})
        ->onClick = [this] { ToggleLyrics(); };

    // A narrow window folds the volume slider behind an arrow beside the speaker, as WMP 12
    // does: the arrow (its half of WMP's split button when lit) opens the flyout.
    add(kVolArrow,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const float k = s.height / 25.0f;
            if (m_win7Art) { // the arrow's half of the split button, held while the flyout is open
                if (l != Layer::Normal || m_volFlyoutOpen)
                    wt::SplitPlate(dc, l == Layer::Hover && !m_volFlyoutOpen ? 4628 : 4627, true, 0, k);
                D2D1_MATRIX_3X2_F base;
                dc->GetTransform(&base);
                dc->SetTransform(D2D1::Matrix3x2F::Translation(-25 * k, 0) * base);
                wt::SplitArrow(dc, k);
                dc->SetTransform(base);
                return;
            }
            paint::ButtonPlate(dc, s, l, m_volFlyoutOpen);
            com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            com_ptr<ID2D1PathGeometry> path;
            com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
            const float cx = s.width / 2, cy = s.height / 2 + 0.5f * k;
            sink->BeginFigure({cx - 3.5f * k, cy - 2 * k}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine({cx + 3.5f * k, cy - 2 * k});
            sink->AddLine({cx, cy + 2 * k});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            const bool lit = l != Layer::Normal || m_volFlyoutOpen;
            dc->FillGeometry(path.get(), wt::RowBands(dc, cy - 2 * k, cy + 2 * k,
                                                      {Hex(lit ? 0xFFFFFF : 0x7FA9E6), Hex(lit ? 0xE4EEFA : 0x2D62B8)},
                                                      true)
                                             .get());
        },
        {})
        ->onClick = [this] { SetVolumeFlyout(!m_volFlyoutOpen); };

    // The command bar's buttons that no longer fit go behind a » at their end (WMP 12).
    add(kCmdMore, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"\x00BB", false); }, {})
        ->onClick = [this] {
        std::vector<MenuAction> actions;
        for (const int id : m_cmdHidden) {
            const wchar_t* label = id == kAddFiles ? L"Add files" : id == kAddFolder ? L"Add folder" : L"Create playlist";
            actions.push_back({Tr(label), [this, id] {
                                   m_splitPart = 0; // the button's main action, not its menu
                                   if (Widget* w = WidgetById(id); w && w->onClick) w->onClick();
                               }});
        }
        POINT pt{m_layout.cmdMore.left, m_layout.cmdMore.bottom};
        ClientToScreen(m_hwnd, &pt);
        ShowActionMenu(m_hwnd, pt, actions);
    };

    // The flyout lies over everything in the band; the slider widget moves into it on top.
    {
        auto& comp = gfx::CompositionHost::Get().Compositor();
        m_volFlyoutGlass = comp.CreateSpriteVisual();
        m_volFlyout = comp.CreateSpriteVisual();
        // Last in the list too, so that it is hit first where the flyout covers the speaker.
        const auto at = std::find_if(m_widgets.begin(), m_widgets.end(), [](const auto& w) { return w->Id() == kVolume; });
        std::rotate(at, at + 1, m_widgets.end());
        Widget* volume = m_widgets.back().get();
        m_root.Children().Remove(volume->Visual());
        m_root.Children().InsertAtTop(volume->Visual());
        m_root.Children().InsertBelow(m_volFlyoutGlass, volume->Visual());
        m_root.Children().InsertBelow(m_volFlyout, volume->Visual());
        m_volFlyoutGlass.IsVisible(false);
        m_volFlyout.IsVisible(false);
        // The split button's shared plate, under both halves.
        m_splitPlate = comp.CreateSpriteVisual();
        m_splitPlate.Opacity(0.0f);
        m_root.Children().InsertBelow(m_splitPlate, WidgetById(kMute)->Visual());
    }
}

void MainWindow::SetVolumeFlyout(bool open) {
    open = open && m_layout.transportLevel >= 2;
    if (open == m_volFlyoutOpen) return;
    m_volFlyoutOpen = open;
    for (int id : {kVolArrow, kMute})
        if (auto* w = WidgetById(id)) w->Invalidate();
    UpdateLayout(); // the slider moves into the flyout, or leaves with it
    UpdateSplitPlate();
}

// The split button's plain plate (4630) shows while either half is under the pointer or
// held, or the flyout is open, and fades as in the mini player.
void MainWindow::UpdateSplitPlate() {
    if (!m_splitPlate) return;
    const auto isSplit = [](const Widget* w) { return w && (w->Id() == kMute || w->Id() == kVolArrow); };
    const bool on = m_win7Art && m_layout.transportLevel >= 2 &&
                    (m_volFlyoutOpen || isSplit(m_hover) || isSplit(m_pressed));
    if (on == m_splitPlateShown) return;
    m_splitPlateShown = on;
    gfx::CompositionHost::Get().AnimateOpacity(m_splitPlate, on ? 1.0f : 0.0f, on ? 160 : 260);
}

// ---- Layout and painting -------------------------------------------------------------

void MainWindow::UpdateLayout() {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    const bool max = IsWindowMaximized();
    Layout& L = m_layout;
    const int line = LinePx();

    // Frame and client area
    L.border = max ? 0 : PxI(m::kResizeBorder);
    const int capH = PxI(max ? m::kCaptionHeightMax : m::kCaptionHeight);
    L.caption = {0, 0, W, capH};
    L.client = {L.border, capH, W - L.border, H - L.border};
    // The lyrics pane takes the right of the window: glass joined to the client area.
    L.lyricsPane = {};
    if (m_lyricsOpen) {
        int paneW = std::min(PxI(kLyricsPaneWidth), std::max(0, (W - 2 * L.border) / 2));
        if (m_paneReveal >= 0) paneW = std::min(paneW, m_paneReveal);
        L.lyricsPane = {W - L.border - paneW, capH, W - L.border, H - L.border};
        L.client.right = L.lyricsPane.left;
    }
    // Light + dark client edge lines, one device pixel each (DWM frame bitmap, 1:1).
    L.clientEdge = {L.client.left - 2, L.client.top - 2, L.client.right + 2, L.client.bottom + 2};
    if (m_lyricsOpen) L.clientEdge.right = L.lyricsPane.right + 2; // one edge around client and pane
    L.navBar = {L.client.left, L.client.top, L.client.right, L.client.top + PxI(NavBarHeight())};
    L.cmdBar = {L.client.left, L.navBar.bottom, L.client.right, L.navBar.bottom + PxI(m::kCommandBarHeight)};
    L.transport = {L.client.left, L.client.bottom - PxI(TransportHeight()), L.client.right, L.client.bottom};
    L.content = {L.client.left, L.cmdBar.bottom, L.client.right, L.transport.top};

    // Caption buttons hang from the top edge (flush when maximized).
    // With the DWM artwork: Win7 sizes measured at 150 % (43 / 41 / 73 x 29 px), hanging one
    // device pixel below the frame's outer line.
    const bool dwm = m_aeroArt;
    const int capRight = W - (max ? PxI(2) : PxI(dwm ? 20.0f / 3 : m::kCapBtnRightInset));
    const int capTop = max ? 0 : (dwm ? line : PxI(m::kCapBtnTopInset));
    const int bh = PxI(dwm ? 29.0f / 1.5f : m::kCapBtnH);
    L.capClose = {capRight - PxI(dwm ? 73.0f / 1.5f : m::kCapBtnCloseW), capTop, capRight, capTop + bh};
    L.capMax = {L.capClose.left - PxI(dwm ? 41.0f / 1.5f : m::kCapBtnMaxW), capTop, L.capClose.left,
                capTop + bh};
    L.capMin = {L.capMax.left - PxI(dwm ? 43.0f / 1.5f : m::kCapBtnMinW), capTop, L.capMax.left, capTop + bh};

    // Icon and caption text (the title visual carries a glow margin on every side)
    const int cy = max ? capH / 2 : (capH + PxI(2)) / 2;
    const int iconSz = PxI(m::kIconSize);
    const int iconLeft = L.border + PxI(max ? 4.0f : 1.0f);
    const int pad = PxI(paint::kGlowPadding);
    L.icon = {iconLeft, cy - iconSz / 2, iconLeft + iconSz, cy - iconSz / 2 + iconSz};
    L.title = {iconLeft - pad, cy - PxI(12) - pad, std::max<int>(iconLeft, L.capMin.left - PxI(4)),
               cy + PxI(12) + pad};

    // Navigation bar: back / forward orbs and the breadcrumb
    const int nbcy = (L.navBar.top + L.navBar.bottom) / 2;
    if (m_win7Art) {
        // WMP 12 orbs in their recess, 8 DIP from the client edge, scaled to the command bar's
        // button height; the orbs sit one bitmap pixel inside the recess.
        const float k = wt::kNavOrbShown / wt::kNavOrb;
        const int gw = PxI(wt::kNavGrooveSize.width * k), gh = PxI(wt::kNavGrooveSize.height * k);
        const int gx = L.client.left + PxI(8), gy = nbcy - gh / 2;
        L.navGroove = {gx, gy, gx + gw, gy + gh};
        const int orb = PxI(wt::kNavOrbShown), inset = PxI(k);
        L.back = {gx + inset, gy + inset, gx + inset + orb, gy + inset + orb};
        L.forward = {L.back.right, L.back.top, L.back.right + orb, L.back.bottom};
        L.breadcrumb = {L.navGroove.right + PxI(6), L.navBar.top, L.client.right - PxI(8),
                        L.navBar.bottom - line};
    } else {
        const int orb = PxI(24);
        L.back = CenteredRect(L.client.left + PxI(18), nbcy, orb, orb);
        L.forward = CenteredRect(L.client.left + PxI(44), nbcy, orb, orb);
        L.breadcrumb = {L.client.left + PxI(64), L.navBar.top, L.client.right - PxI(8),
                        L.navBar.bottom - line};
    }

    // Command bar
    const int cbcy = (L.cmdBar.top + L.cmdBar.bottom) / 2 - line;
    const int bh2 = PxI(23);
    int x = L.client.left + PxI(6);
    auto button = [&](float wDip) {
        RECT r{x, cbcy - bh2 / 2, x + PxI(wDip), cbcy - bh2 / 2 + bh2};
        x = r.right + PxI(2);
        return r;
    };
    // Widths follow the text (6 DIP padding each side, 12 more for a drop-down arrow).
    auto fit = [](const wchar_t* text, bool drop) {
        auto layout = MakeText(Tr(text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 400, 20);
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        return std::ceil(tm.widthIncludingTrailingWhitespace) + 12 + (drop ? 12.0f : 0.0f);
    };
    L.organize = button(fit(L"Organize", true));
    L.addFiles = button(fit(L"Add files", false));
    L.addFolder = button(fit(L"Add folder", false));
    // Split buttons (WMP 12): the main part acts at once, the arrow part opens a menu.
    m_newPlaylistSplit = fit(L"Create playlist", false);
    L.newPlaylist = button(m_newPlaylistSplit + kSplitArrow);
    // The search box gives way first in a narrow window: 230 DIP down to 140.
    const float clientDip = (L.client.right - L.client.left) / m_scale;
    const int searchW = PxI(std::clamp(clientDip * 0.32f, 140.0f, 230.0f));
    L.search = {L.client.right - PxI(8) - searchW, cbcy - PxI(11), L.client.right - PxI(8), cbcy + PxI(11)};
    // View options (WMP: icon / expanded tile / details) left of the search box, library only.
    if (m_source && ViewStyles() > 1) {
        const int right = L.search.left - PxI(8);
        L.viewOptions = {right - PxI(kViewOptionsSplit + kSplitArrow), cbcy - PxI(11), right, cbcy + PxI(11)};
    } else {
        L.viewOptions = {};
    }
    {
        // The page's command (Collect) sits left of the view options (or the search box), only
        // on pages that have one. Its width follows the centred label.
        const DetailInfo detail = m_page ? m_page->Detail() : DetailInfo{};
        int right = (L.viewOptions.right > L.viewOptions.left ? L.viewOptions.left : L.search.left) - PxI(8);
        L.download = {};
        if (!detail.downloadLabel.empty()) {
            const float w = fit(detail.downloadLabel.c_str(), false);
            L.download = {right - PxI(w), cbcy - PxI(11), right, cbcy + PxI(11)};
            right = L.download.left - PxI(2);
        }
        if (!detail.saveLabel.empty()) {
            const float w = fit(detail.saveLabel.c_str(), false);
            L.refresh = {right - PxI(w), cbcy - PxI(11), right, cbcy + PxI(11)};
            right = L.refresh.left - PxI(2);
        } else {
            L.refresh = {};
        }
        L.localPlay = {};
        if (LocalPlayShown()) {
            const float w = fit(L"Play", false) + 14 + 5; // the triangle and its gap
            L.localPlay = {right - PxI(w), cbcy - PxI(11), right, cbcy + PxI(11)};
        }
    }
    // Then the buttons on the left that would run into the right group go, last first, behind
    // a » after the ones that stay (WMP 12); Organize always stays.
    {
        LONG rightGroup = L.search.left;
        for (const RECT* r : {&L.viewOptions, &L.refresh, &L.download, &L.localPlay})
            if (r->right > r->left) rightGroup = std::min(rightGroup, r->left);
        const int moreW = PxI(22);
        m_cmdHidden.clear();
        RECT* order[] = {&L.addFiles, &L.addFolder, &L.newPlaylist};
        const int ids[] = {kAddFiles, kAddFolder, kNewPlaylist};
        int kept = 3; // how many of the three stay: all, or as many as leave room for the »
        if (order[2]->right + PxI(8) > rightGroup) {
            kept = 2;
            while (kept > 0 && order[kept - 1]->right + PxI(2) + moreW + PxI(8) > rightGroup) --kept;
        }
        for (int i = kept; i < 3; ++i) {
            m_cmdHidden.push_back(ids[i]);
            *order[i] = {};
        }
        if (m_cmdHidden.empty()) {
            L.cmdMore = {};
        } else {
            LONG lastRight = L.organize.right;
            for (const RECT* r : order)
                if (r->right > r->left) lastRight = r->right;
            L.cmdMore = {lastRight + PxI(2), cbcy - bh2 / 2, lastRight + PxI(2) + moreW, cbcy - bh2 / 2 + bh2};
        }
    }

    // Content: navigation pane + list (a detail band sits above artist, album and playlist pages).
    // The navigation pane narrows with the window, from 190 DIP to 150.
    const int navW = PxI(std::clamp(clientDip * 0.3f, 150.0f, kNavPaneWidth));
    L.navPane = {L.content.left, L.content.top, L.content.left + navW, L.content.bottom - PxI(8)};
    L.list = {L.navPane.right, L.content.top, L.content.right, L.content.bottom};
    L.detailHeader = {};
    L.detailAction = {};
    L.detailBriefToggle = {};
    L.detailBriefArea = {};
    L.detailBriefBar = {};
    m_detailBriefOverflow = false;
    m_detailBriefScrollable = false;
    m_detailBriefViewH = 0;
    m_detailBriefContentH = 0;
    m_detailBriefTextW = 0;
    m_detail = m_page ? m_page->Detail() : DetailInfo{};
    if (m_detail.visible) {
        L.detailHeader = {L.navPane.right, L.content.top, L.content.right,
                          std::min<LONG>(L.content.top + PxI(kDetailHeaderHeight), L.content.bottom)};
        if (m_detail.action != DetailInfo::Action::None) {
            const int right = L.detailHeader.right - PxI(18);
            L.detailAction = {right - PxI(kDetailActionWidth), L.detailHeader.top + PxI(22), right,
                              L.detailHeader.top + PxI(22 + kDetailActionHeight)};
        }
        // The facts and the tagline come under the stats line, the description under them.
        {
            const float textW = DetailHeaderTextWidth();
            m_detailBriefTop = DetailTextTop() + DetailFactsHeight(textW) + DetailTaglineHeight(textW);
            L.detailHeader.bottom =
                std::min<LONG>(L.content.top + std::max<int>(PxI(kDetailHeaderHeight), PxI(m_detailBriefTop + 12)),
                               L.content.bottom);
        }
        const float briefTop = m_detailBriefTop;
        // The description is limited to three lines until expanded. Expanded text has its own
        // maximum height and scrollbar, so the song list below always stays reachable.
        if (!m_detail.brief.empty()) {
            const float baseW = DetailHeaderTextWidth();
            const float textX = 18 + kDetailArt + 18;
            const float bandSpace = (L.content.bottom - L.content.top) / m_scale;
            const float maxView =
                std::min(kDetailBriefMaxHeight, std::max(96.0f, bandSpace - briefTop - 12 - 18 - 8));
            auto measure = [&](float width) {
                auto layout =
                    MakeWrappedText(m_detail.brief, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, width, 4000.0f);
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
                return metrics.height;
            };
            const float fullBase = measure(baseW);
            const float collapsed = std::min(fullBase, 48.0f);
            m_detailBriefOverflow = fullBase > collapsed + 1.0f;
            float textW = baseW;
            float contentH = fullBase;
            float used = collapsed;
            bool scrollable = false;
            if (m_detailBriefExpanded) {
                if (fullBase > maxView + 1.0f) {
                    scrollable = true;
                    textW = std::max(80.0f, baseW - kDetailScrollWidth - 6);
                    contentH = measure(textW);
                    used = std::min(contentH, maxView);
                } else {
                    used = fullBase;
                }
            }
            // Collapsed, Show more follows the last line's text instead of standing on a line of
            // its own under a blank one (owner, 2026-11-14): blank lines close up and the last
            // line is cut with an ellipsis where the link needs the room.
            m_detailBriefCollapsed.clear();
            D2D1_POINT_2F inlineLink{-1, -1};
            if (m_detailBriefOverflow && !m_detailBriefExpanded) {
                auto link = MakeText(Tr(L"Show more"), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 200.0f, 18.0f);
                DWRITE_TEXT_METRICS lm{};
                link->GetMetrics(&lm);
                const float linkW = std::ceil(lm.widthIncludingTrailingWhitespace) + 4.0f;
                std::wstring text;
                for (size_t i = 0; i < m_detail.brief.size(); ++i) { // blank lines closed up
                    const wchar_t c = m_detail.brief[i];
                    if (c == L'\r') continue;
                    if (c == L'\n' && (text.empty() || text.back() == L'\n')) continue;
                    text += c;
                }
                while (!text.empty() && iswspace(text.back())) text.pop_back();
                const auto fits = [&](const std::wstring& t, D2D1_POINT_2F* end) {
                    auto layout = MakeWrappedText(t, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 4000.0f);
                    DWRITE_TEXT_METRICS tm{};
                    layout->GetMetrics(&tm);
                    if (tm.lineCount > 3) return false;
                    DWRITE_HIT_TEST_METRICS hm{};
                    float x = 0, y = 0;
                    layout->HitTestTextPosition(static_cast<UINT32>(t.size() - 1), TRUE, &x, &y, &hm);
                    // The last line's top from the line metrics: the caret's y put the link a
                    // line too low (owner, 2026-11-14).
                    UINT32 count = 0;
                    layout->GetLineMetrics(nullptr, 0, &count);
                    std::vector<DWRITE_LINE_METRICS> lines(count);
                    layout->GetLineMetrics(lines.data(), count, &count);
                    float top = 0;
                    for (UINT32 i = 0; i + 1 < count; ++i) top += lines[i].height;
                    *end = {x, top};
                    return x + 6 + linkW <= textW;
                };
                D2D1_POINT_2F end{};
                if (!text.empty() && fits(text, &end)) {
                    m_detailBriefCollapsed = text;
                } else {
                    // The first three lines' text, then shortened until "…" and the link fit.
                    auto layout = MakeWrappedText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 4000.0f);
                    UINT32 count = 0;
                    layout->GetLineMetrics(nullptr, 0, &count);
                    std::vector<DWRITE_LINE_METRICS> lines(count);
                    layout->GetLineMetrics(lines.data(), count, &count);
                    size_t cut = 0;
                    for (UINT32 i = 0; i < std::min<UINT32>(count, 3); ++i) cut += lines[i].length;
                    cut = std::min(cut, text.size());
                    for (int tries = 0; cut > 0 && tries < 400; ++tries, --cut) {
                        std::wstring t = text.substr(0, cut);
                        while (!t.empty() && iswspace(t.back())) t.pop_back();
                        t += L'\x2026';
                        if (fits(t, &end)) {
                            m_detailBriefCollapsed = std::move(t);
                            break;
                        }
                    }
                }
                if (!m_detailBriefCollapsed.empty()) {
                    inlineLink = {end.x + 6, end.y};
                    used = std::min(used, end.y + 16.0f);
                }
            }
            float needed = briefTop + used + 12;
            if (m_detailBriefOverflow && inlineLink.x < 0) needed += 18;
            if (m_detailBriefExpanded) needed += 6; // pixel rounding must not clip the last line
            L.detailHeader.bottom = std::min<LONG>(
                L.content.top + std::max<int>(PxI(kDetailHeaderHeight), PxI(needed)), L.content.bottom);
            m_detailBriefScrollable = scrollable;
            m_detailBriefViewH = used;
            m_detailBriefContentH = contentH;
            m_detailBriefTextW = textW;
            const float maxScroll = std::max(0.0f, contentH - used);
            m_detailBriefScroll = scrollable ? std::clamp(m_detailBriefScroll, 0.0f, maxScroll) : 0.0f;
            const int briefX = L.detailHeader.left + PxI(textX);
            const int briefY = L.detailHeader.top + PxI(briefTop);
            L.detailBriefArea = {briefX, briefY, briefX + PxI(textW), briefY + PxI(used)};
            if (scrollable) {
                const int barLeft = L.detailHeader.left + PxI(textX + baseW - kDetailScrollWidth);
                L.detailBriefBar = {barLeft, briefY, barLeft + PxI(kDetailScrollWidth), briefY + PxI(used)};
                L.detailBriefArea.right = barLeft - PxI(4);
            }
            if (m_detailBriefOverflow) {
                const std::wstring label = Tr(m_detailBriefExpanded ? L"Show less" : L"Show more");
                auto link = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 200.0f, 18.0f);
                DWRITE_TEXT_METRICS linkMetrics{};
                link->GetMetrics(&linkMetrics);
                const bool inLine = inlineLink.x >= 0;
                const int linkX = L.detailHeader.left + PxI(textX + (inLine ? inlineLink.x : 0));
                const int y = L.detailHeader.top + PxI(briefTop + (inLine ? inlineLink.y : used + 2));
                const int w = PxI(std::ceil(linkMetrics.widthIncludingTrailingWhitespace) + 4.0f);
                L.detailBriefToggle = {linkX, y, linkX + w, y + PxI(18)};
            }
        }
        L.list.top = L.detailHeader.bottom;
    }
    L.tabs = {};
    if (m_page && !m_page->Tabs().empty()) {
        L.tabs = {L.list.left, L.list.top, L.list.right, std::min<LONG>(L.list.top + PxI(kTabBand), L.list.bottom)};
        L.list.top = L.tabs.bottom;
    }

    // Transport band: seek rule on top, capsule centered on the orb below it.
    // WMP 12: the seek bar runs the full client width right under the band's top rule;
    // the widget is taller than the bar for the hover thumb and an easier grab.
    L.seek = {L.client.left, L.transport.top - PxI(6), L.client.right, L.transport.top + PxI(9)};
    const int tcx = (L.transport.left + L.transport.right) / 2;
    // A narrow window gives way in WMP 12's order: shuffle and repeat leave the capsule, then
    // the volume slider folds behind an arrow beside the speaker (a flyout, as in the mini
    // player), then stop leaves. The capsule shortens to what it holds and stays centred on
    // the orb; the time moves to the left of the capsule when it no longer fits on the right.
    // Ends of the capsule per level, DIP from the orb's centre.
    const auto span = [&](int level) -> std::pair<float, float> {
        if (m_win7Art) {
            const float o = wt::kOrbRect.left + (wt::kOrbRect.right - wt::kOrbRect.left) / 2; // orb centre, cluster x
            const float left = level == 0 ? wt::kCapsuleVisible.left : level < 3 ? -8.0f : 28.0f;
            const float right = level < 2 ? wt::kCapsuleVisible.right : wt::kMuteRect.right + 14 + 9;
            return {left - o, right - o};
        }
        const float ib = m::kIconButtonW / 2;
        const float left = level == 0 ? m::kShuffleX - ib - 10 : level < 3 ? m::kStopX - ib - 10 : -m::kSegmentReach - 8;
        const float right = level < 2 ? m::kSliderRight + 10 : m::kVolumeX + ib + 16 + 10;
        return {left, right};
    };
    // Nothing may overlap (owner, 2026-11-13: WMP's own narrow layout lets the time run into
    // the capsule): left to right the song's text (at least 110 DIP), the time when it stands
    // left, the capsule, the time when it stands right, the lyrics toggle, 12 DIP apart. The
    // capsule stays centred on the band while some step allows it; else it moves off centre
    // just enough, and only then do further steps give way.
    const int toggleLeft = L.client.right - PxI(10 + 28);
    // The time box is as wide as its text can get for this song ("4:56 / 4:56"; the digits
    // are tabular), not a fixed 110 DIP: the stream block starts right after it.
    if (m_timeMeasuredFor != m_length) {
        m_timeMeasuredFor = m_length;
        const std::wstring longest = m_length > 0 ? FormatTime(m_length) + L" / " + FormatTime(m_length) : L"0:00";
        DWRITE_TEXT_METRICS tm{};
        MakeText(longest, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 400, 20)->GetMetrics(&tm);
        m_timeTextW = std::ceil(tm.widthIncludingTrailingWhitespace) + 4;
    }
    const int timeW = PxI(std::max(40.0f, m_timeTextW)), gap = PxI(12), textLeft = L.client.left + PxI(10);
    const int nowMin = PxI(110);
    struct Fit {
        int level;
        bool timeLeft;
        int lo, hi; // range of the orb centre that fits
    };
    const auto fitTransport = [&](int lv, bool left) {
        const auto [sl, sr] = span(lv);
        const int minLeft = textLeft + nowMin + gap + (left ? timeW + gap : 0);
        const int maxRight = toggleLeft - gap - (left ? 0 : timeW + gap);
        return Fit{lv, left, minLeft - PxI(sl), maxRight - PxI(sr)};
    };
    std::optional<Fit> chosen;
    for (const bool centred : {true, false}) {
        for (int lv = 0; lv <= 3 && !chosen; ++lv)
            for (const bool left : {false, true}) {
                const Fit f = fitTransport(lv, left);
                if (f.lo <= f.hi && (!centred || (tcx >= f.lo && tcx <= f.hi))) {
                    chosen = f;
                    break;
                }
            }
        if (chosen) break;
    }
    // Narrower than the minimum allows (it should not be): the capsule keeps clear of the
    // time and the toggle, and the song's text gets what is left.
    const Fit placed = chosen ? *chosen : fitTransport(3, false);
    const int orb = chosen ? std::clamp(tcx, placed.lo, placed.hi) : placed.hi;
    const int level = placed.level;
    const bool timeLeft = placed.timeLeft;
    L.transportLevel = level;
    const bool timeMoved = L.timeLeft != timeLeft;
    L.timeLeft = timeLeft;
    {
        const auto [sl, sr] = span(level);
        const float o = m_win7Art ? wt::kOrbRect.left + (wt::kOrbRect.right - wt::kOrbRect.left) / 2 : 0.0f;
        L.capsuleLeft = sl + o;
        L.capsuleRight = sr + o;
    }
    if (m_win7Art) {
        // Original bitmaps, placed relative to the cluster as in WMP 12 (orb center = band center,
        // unless a narrow window moved it).
        L.cluster = {orb - PxI(108), L.transport.top + PxI(wt::kClusterTop)};
        L.capsule = ClusterRect(wt::kCapsuleRect);
        L.play = ClusterRect(wt::kOrbRect);
        L.prev = ClusterRect(wt::kPrevRect);
        L.next = ClusterRect(wt::kNextRect);
        L.stop = ClusterRect(wt::kStopRect);
        L.shuffle = ClusterRect(wt::kShuffleRect);
        L.repeat = ClusterRect(wt::kRepeatRect);
        L.mute = ClusterRect(wt::kMuteRect);
        L.volume = ClusterRect(wt::kVolumeRect);
        L.divider = {};
    } else {
        const int tcy = (L.transport.top + PxI(6) + L.transport.bottom) / 2;
        auto at = [&](float dx) { return orb + PxI(dx); };
        L.capsule = CenteredRect(orb, tcy, PxI(m::kCapsuleWidth), PxI(m::kCapsuleHeight));
        const int orbPx = PxI(m::kOrbDiameter + 2.0f);
        L.play = CenteredRect(orb, tcy, orbPx, orbPx);
        const int segH = PxI(m::kSegmentHeight);
        L.prev = {at(-m::kSegmentReach), tcy - segH / 2, orb, tcy - segH / 2 + segH};
        L.next = {orb, tcy - segH / 2, at(m::kSegmentReach), tcy - segH / 2 + segH};
        const int ibw = PxI(m::kIconButtonW), ibh = PxI(m::kIconButtonH);
        L.shuffle = CenteredRect(at(m::kShuffleX), tcy, ibw, ibh);
        L.repeat = CenteredRect(at(m::kRepeatX), tcy, ibw, ibh);
        L.stop = CenteredRect(at(m::kStopX), tcy, ibw, ibh);
        L.mute = CenteredRect(at(m::kVolumeX), tcy, ibw, ibh);
        L.volume = {at(m::kSliderLeft), tcy - ibh / 2, at(m::kSliderRight), tcy - ibh / 2 + ibh};
        L.divider = CenteredRect(at(m::kDividerX), tcy, PxI(4), PxI(16));
        L.capsule.left = at(L.capsuleLeft);
        L.capsule.right = at(L.capsuleRight);
    }
    const int capL = m_win7Art ? L.cluster.x + PxI(L.capsuleLeft) : L.capsule.left;
    const int capR = m_win7Art ? L.cluster.x + PxI(L.capsuleRight) : L.capsule.right;
    if (level >= 1) L.shuffle = L.repeat = {};
    if (level >= 3) L.stop = {};
    L.volArrow = L.volFlyout = {};
    if (level >= 2) {
        // The speaker keeps its place; the arrow follows it, 14 DIP wide (WMP's split button).
        L.volArrow = {L.mute.right, L.mute.top, L.mute.right + PxI(14), L.mute.top + PxI(25)};
        if (!m_win7Art) L.volArrow.bottom = L.mute.bottom;
        // The flyout over the button, its right edge on the arrow's (WMP 12): a 5 DIP frame
        // round the slider.
        const int sw = PxI(80), sh = PxI(23), fw = sw + PxI(16), fh = sh + PxI(12);
        const int mid = (L.mute.top + L.mute.bottom) / 2;
        const int right = std::min<LONG>(L.volArrow.right, L.client.right - PxI(2));
        L.volFlyout = {right - fw, mid - fh / 2, right, mid - fh / 2 + fh};
        L.volume = m_volFlyoutOpen ? RECT{L.volFlyout.left + PxI(8), L.volFlyout.top + PxI(6),
                                          L.volFlyout.left + PxI(8) + sw, L.volFlyout.top + PxI(6) + sh}
                                   : RECT{};
    }
    if (level < 2) m_volFlyoutOpen = false;
    // Time right of the controls (after the volume) when it fits there, else left of them;
    // stream format and output device right-aligned at the band's end, where there is room.
    {
        const int tcyMid = (L.transport.top + PxI(4) + L.transport.bottom) / 2;
        L.lyricsToggle = {toggleLeft, tcyMid - PxI(12), L.client.right - PxI(10), tcyMid + PxI(12)};
        const int timeX = timeLeft ? capL - PxI(12) - timeW : capR + PxI(12);
        L.time = {timeX, tcyMid - PxI(10), timeX + timeW, tcyMid + PxI(10)};
        const LONG streamLeft = std::max<LONG>(timeLeft ? capR + gap : L.time.right + gap,
                                               L.lyricsToggle.left - PxI(8) - PxI(320));
        L.stream = {streamLeft, L.transport.top + PxI(8), L.lyricsToggle.left - PxI(8), L.transport.bottom - PxI(4)};
        if (L.stream.right - L.stream.left < PxI(60)) L.stream = {};
    }
    L.nowPlaying = {L.client.left + PxI(10), L.transport.top + PxI(7),
                    std::max<int>(L.client.left + PxI(10), (timeLeft ? L.time.left : capL) - PxI(12)),
                    L.transport.bottom - PxI(3)};

    // Apply to the visual tree.
    const float radius = max ? 0.0f : Px(m::kCornerRadius);
    m_clipGeometry.Size(float2{static_cast<float>(W), static_cast<float>(H)});
    m_clipGeometry.CornerRadius(float2{radius, radius});
    m_frameOutline.IsVisible(!max);

    // DWM's side highlights run down the left / right frame from the top and end sharply at
    // H / 4 + 20 DIP (fitted to Win7 captures: 110 px on a 360 px window at 96 DPI, 290 px on
    // a 1050 px window at 144 DPI).
    {
        const bool show = m_aeroArt && !max && L.clientEdge.left > 0;
        const SIZE size{L.clientEdge.left, H / 4 + PxI(20)};
        for (auto& v : m_sideHighlight)
            v.IsVisible(show);
        if (show) {
            Place(m_sideHighlight[0], {0, 0, size.cx, size.cy});
            Place(m_sideHighlight[1], {W - size.cx, 0, W, size.cy});
            if (size.cx != m_sideHighlightSize.cx || size.cy != m_sideHighlightSize.cy) {
                m_sideHighlightSize = size;
                PaintSideHighlights();
            }
        }
    }

    Place(m_navGroove, L.navGroove);
    if (const SIZE gs{L.navGroove.right - L.navGroove.left, L.navGroove.bottom - L.navGroove.top};
        m_win7Art && gs.cx > 0 && (gs.cx != m_navGrooveSize.cx || gs.cy != m_navGrooveSize.cy)) {
        m_navGrooveSize = gs;
        gfx::CompositionHost::Get().Repaint(m_navGroove, gs.cx, gs.cy, m_scale, [&](ID2D1DeviceContext* dc) {
            wt::NavGroove(dc, D2D1::SizeF(gs.cx / m_scale, gs.cy / m_scale));
        });
    }
    Place(m_clientEdge, L.clientEdge);
    Place(m_clientBackground, L.client);
    Place(m_clientGlow, L.client);
    Place(m_navBar, L.navBar);
    Place(m_cmdBar, L.cmdBar);
    Place(m_contentTop, {L.content.left, L.content.top, L.content.right, L.content.top + PxI(14)});
    Place(m_libraryBg, {L.list.left, L.tabs.bottom > L.tabs.top ? L.tabs.top : L.list.top, L.list.right,
                        L.list.bottom});
    LayoutTabs();
    if (m_detailHeader) {
        const bool show = m_detail.visible && L.detailHeader.right > L.detailHeader.left;
        m_detailHeader.IsVisible(show);
        if (show) {
            const SIZE size{L.detailHeader.right - L.detailHeader.left,
                            L.detailHeader.bottom - L.detailHeader.top};
            if (m_detailHeaderSize.cx != size.cx || m_detailHeaderSize.cy != size.cy)
                PaintDetailHeader(); // a new surface size: repaint and re-place
            else
                Place(m_detailHeader, L.detailHeader);
        }
    }
    Place(m_transportBand, L.transport);
    Place(m_capsule, L.capsule);
    Place(m_cluster, ClusterRect(wt::kClusterRect));
    Place(m_divider, L.divider);
    {
        RECT t = L.title;
        t.right = std::min<LONG>(t.right, t.left + PxI(600) + 2 * pad);
        Place(m_title, t);
    }
    {
        RECT t = L.nowPlaying;
        t.right = std::min<LONG>(t.right, t.left + PxI(360));
        Place(m_nowPlaying, t);
        Place(m_npLyric, t);
    }
    Place(m_time, L.time);
    Place(m_streamVisual, L.stream);
    if (const LONG sw = L.stream.right - L.stream.left; sw != m_streamWidth) {
        m_streamWidth = sw;
        RepaintStreamInfo();
    }
    {
        RECT t = L.breadcrumb;
        t.right = std::min<LONG>(t.right, t.left + PxI(900));
        Place(m_breadcrumb, t);
        m_crumbBar->SetBounds(t, m_scale);
    }
    m_nav->SetBounds(L.navPane, m_scale);
    m_list->SetBounds(L.list, m_scale);
    m_search->SetBounds(L.search, m_scale);
    if (m_pane && !m_paneLent) {
        m_pane->SetBounds(L.lyricsPane, m_scale);
        m_pane->SetVisible(m_lyricsOpen && L.lyricsPane.right > L.lyricsPane.left);
    }

    for (auto& w : m_widgets) {
        const RECT* r = nullptr;
        switch (w->Id()) {
        case kCapMin:
            r = &L.capMin;
            break;
        case kCapMax:
            r = &L.capMax;
            break;
        case kCapClose:
            r = &L.capClose;
            break;
        case kBack:
            r = &L.back;
            break;
        case kForward:
            r = &L.forward;
            break;
        case kOrganize:
            r = &L.organize;
            break;
        case kAddFiles:
            r = &L.addFiles;
            break;
        case kAddFolder:
            r = &L.addFolder;
            break;
        case kNewPlaylist:
            r = &L.newPlaylist;
            break;
        case kViewOptions:
            r = &L.viewOptions;
            break;
        case kRefresh:
            r = &L.refresh;
            break;
        case kDownloadAll:
            r = &L.download;
            break;
        case kLocalPlay:
            r = &L.localPlay;
            break;
        case kDetailAction:
            r = &L.detailAction;
            break;
        case kDetailBriefToggle:
            r = &L.detailBriefToggle;
            break;
        case kSeek:
            r = &L.seek;
            break;
        case kShuffle:
            r = &L.shuffle;
            break;
        case kRepeat:
            r = &L.repeat;
            break;
        case kStop:
            r = &L.stop;
            break;
        case kPrev:
            r = &L.prev;
            break;
        case kNext:
            r = &L.next;
            break;
        case kPlay:
            r = &L.play;
            break;
        case kMute:
            r = &L.mute;
            break;
        case kVolume:
            r = &L.volume;
            break;
        case kLyricsToggle:
            r = &L.lyricsToggle;
            break;
        case kVolArrow:
            r = &L.volArrow;
            break;
        case kCmdMore:
            r = &L.cmdMore;
            break;
        }
        if (r) w->SetBounds(*r, m_scale);
    }
    PaintCapsule();
    // The folded speaker's plate (WMP's split button 4630) behind both halves; the speaker
    // itself draws differently folded, at the same size.
    if (const bool folded = m_win7Art && L.transportLevel >= 2; folded != m_speakerFolded) {
        m_speakerFolded = folded;
        if (auto* w = WidgetById(kMute)) w->Invalidate();
        UpdateSplitPlate();
    }
    m_splitPlate.IsVisible(m_speakerFolded);
    if (m_speakerFolded) {
        const RECT pr{L.mute.left, L.mute.top, L.volArrow.right, L.mute.top + PxI(25)};
        Place(m_splitPlate, pr);
        const int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
        gfx::CompositionHost::Get().Repaint(m_splitPlate, pw, ph, m_scale, [&](ID2D1DeviceContext* dc) {
            const float k = ph / m_scale / 25.0f;
            wt::SplitPlate(dc, 4630, false, 0, k);
            wt::SplitPlate(dc, 4630, true, 25 * k, k);
        });
    }
    // The folded volume's flyout: WMP's frame over a frosted copy of the band.
    {
        const bool open = m_volFlyoutOpen && L.volFlyout.right > L.volFlyout.left;
        m_volFlyout.IsVisible(open);
        m_volFlyoutGlass.IsVisible(open);
        if (open) {
            Place(m_volFlyout, L.volFlyout);
            Place(m_volFlyoutGlass, L.volFlyout);
            if (m_volFlyoutScale != m_scale) {
                m_volFlyoutScale = m_scale;
                m_volFlyoutGlass.Brush(gfx::MakeFrostBrush(m_volFlyoutGlass.Compositor(), Px(4.0f)));
            }
            const int w = L.volFlyout.right - L.volFlyout.left, h = L.volFlyout.bottom - L.volFlyout.top;
            gfx::CompositionHost::Get().Repaint(m_volFlyout, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
                wt::FlyoutFrame(dc, D2D1::SizeF(w / m_scale, h / m_scale));
            });
        }
    }
    m_glass->UpdateReflectionAnchor(m_hwnd);
    // The time's alignment follows its side, and the song's text is laid out within the width
    // the block has (cut short with an ellipsis, its star never under the capsule). Painting
    // lays out again, which then finds both unchanged.
    if (timeMoved || NowPlayingWidth() != m_npPaintedWidth || L.time.right - L.time.left != m_timePaintedW)
        RepaintNowPlaying();
}

float MainWindow::NowPlayingWidth() const {
    const float w = (m_layout.nowPlaying.right - m_layout.nowPlaying.left) / m_scale;
    return std::clamp(std::floor(w) - 4, 0.0f, 350.0f);
}

// The capsule holds what the transport shows: WMP's bitmap with its ends moved in for a
// narrow window (drawn again only when they move).
void MainWindow::PaintCapsule() {
    if (!m_win7Art) return;
    const Layout& L = m_layout;
    if (L.capsuleRight <= L.capsuleLeft) return; // not laid out yet
    if (L.capsuleLeft == m_capsulePaintedLeft && L.capsuleRight == m_capsulePaintedRight &&
        m_capsulePaintedScale == m_scale)
        return;
    m_capsulePaintedLeft = L.capsuleLeft;
    m_capsulePaintedRight = L.capsuleRight;
    m_capsulePaintedScale = m_scale;
    const RECT cap = ClusterRect(wt::kCapsuleRect);
    const float left = L.capsuleLeft, right = L.capsuleRight;
    gfx::CompositionHost::Get().Repaint(m_capsule, cap.right - cap.left, cap.bottom - cap.top, m_scale,
                                        [left, right](ID2D1DeviceContext* dc) { wt::CapsuleSpan(dc, left, right); });
}

// Every surface came back empty: what is painted once and kept (the frame, the bars, the
// capsule, the now-playing block, the widgets) is painted again, the scale-keyed marks first
// forgotten. The panes and views repaint themselves on the same notification.
void MainWindow::OnDeviceReplaced() {
    m_capsulePaintedScale = 0;
    m_npPaintedWidth = -1;
    m_timePaintedW = 0;
    RepaintScaleDependent();
    UpdateLayout();
    for (auto& w : m_widgets)
        w->Invalidate();
    m_seekPaint.Request();
    FlushPlaybackPaints();
    RepaintNowPlaying();
}

void MainWindow::RepaintScaleDependent() {
    const float s = m_scale;
    m_glass->SetScale(s);
    PaintSideHighlights();
    m_navGrooveSize = {}; // repainted at the new scale by the next layout
    const int line = LinePx();

    // Frame lines are one device pixel at any DPI (as DWM draws them); the corner scales with DPI.
    const int frameSrc = PxI(32);
    m_frameOutline.Brush(NineGrid(frameSrc, frameSrc, s, static_cast<float>(PxI(m::kCornerRadius + 2)), true,
                                  [&](ID2D1DeviceContext* dc) {
                                      paint::FrameOutline(dc, D2D1::SizeF(frameSrc / s, frameSrc / s),
                                                          m::kCornerRadius);
                                  }));
    const int edgeSrc = PxI(16);
    m_clientEdge.Brush(NineGrid(edgeSrc, edgeSrc, s, 3.0f, true, [&](ID2D1DeviceContext* dc) {
        paint::ClientEdge(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));
    m_clientGlow.Brush(NineGrid(edgeSrc, edgeSrc, s, 4.0f, true, [&](ID2D1DeviceContext* dc) {
        paint::ClientGlow(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));

    // WMP 12 client chrome (measured; docs/design/visual-spec.md)
    const float lineDip = line / s;
    m_navBar.Brush(Strip(PxI(NavBarHeight()), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle({-1, 0, 100, h},
                          VGrad(dc, 0, h, {{0.0f, 0xDAEFFF}, {0.3f, 0xE2EBF8}, {1.0f, 0xE7EEFE}}).get());
        FillLine(dc, h - lineDip, lineDip, 0xC3CCCF);
    }));
    m_cmdBar.Brush(Strip(PxI(m::kCommandBarHeight), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle(
            {-1, 0, 100, h},
            VGrad(
                dc, 0, h,
                {{0.0f, 0xF5FEFF}, {0.3f, 0xEDF7FD}, {0.48f, 0xE9F3FD}, {0.52f, 0xDCE6F2}, {1.0f, 0xE1EAF9}})
                .get());
        FillLine(dc, h - 2 * lineDip, lineDip, 0xCBD2E2);
        FillLine(dc, h - lineDip, lineDip, 0xABB1BF);
    }));
    m_contentTop.Brush(Strip(PxI(14), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle({-1, 0, 100, h}, VGrad(dc, 0, h, {{0.0f, 0xE0E7F5}, {1.0f, 0xF1F7FD}}).get());
    }));
    m_transportBand.Brush(Strip(PxI(TransportHeight()), s, [&](ID2D1DeviceContext* dc, float h) {
        if (m_win7Art) {
            // Native WMP 12 capture, 96 DPI: dark top rule, then E9F1F9 fading to DFEAF7.
            dc->FillRectangle(
                {-1, 0, 100, h},
                VGrad(dc, 0, h, {{0.0f, 0xEBF2FA}, {0.1f, 0xE9F1F9}, {0.4f, 0xE3EDF8}, {1.0f, 0xDFEAF7}})
                    .get());
            FillLine(dc, 0, lineDip, 0x9DA1A6);
            return;
        }
        dc->FillRectangle({-1, 0, 100, h}, VGrad(dc, 0, h, {{0.0f, 0xEBF2FB}, {1.0f, 0xE3ECF7}}).get());
        FillLine(dc, 0, lineDip, 0x99A1AA);
        FillLine(dc, lineDip, lineDip, 0xF6FAFE);
    }));
    if (m_win7Art) {
        auto& host = gfx::CompositionHost::Get();
        const RECT cl = ClusterRect(wt::kClusterRect);
        m_capsulePaintedScale = 0; // drawn by the layout at this scale, as long as it is
        PaintCapsule();
        host.Repaint(m_cluster, cl.right - cl.left, cl.bottom - cl.top, s,
                     [](ID2D1DeviceContext* dc) { wt::ClusterBackground(dc); });
    } else {
        const int capH = PxI(m::kCapsuleHeight);
        m_capsule.Brush(
            NineGrid(2 * capH, capH, s, static_cast<float>(capH / 2), false, [&](ID2D1DeviceContext* dc) {
                paint::TransportCapsule(dc, D2D1::SizeF(2 * capH / s, capH / s));
            }));
    }
    {
        auto& host = gfx::CompositionHost::Get();
        const int w = PxI(4), h = PxI(16);
        host.Repaint(m_divider, w, h, s,
                     [&](ID2D1DeviceContext* dc) { paint::CapsuleDivider(dc, D2D1::SizeF(w / s, h / s)); });
    }
    m_icon = LoadAppIcon(PxI(m::kIconSize));
    // Window icons for the taskbar, Alt+Tab and the system menu, at this monitor's sizes.
    const UINT dpi = static_cast<UINT>(std::lround(m_scale * 96));
    for (auto [which, metric] : {std::pair{ICON_SMALL, SM_CXSMICON}, std::pair{ICON_BIG, SM_CXICON}}) {
        const int size = GetSystemMetricsForDpi(metric, dpi);
        HICON icon = which == ICON_BIG ? CreateCenteredAppIcon(size) : CreateAppIcon(size);
        if (HICON old = reinterpret_cast<HICON>(
                SendMessageW(m_hwnd, WM_SETICON, which, reinterpret_cast<LPARAM>(icon))))
            if (old != icon) DestroyIcon(old);
    }
    RepaintTitle();
    RepaintNowPlaying();
    RepaintBreadcrumb();
}

void MainWindow::RepaintTitle() {
    auto& host = gfx::CompositionHost::Get();
    const float pad = paint::kGlowPadding;
    const int w = PxI(600 + 2 * pad), h = PxI(24 + 2 * pad);
    host.Repaint(m_title, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float iconSz = m::kIconSize;
        paint::Icon(dc, m_icon.get(),
                    D2D1::RectF(pad, pad + 12 - iconSz / 2, pad + iconSz, pad + 12 + iconSz / 2));
        auto layout = MakeText(kAppName, m::kTitleFontSize, DWRITE_FONT_WEIGHT_NORMAL, 560, 24);
        paint::GlowText(dc, layout.get(), D2D1::Point2F(pad + iconSz + 6, pad), D2D1::ColorF(0, 0, 0, 1));
    });
    UpdateLayout(); // Repaint resets the visual to the surface size; re-apply the layout clip.
}

void MainWindow::RefreshNowPlayingArt(bool refreshText) {
    if (m_held.track.is_valid()) { // a song waiting for its cover: shown with it once it has come
        ReleaseHeldTrack(false);
        return;
    }
    auto pc = playback_control::get();
    metadb_handle_ptr now;
    winrt::com_ptr<IWICBitmapSource> art;
    if (pc->get_now_playing(now)) {
        const std::wstring identity = lyrics::Service::TrackIdentity(now);
        // Another song than the one shown comes in through ShowTrack (with its text), never here.
        if (identity != m_artTrackIdentity) return;
        const bool identityChanged = false;
        const std::uint64_t generation = ++m_artGeneration;
        const int sizePx = std::max(PxI(TransportHeight()), PxI(64));
        ArtCache::Get().Find(now, sizePx, &art);
        metadb_handle_ptr current;
        if (generation != m_artGeneration || !pc->get_now_playing(current) ||
            lyrics::Service::TrackIdentity(current) != identity)
            return;
        if (!identityChanged && !refreshText && art.get() == m_art.get()) return;
        m_art = std::move(art);
        m_lyricsArt = m_art;
    } else {
        // Between songs (or an output restart) the last one stays shown whole; a stop clears it
        // (ClearShownTrack).
        if (m_lyricsTrack.is_valid() || !m_artTrackIdentity.empty()) return;
        if (!refreshText && !m_art) return;
        m_art = nullptr;
        m_lyricsArt = nullptr;
        ++m_artGeneration;
    }
    if (m_pane) m_pane->SetTrack(m_trackTitle, m_trackArtist, m_lyricsArt);
    {
        // The cover's colour: under the lyrics at once (clear while it loads, the window's
        // tint showing), and on the glass when the tint follows the cover - there the last
        // cover's colour stays until the next one has loaded.
        const std::optional<uint32_t> tint = CoverTint(m_art.get());
        if (m_pane) m_pane->SetCoverTint(tint);
        if (tint && tint != m_windowCoverRgb) {
            m_windowCoverRgb = tint;
            ApplyTint(true);
        }
    }
    RepaintNowPlaying();
    const metadb_handle_ptr cover = m_artTrackIdentity.empty() ? metadb_handle_ptr() : now;
    if (m_taskbar) m_taskbar->SetCoverTrack(cover);
    if (m_playerTaskbar) m_playerTaskbar->SetCoverTrack(cover);
}

bool MainWindow::PlaybackPaintAllowed() const {
    return m_hwnd && IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd);
}

void MainWindow::FlushPlaybackPaints() {
    if (!PlaybackPaintAllowed()) return;
    if (m_seekPaint.Take(true))
        if (auto* seek = WidgetById(kSeek)) seek->Invalidate();
    if (m_nowPlayingPaint.Pending()) RepaintNowPlaying();
}

void MainWindow::UpdatePlaybackPresentation() {
    const bool allowed = PlaybackPaintAllowed();
    if (!allowed) {
        m_playbackPresentationSuspended = true;
        if (m_frameClock) m_frameClock->Stop();
        return;
    }
    if (!m_playbackPresentationSuspended) return;
    m_playbackPresentationSuspended = false;
    FlushPlaybackPaints();
    bool more = m_pane && m_pane->Tick();
    more |= TickNowPlayingLyric();
    if (more) StartLyricsFrames();
    StartSeekFrames();
}

void MainWindow::RepaintNowPlaying() {
    m_nowPlayingPaint.Request();
    if (!m_nowPlayingPaint.Take(PlaybackPaintAllowed())) return;
    m_npPaintedWidth = NowPlayingWidth();
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(360), h = PxI(TransportHeight() - 10);
    host.Repaint(m_nowPlaying, m_nowPlayingBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_trackTitle.empty()) return;
        const float H = h / m_scale;
        const float side = H - 2;
        const D2D1_RECT_F coverBox{2, 1, 2 + side, 1 + side};
        com_ptr<ID2D1Bitmap> bmp;
        if (m_art) {
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(m_art.get(), nullptr, bmp.put()))) {
                const D2D1_SIZE_F sz = bmp->GetSize();
                const float k = side / std::max(sz.width, sz.height);
                const D2D1_RECT_F dst{2, 1 + (side - sz.height * k) / 2, 2 + sz.width * k,
                                      1 + (side + sz.height * k) / 2};
                // Same edge as the covers in the library (soft shadow, one-pixel 18 % black line).
                const float m = (dst.right - dst.left) * 8.0f / 184.0f;
                win7::DrawStretched(
                    dc, win7::Module::Wmploc, L"ALBUMDROPSHADOW_200X200.PNG",
                    {dst.left - m + 1, dst.top - m + 2, dst.right + m + 1, dst.bottom + m + 2}, 0.45f);
                dc->DrawBitmap(bmp.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr,
                               nullptr);
                com_ptr<ID2D1SolidColorBrush> frame;
                dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.18f), frame.put());
                const float px = 1.0f / m_scale;
                dc->DrawRectangle(
                    {dst.left + px / 2, dst.top + px / 2, dst.right - px / 2, dst.bottom - px / 2},
                    frame.get(), px);
            }
        }
        if (!bmp && !win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUM_0.PNG", coverBox))
            pane::DrawCover(dc, nullptr, coverBox, 4.0f);
        if (m_npHot == 5) {
            // Like Apple Music's cover: a veil and the glyph of what a click does.
            com_ptr<ID2D1SolidColorBrush> veil;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x0A1E46, 0.55f), veil.put());
            dc->FillRectangle(coverBox, veil.get());
            const float cx = (coverBox.left + coverBox.right) / 2, cy = (coverBox.top + coverBox.bottom) / 2;
            if (!win7::Draw(dc, win7::Module::Wmploc, 4601, {std::round(cx - 12.5f), std::round(cy - 12.5f)}))
                pane::DrawIcon(dc, L'\xE740', 14, {cx, cy}, pane::White(dc, 0.95f).get());
        }
        const float x = coverBox.right + 8;
        m_npArtRect = {m_layout.nowPlaying.left + PxI(coverBox.left), m_layout.nowPlaying.top + PxI(coverBox.top),
                       m_layout.nowPlaying.left + PxI(coverBox.right), m_layout.nowPlaying.top + PxI(coverBox.bottom)};
        com_ptr<ID2D1SolidColorBrush> dark, grey, link;
        dc->CreateSolidColorBrush(Hex(0x1E1E1E), dark.put());
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        dc->CreateSolidColorBrush(Hex(0x0066CC), link.put());
        // Row 1: title and artist (links: the song in the list, the artist), then the Favorite
        // star. Row 2: the current lyric line (PaintNowPlayingLyric). Without synced lyrics, or
        // with the line hidden, row 1 holds the title and the star and row 2 the artist.
        const bool lyricRow = m_npLyrics && m_npLyrics->synced && settings::LyricLineShown();
        const float top = std::round(H / 2 - kInfoRow);
        m_npTextX = x;
        m_npLyricTop = top + kInfoRow;
        const auto toClient = [&](D2D1_RECT_F r) {
            return RECT{m_layout.nowPlaying.left + PxI(r.left), m_layout.nowPlaying.top + PxI(r.top),
                        m_layout.nowPlaying.left + PxI(r.right), m_layout.nowPlaying.top + PxI(r.bottom)};
        };
        const auto natural = [](const std::wstring& text) {
            if (text.empty()) return 0.0f;
            DWRITE_TEXT_METRICS tm{};
            MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 2000, kInfoRow + 2)->GetMetrics(&tm);
            return std::ceil(tm.widthIncludingTrailingWhitespace);
        };
        constexpr float kStar = 22, kGap = 10;
        const float limit = m_npPaintedWidth;
        float titleW = natural(m_trackTitle), artistW = natural(m_trackArtist);
        const auto text = [&](const std::wstring& t, float at, float rowTop, float width, ID2D1SolidColorBrush* ink,
                              bool hot) {
            auto layout = MakeText(t, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, std::max(width, 1.0f), kInfoRow + 2);
            if (hot) layout->SetUnderline(TRUE, {0, static_cast<UINT32>(t.size())});
            dc->DrawTextLayout({at, rowTop - 1}, layout.get(), hot ? link.get() : ink);
            return D2D1_RECT_F{at, rowTop, at + width, rowTop + kInfoRow};
        };
        float end = x;
        m_npArtistRect = {};
        m_npCut.clear();
        const float fullTitleW = titleW, fullArtistW = artistW;
        if (lyricRow) {
            // Share the row: the title first, the artist keeps at least 40 % when both are long.
            // Too narrow for both to stay readable, the artist leaves the row to the title
            // (owner, 2026-11-14: in the narrowest window the two were drawn over each other).
            const float room = std::max(0.0f, limit - x - kStar);
            constexpr float kBothMin = 140;
            if (artistW > 0 && titleW + kGap + artistW > room) {
                const float share = room - kGap;
                if (share < kBothMin) {
                    artistW = 0;
                } else {
                    artistW = std::min(artistW, std::max(std::min(artistW, 60.0f), share * 0.4f));
                    titleW = std::min(titleW, share - artistW);
                    artistW = std::min(artistW, share - titleW);
                }
            }
            titleW = std::min(titleW, room);
            m_npTitleRect = toClient(text(m_trackTitle, x, top, titleW, dark.get(), m_npHot == 1));
            end = x + titleW;
            if (artistW > 0) {
                m_npArtistRect = toClient(text(m_trackArtist, end + kGap, top, artistW, grey.get(), m_npHot == 2));
                end += kGap + artistW;
            }
        } else {
            titleW = std::min(titleW, std::max(0.0f, limit - x - kStar));
            m_npTitleRect = toClient(text(m_trackTitle, x, top, titleW, dark.get(), m_npHot == 1));
            end = x + titleW;
            artistW = std::min(artistW, std::max(0.0f, limit - x));
            if (artistW > 0)
                m_npArtistRect = toClient(text(m_trackArtist, x, top + kInfoRow, artistW, grey.get(), m_npHot == 2));
        }
        // Cut short (or left out for the title): the tooltip shows it whole (UpdateDetailTip).
        if (titleW + 0.5f < fullTitleW && m_npTitleRect.right > m_npTitleRect.left)
            m_npCut.push_back({m_npTitleRect, m_trackTitle});
        if (artistW > 0 && artistW + 0.5f < fullArtistW) m_npCut.push_back({m_npArtistRect, m_trackArtist});
        // WMP's stars: gold (5530) in Favorites, gold outline (5531) on hover, empty (5533).
        const float sx = std::round((end + 6) * m_scale) / m_scale;
        const float sy = std::round((top + (kInfoRow - 14) / 2) * m_scale) / m_scale;
        win7::Draw(dc, win7::Module::Wmploc, m_npFavorite ? 5530 : m_npHot == 4 ? 5531 : 5533, {sx, sy});
        m_npStarRect = toClient({sx - 3, sy - 3, sx + 17, sy + 17});
    });
    if (m_trackTitle.empty()) m_npTitleRect = m_npArtistRect = m_npStarRect = {};
    if (!m_trackTitle.empty()) {
        metadb_handle_ptr now;
        if (playback_control::get()->get_now_playing(now)) {
            auto* provider = stream::ProviderOf(now);
            if (provider && !provider->CanSetFavorite(!m_npFavorite)) m_npStarRect = {};
        }
    }
    m_npLyricLine = -2;
    PaintNowPlayingLyric();
    // Time beside the controls, painted at its box's width (the widest its text gets).
    m_timePaintedW = std::max<LONG>(1, m_layout.time.right - m_layout.time.left);
    const int timeW = static_cast<int>(m_timePaintedW);
    host.Repaint(m_time, m_timeBacking, timeW, PxI(20), m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_length <= 0 && m_trackTitle.empty()) return;
        std::wstring text = FormatTime(m_position);
        if (m_length > 0) text += L" / " + FormatTime(m_length);
        com_ptr<ID2D1SolidColorBrush> grey;
        dc->CreateSolidColorBrush(Hex(0x4A4A4A), grey.put());
        // Left of the capsule (a narrow window) it ends at the capsule instead.
        auto layout = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, timeW / m_scale + 20, 20,
                               m_layout.timeLeft ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        if (m_layout.timeLeft) layout->SetMaxWidth(timeW / m_scale);
        dc->DrawTextLayout({0, 0}, layout.get(), grey.get());
    });
    UpdateLayout();
}

// The lyric line under the title. Word-timed lines turn blue word by word as they are sung;
// line-timed ones are blue as a whole.
void MainWindow::PaintNowPlayingLyric() {
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(360), h = PxI(TransportHeight() - 10);
    if (!m_npLyricSurface || m_npLyricSize.cx != w || m_npLyricSize.cy != h) {
        m_npLyricSurface = host.CreateSurface(w, h);
        m_npLyricSize = {w, h};
        auto brush = host.Compositor().CreateSurfaceBrush(m_npLyricSurface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        m_npLyric.Brush(brush);
    }
    double now = m_position;
    try {
        if (auto pc = playback_control::get(); pc->is_playing()) now = pc->playback_get_position();
    } catch (...) {
    }
    const int ms = static_cast<int>(now * 1000);
    int index = -1;
    if (m_npLyrics && m_npLyrics->synced && !m_trackTitle.empty() && settings::LyricLineShown())
        for (size_t i = 0; i < m_npLyrics->lines.size() && m_npLyrics->lines[i].start <= ms; ++i)
            index = static_cast<int>(i);
    m_npLyricLine = index;
    m_lyricCut.clear();
    host.Draw(m_npLyricSurface, m_scale, [&](ID2D1DeviceContext* dc) {
        if (index < 0) return;
        const lyrics::Line& line = m_npLyrics->lines[static_cast<size_t>(index)];
        const float width = std::max(1.0f, NowPlayingWidth() - m_npTextX);
        auto layout = MakeText(line.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, width, kInfoRow + 2);
        {
            DWRITE_TEXT_METRICS natural{};
            MakeText(line.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 10000, kInfoRow + 2)->GetMetrics(&natural);
            if (natural.widthIncludingTrailingWhitespace > width + 0.5f) {
                const RECT& np = m_layout.nowPlaying;
                m_lyricCut.push_back({{np.left + PxI(m_npTextX), np.top + PxI(m_npLyricTop),
                                       np.left + PxI(m_npTextX + width), np.top + PxI(m_npLyricTop + kInfoRow)},
                                      line.text});
            }
        }
        com_ptr<ID2D1SolidColorBrush> grey, blue;
        dc->CreateSolidColorBrush(Hex(0x6E6E6E), grey.put());
        dc->CreateSolidColorBrush(Hex(0x1F5FB8), blue.put());
        const D2D1_POINT_2F at{m_npTextX, m_npLyricTop - 1};
        if (line.words.empty()) {
            dc->DrawTextLayout(at, layout.get(), blue.get());
            return;
        }
        // Sung part: words before the current one whole, the current one by its progress.
        // Measured on the whole line: a line cut short with an ellipsis hit-tests its hidden
        // words at odd places, and the fill jumped back and forth. Past the cut the visible
        // line, ellipsis included, stays blue.
        auto whole = MakeText(line.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 10000, kInfoRow + 2);
        float fill = 0;
        UINT32 pos = 0;
        for (const auto& word : line.words) {
            const UINT32 len = static_cast<UINT32>(word.text.size());
            if (word.start > ms || pos >= line.text.size()) break;
            DWRITE_HIT_TEST_METRICS hit{};
            UINT32 count = 0;
            whole->HitTestTextRange(pos, std::min<UINT32>(len, static_cast<UINT32>(line.text.size()) - pos), 0, 0,
                                    &hit, 1, &count);
            const float p =
                word.end > word.start
                    ? std::clamp(static_cast<float>(ms - word.start) / (word.end - word.start), 0.0f, 1.0f)
                    : 1.0f;
            if (count > 0) fill = std::max(fill, hit.left + hit.width * p);
            pos += len;
        }
        DWRITE_TEXT_METRICS full{}, shown{};
        whole->GetMetrics(&full);
        layout->GetMetrics(&shown);
        if (full.widthIncludingTrailingWhitespace > width + 0.5f) {
            const float visible = shown.left + shown.width;
            constexpr float kEllipsis = 12; // the trimming sign and the room before it
            if (fill >= visible - kEllipsis) fill = visible + 1;
        }
        dc->DrawTextLayout(at, layout.get(), grey.get());
        if (fill > 0) {
            dc->PushAxisAlignedClip({at.x, 0, at.x + fill, h / m_scale}, D2D1_ANTIALIAS_MODE_ALIASED);
            dc->DrawTextLayout(at, layout.get(), blue.get());
            dc->PopAxisAlignedClip();
        }
    });
}

bool MainWindow::TickNowPlayingLyric() {
    if (!m_npLyrics || !m_npLyrics->synced || !m_playing || !settings::LyricLineShown()) return false;
    const double now = playback_control::get()->playback_get_position();
    const int ms = static_cast<int>(now * 1000);
    int index = -1;
    for (size_t i = 0; i < m_npLyrics->lines.size() && m_npLyrics->lines[i].start <= ms; ++i)
        index = static_cast<int>(i);
    // Repaint when the line changes, and every frame while a word-timed line fills.
    const bool filling = index >= 0 && !m_npLyrics->lines[static_cast<size_t>(index)].words.empty() &&
                         ms <= m_npLyrics->lines[static_cast<size_t>(index)].end + 50;
    if (index != m_npLyricLine || filling) PaintNowPlayingLyric();
    return !m_paused;
}

void MainWindow::RepaintStreamInfo() {
    // Right-aligned: stream format on top, the output device (a menu) below.
    auto& host = gfx::CompositionHost::Get();
    // Painted at the block's real width (narrow windows shrink it); long names get an ellipsis.
    const int w = std::max<int>(1, m_layout.stream.right - m_layout.stream.left),
              h = PxI(TransportHeight() - 12);
    if (m_deviceName.empty()) RefreshDeviceName();
    const std::wstring device = m_deviceName.empty() ? Tr(L"Output device") : m_deviceName;
    float deviceLeft = 0, deviceTop = 0, deviceRight = 0;
    m_streamCut.clear();
    host.Repaint(m_streamVisual, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float W = w / m_scale, H = h / m_scale;
        com_ptr<ID2D1SolidColorBrush> grey, link;
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        dc->CreateSolidColorBrush(Hex(m_npHot == 3 ? 0x0066CC : 0x2E4E7A), link.put());
        const float lh = kInfoRow, top = std::round(H / 2 - lh);
        if (!m_streamInfo.empty()) {
            auto info = MakeText(m_streamInfo, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, W, lh + 2);
            info->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            dc->DrawTextLayout({0, top - 1}, info.get(), grey.get());
            DWRITE_TEXT_METRICS natural{};
            MakeText(m_streamInfo, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, 10000, lh + 2)->GetMetrics(&natural);
            if (natural.widthIncludingTrailingWhitespace > W + 0.5f) {
                const RECT& r = m_layout.stream;
                m_streamCut.push_back({{r.left, r.top + PxI(top), r.right, r.top + PxI(top + lh)}, m_streamInfo});
            }
        }
        const std::wstring label = device + L"  ▾";
        auto dev = MakeText(label, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, W, lh + 2);
        dev->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        dev->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const DWRITE_TRIMMING ellipsis{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        com_ptr<IDWriteInlineObject> sign;
        if (com_ptr<IDWriteFactory> f; SUCCEEDED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.put()))))
            f->CreateEllipsisTrimmingSign(dev.get(), sign.put());
        dev->SetTrimming(&ellipsis, sign.get());
        if (m_npHot == 3) dev->SetUnderline(TRUE, {0, static_cast<UINT32>(device.size())});
        dc->DrawTextLayout({0, top + lh - 1}, dev.get(), link.get());
        DWRITE_TEXT_METRICS tm{};
        dev->GetMetrics(&tm);
        {
            DWRITE_TEXT_METRICS natural{};
            MakeText(label, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, 10000, lh + 2)->GetMetrics(&natural);
            if (natural.widthIncludingTrailingWhitespace > W + 0.5f) {
                const RECT& r = m_layout.stream;
                m_streamCut.push_back({{r.left, r.top + PxI(top + lh), r.right, r.top + PxI(top + 2 * lh)}, device});
            }
        }
        deviceLeft = tm.left;
        deviceRight = tm.left + tm.width;
        deviceTop = top + lh;
    });
    m_deviceText = {deviceLeft, deviceTop, deviceRight, deviceTop + kInfoRow};
}

void MainWindow::RefreshDeviceName() {
    m_deviceName.clear();
    try {
        const auto cfg = output_manager::get()->getCoreConfig();
        output_manager_v2::get()->listDevices([&](const char* name, const GUID& output, const GUID& dev) {
            if (output == cfg.m_output && dev == cfg.m_device) m_deviceName = Wide(name);
        });
    } catch (...) {
    }
}

RECT MainWindow::DeviceRect() const {
    const RECT& s = m_layout.stream;
    if (m_deviceText.right <= m_deviceText.left) return {};
    return {s.left + PxI(m_deviceText.left), s.top + PxI(m_deviceText.top), s.left + PxI(m_deviceText.right),
            s.top + PxI(m_deviceText.bottom)};
}

void MainWindow::ToggleLyrics() {
    KillTimer(m_hwnd, kPaneRevealTimer);
    const bool open = !m_lyricsOpen;
    cfg_lyricsOpen.set(open ? 1 : 0);
    const int paneW = PxI(kLyricsPaneWidth);
    if (IsWindowMaximized() || IsIconic(m_hwnd) || !IsWindowVisible(m_hwnd)) {
        // A maximized window gives the pane part of its width at once.
        m_lyricsOpen = open;
        m_paneReveal = -1;
        UpdateLayout();
    } else {
        // A restored window grows to the right by the pane, eased, and shrinks back the
        // same way; the pane's width follows the window so the library never moves.
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        const int shown = m_paneReveal >= 0 ? m_paneReveal : (m_lyricsOpen ? paneW : 0);
        m_paneRevealBase = wr.right - wr.left - shown; // the window's width without any pane
        m_paneRevealFrom = shown;
        m_paneRevealTo = open ? paneW : 0;
        m_paneReveal = shown;
        m_paneRevealStart = GetTickCount64();
        m_lyricsOpen = true; // the pane stays in the layout until it has slid shut
        SetTimer(m_hwnd, kPaneRevealTimer, 8, nullptr);
        StepPaneReveal();
    }
    if (auto* w = WidgetById(kLyricsToggle)) w->Invalidate();
    RepaintNowPlaying();
}

void MainWindow::StepPaneReveal() {
    const float t = std::clamp(static_cast<float>(GetTickCount64() - m_paneRevealStart) / kPaneRevealMs, 0.0f, 1.0f);
    const float eased = 1.0f - std::pow(1.0f - t, 4.0f); // quartic ease-out: quick start, soft landing
    const bool done = t >= 1.0f;
    m_paneReveal = static_cast<int>(std::lround(m_paneRevealFrom + (m_paneRevealTo - m_paneRevealFrom) * eased));
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int width = std::min<int>(m_paneRevealBase + m_paneReveal, work.right - work.left);
    int left = wr.left;
    if (left + width > work.right) left = std::max<int>(work.left, work.right - width);
    if (done) {
        KillTimer(m_hwnd, kPaneRevealTimer);
        m_lyricsOpen = m_paneRevealTo > 0;
        m_paneReveal = -1;
    }
    SetWindowPos(m_hwnd, nullptr, left, wr.top, width, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateLayout();
    if (done) {
        if (auto* w = WidgetById(kLyricsToggle)) w->Invalidate();
        RepaintNowPlaying();
    }
}

// ---- Window colour ----------------------------------------------------------------------

aero::Colorization MainWindow::WindowColorization() const {
    const int tint = settings::Tint();
    uint32_t rgb = aero::kWin7Tints[0].rgb; // following the cover before any: Sky
    if (tint < static_cast<int>(aero::kWin7Tints.size()))
        rgb = aero::kWin7Tints[static_cast<size_t>(tint)].rgb;
    else if (m_windowCoverRgb)
        rgb = *m_windowCoverRgb;
    return aero::Win7Colorization(rgb, settings::TintIntensity());
}

void MainWindow::ApplyTint(bool fade) {
    const aero::Colorization c = WindowColorization();
    const int ms = fade ? 600 : 0;
    if (m_glass) m_glass->SetColorization(c, ms);
    if (m_player) m_player->SetColorization(c, ms);
}

aero::Colorization MainWindow::PlayerColorization() const { return WindowColorization(); }

// ---- Player window ----------------------------------------------------------------------

void MainWindow::ShowPlayer(bool fullScreen) {
    if (!m_pane) return;
    if (!m_player) m_player = std::make_unique<PlayerWindow>(*this);
    m_player->Show(fullScreen ? PlayerWindow::Mode::Full : PlayerWindow::Mode::Mini);
    if (IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
        m_shadow.Sync();
    }
}

NowPlayingPane* MainWindow::LendPane(const wuc::ContainerVisual& parent) {
    if (!m_pane) return nullptr;
    m_paneLent = true;
    m_pane->SetVisible(false);
    m_pane->Reparent(parent);
    m_pane->SetEmbedded(true);
    RefreshPaneQueue();
    return m_pane.get();
}

void MainWindow::ReturnPane() {
    if (!m_paneLent) return;
    m_paneLent = false;
    if (!m_pane) return;
    m_pane->SetVisible(false);
    m_pane->SetEmbedded(false);
    m_pane->Reparent(m_paneSlot);
}

void MainWindow::PlayerClosed() {
    if (!m_hwnd) return;
    if (!IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_SHOW);
    UpdateLayout();
    m_shadow.Sync();
    SetForegroundWindow(m_hwnd);
}

void MainWindow::SetPlayerVolume(float slider) {
    m_volume = std::clamp(slider, 0.0f, 1.0f);
    for (int id : {kVolume, kMute})
        if (auto* w = WidgetById(id)) w->Invalidate();
    playback_control::get()->set_volume(DbFromSlider(m_volume));
}

void MainWindow::OnSwitching(bool switching, const metadb_handle_ptr& opened) {
    // The start command and the stop for it arrive in either order around the input's
    // announcement; a switch already under way keeps the song it is opening.
    if (switching) {
        if (!m_switching) m_openingTrack.release();
        m_switching = true;
        return;
    }
    m_switching = false;
    m_openingTrack.release();
    // A real stop forgets the last song: the player shows nothing playing, as before.
    m_shownTrack = opened;
}

// Gapless playback opens the next song while this one still plays; only a switch counts.
void MainWindow::OnTrackOpening(const std::string& path) {
    if (!m_switching) return;
    metadb_handle_ptr track;
    metadb::get()->handle_create(track, make_playable_location(path.c_str(), 0));
    if (!track.is_valid() || track == m_openingTrack) return;
    metadb_handle_ptr now;
    if (playback_control::get()->get_now_playing(now)) return; // already playing it
    m_openingTrack = track;
    if (m_player) m_player->OnPlayback();
}

metadb_handle_ptr MainWindow::PlayerTrack() const {
    metadb_handle_ptr now;
    if (playback_control::get()->get_now_playing(now)) return now;
    if (!m_switching) return {};
    return m_openingTrack.is_valid() ? m_openingTrack : m_shownTrack;
}

void MainWindow::PlayerTrackText(const metadb_handle_ptr& track, std::wstring& title, std::wstring& artist,
                                 std::wstring& album) const {
    title.clear();
    artist.clear();
    album.clear();
    if (!track.is_valid()) return;
    auto pc = playback_control::get();
    metadb_handle_ptr now;
    if (pc->get_now_playing(now) && now == track) {
        title = m_trackTitle;
        artist = m_trackArtist;
        static titleformat_object::ptr s_album;
        if (!s_album) titleformat_compiler::get()->compile_safe(s_album, "[%album%]");
        pfc::string8 text;
        pc->playback_format_title(nullptr, text, s_album, nullptr, playback_control::display_level_all);
        album = Wide(text);
    } else {
        // Not decoding (yet, or any more): the library's tags, as the song table shows them.
        static titleformat_object::ptr s_text;
        if (!s_text) titleformat_compiler::get()->compile_safe(s_text, "$if2(%title%,%filename%)|[%artist%]|[%album%]");
        pfc::string8 text;
        track->format_title(nullptr, text, s_text, nullptr);
        std::wstringstream in(Wide(text));
        std::getline(in, title, L'|');
        std::getline(in, artist, L'|');
        std::getline(in, album);
        if (const media::RepairedText* r = media::CueCatalog::Get().Repair(track)) {
            if (!r->title.empty()) title = r->title;
            if (!r->artist.empty()) artist = r->artist;
        }
    }
    if (auto* provider = stream::ProviderOf(track)) {
        if (const auto found = provider->FindTrack(provider->TrackId(track))) {
            if (!found->title.empty()) title = found->title;
            if (!found->artists.empty()) artist = stream::JoinArtists(found->artists);
            if (!found->album.empty()) album = found->album;
        }
    }
}

void MainWindow::TogglePlayerFavorite() {
    if (m_taskbar && m_taskbar->onFavorite) m_taskbar->onFavorite();
}

void MainWindow::AdoptPlayerWindow(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    for (auto [which, metric] : {std::pair{ICON_SMALL, SM_CXSMICON}, std::pair{ICON_BIG, SM_CXICON}}) {
        const int size = GetSystemMetricsForDpi(metric, dpi);
        HICON icon = which == ICON_BIG ? CreateCenteredAppIcon(size) : CreateAppIcon(size);
        if (HICON old = reinterpret_cast<HICON>(SendMessageW(window, WM_SETICON, which, reinterpret_cast<LPARAM>(icon))))
            if (old != icon) DestroyIcon(old);
    }
    m_playerTaskbar = std::make_unique<Taskbar>(window);
    m_playerTaskbar->onFavorite = [this] {
        if (m_taskbar && m_taskbar->onFavorite) m_taskbar->onFavorite();
    };
    m_playerTaskbar->SetPlayState(m_playing, m_paused);
    m_playerTaskbar->SetFavorite(m_npFavorite);
    metadb_handle_ptr now;
    if (!m_artTrackIdentity.empty() && playback_control::get()->get_now_playing(now))
        m_playerTaskbar->SetCoverTrack(now);
}

bool MainWindow::PlayerWindowMessage(UINT msg, WPARAM wp, LPARAM lp) {
    return m_playerTaskbar && m_playerTaskbar->OnMessage(msg, wp, lp);
}

// One track per album of the playing playlist (else the active one), each album once. A
// streamed track names its album's picture (the art cache's key); a local file belongs to its
// album artist and album, or to its folder when it has no album tag.
std::vector<metadb_handle_ptr> MainWindow::WallAlbums() const {
    std::vector<metadb_handle_ptr> albums;
    auto pm = playlist_manager::get();
    size_t playlist = pm->get_playing_playlist();
    if (playlist == SIZE_MAX) playlist = pm->get_active_playlist();
    if (playlist == SIZE_MAX) return albums;
    metadb_handle_list items;
    pm->playlist_get_all_items(playlist, items);
    static titleformat_object::ptr s_album;
    if (!s_album) titleformat_compiler::get()->compile_safe(s_album, "$if(%album%,%album artist%|%album%)");
    std::unordered_set<std::wstring> seen;
    pfc::string8 text;
    for (size_t i = 0; i < items.get_count(); ++i) {
        const metadb_handle_ptr& track = items[i];
        std::wstring key;
        if (stream::ProviderOf(track)) {
            key = L"art:" + ArtCache::SourceKey(track);
        } else {
            track->format_title(nullptr, text, s_album, nullptr);
            if (!text.is_empty()) {
                key = L"album:" + std::wstring(pfc::stringcvt::string_wide_from_utf8(text.get_ptr()).get_ptr());
            } else {
                std::wstring path = pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr();
                path.resize(path.find_last_of(L"/\\") == std::wstring::npos ? 0 : path.find_last_of(L"/\\"));
                key = L"folder:" + path;
            }
        }
        if (seen.insert(std::move(key)).second) albums.push_back(track);
    }
    return albums;
}

void MainWindow::ToggleQueuePane() {
    if (!m_pane) return;
    if (!m_lyricsOpen) {
        m_pane->SetMode(NowPlayingPane::Mode::Queue);
        ToggleLyrics();
        return;
    }
    m_pane->SetMode(m_pane->CurrentMode() == NowPlayingPane::Mode::Queue ? NowPlayingPane::Mode::Lyrics
                                                                         : NowPlayingPane::Mode::Queue);
}

void MainWindow::RequestLyrics(const metadb_handle_ptr& track, bool again) {
    const std::wstring identity = lyrics::Service::TrackIdentity(track);
    if (identity != m_lyricsTrackIdentity) ResetLyrics();
    m_lyricsTrack = track;
    m_lyricsTrackIdentity = identity;
    const std::uint64_t request = ++m_lyricsRequestGeneration;
    auto done = [this, alive = std::weak_ptr<int>(m_alive), request, identity](const lyrics::Result& result) {
        if (alive.expired()) return;
        if (!lyrics::IsCurrentLyricsRequest(request, m_lyricsRequestGeneration, identity,
                                            m_lyricsTrackIdentity))
            return;
        OnLyrics(result);
    };
    if (again)
        lyrics::Service::Get().Research(track, done);
    else
        lyrics::Service::Get().Request(track, done);
}

void MainWindow::ResetLyrics() {
    ++m_lyricsRequestGeneration;
    lyrics::Service::Get().Cancel();
    CloseLyricsSearch();
    m_lyricsTrack.release();
    m_lyricsTrackIdentity.clear();
    OnLyrics({});
}

void MainWindow::OnLyrics(const lyrics::Result& result) {
    // Keep the current document and its animated position visible while a same-track
    // refresh is in flight. Actual results replace it only when their content differs.
    if (result.status == lyrics::Status::Searching && m_npLyrics) {
        if (m_pane) m_pane->SetResult(result);
        return;
    }
    // Every view shows the same text: with Swap original and translation on, the translation
    // is the lyric (and is sung word by word) and the original the line under it.
    // The owner's offset for the song (Adjust offset) moves every time first.
    m_lyricsResult = result;
    lyrics::Result shown = result;
    if (shown.status == lyrics::Status::Found && shown.lyrics) {
        const int offset = lyrics::Service::Get().Offset(m_lyricsTrack, *shown.lyrics);
        const bool translated = std::any_of(shown.lyrics->lines.begin(), shown.lyrics->lines.end(),
                                            [](const lyrics::Line& l) { return !l.translation.empty(); });
        const bool swap = translated && settings::SwapTranslation();
        // Hide translation hides the small line, whichever text it holds after a swap.
        const bool hide = translated && settings::HideTranslation();
        if (offset != 0 || swap || hide) {
            lyrics::Lyrics adjusted = *shown.lyrics;
            lyrics::ApplyOffset(adjusted, offset);
            if (swap) adjusted = lyrics::SwapTranslation(adjusted);
            if (hide)
                for (auto& line : adjusted.lines) line.translation.clear();
            shown.lyrics = std::make_shared<const lyrics::Lyrics>(std::move(adjusted));
        }
    }
    m_npLyrics = shown.status == lyrics::Status::Found ? shown.lyrics : nullptr;
    m_npLyricLine = -2;
    if (m_pane) m_pane->SetResult(shown);
    RepaintNowPlaying();
    if (m_player) m_player->OnPlayback();
    if (m_npLyrics && m_npLyrics->synced && m_playing && !m_paused) StartLyricsFrames();
}

void MainWindow::StartLyricsFrames() {
    if (m_paneLent) {
        if (m_player) m_player->StartFrames();
        return;
    }
    if (!PlaybackPaintAllowed()) {
        m_playbackPresentationSuspended = true;
        if (m_frameClock) m_frameClock->Stop();
        return;
    }
    if (!m_frameClock) m_frameClock = std::make_unique<gfx::FrameClock>(m_hwnd, kFrameMessage);
    m_frameClock->Start();
}

// Up Next follows Now Playing after its current song, wrapping once when repeat-all is on.
// History uses the cross-source play history, oldest first.
void MainWindow::SchedulePaneQueueRefresh(bool playbackChanged) {
    m_queueFollowPending |= playbackChanged;
    if (m_queueRefreshPending) return;
    m_queueRefreshPending = true;
    // Playlist batches and playback callbacks can run before the core commits the new
    // playing location. Read one complete snapshot after those callbacks have returned.
    fb2k::inMainThread([this, alive = std::weak_ptr<int>(m_alive)] {
        if (alive.expired()) return;
        m_queueRefreshPending = false;
        const bool follow = std::exchange(m_queueFollowPending, false);
        RefreshPaneQueue(follow);
    });
}

void MainWindow::RefreshPaneQueue(bool playbackChanged) {
    if (!m_pane || !(m_lyricsOpen || m_paneLent)) return;
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    auto pm = playlist_manager::get();
    std::vector<QueueView::Row> upNext;
    m_paneUpcoming.clear();
    m_paneScheduleRevision = media::PlaybackScheduler::Get().ScheduleRevision();
    const t_size queue = media::QueuePlaylist(false);
    if (queue != SIZE_MAX) {
        t_size playing = SIZE_MAX, index = SIZE_MAX;
        const bool fromQueue = pm->get_playing_item_location(&playing, &index) && playing == queue;
        const t_size count = pm->playlist_get_item_count(queue);
        if (!fromQueue) index = playback_control::get()->is_playing() ? SIZE_MAX : m_panePlayingIndex;
        if (index >= count) index = SIZE_MAX;
        metadb_handle_ptr cursor;
        if (index != SIZE_MAX) cursor = pm->playlist_get_item_handle(queue, index);
        if (!fromQueue && cursor != m_panePlayingTrack) {
            // A stopped cursor must not survive replacing or removing its old item.
            index = SIZE_MAX;
            cursor.release();
        }
        playbackChanged |= index != m_panePlayingIndex;
        m_panePlayingIndex = index;
        m_panePlayingTrack = cursor;
        std::unordered_set<size_t> shownPhysical;
        for (const auto& next : media::PlaybackScheduler::Get().DisplayUpcomingSnapshot()) {
            if (!next.handle.is_valid()) continue;
            if (next.source == media::ScheduledUpcomingSource::Playlist && next.playlistIndex >= count)
                continue;
            QueueView::Row row;
            row.handle = next.handle;
            pfc::string8 text;
            row.handle->format_title(nullptr, text, s_title, nullptr);
            row.title = Wide(text);
            row.handle->format_title(nullptr, text, s_artist, nullptr);
            row.artist = Wide(text);
            row.badges = stream::TrackBadges(row.handle);
            row.playlistIndex = next.playlistIndex < count && shownPhysical.insert(next.playlistIndex).second
                                    ? next.playlistIndex
                                    : QueueView::kExplicitQueueTag | next.queueIndex;
            m_paneUpcoming.emplace(row.playlistIndex, next);
            upNext.push_back(std::move(row));
        }
    }
    std::vector<QueueView::Row> history;
    metadb_handle_ptr now;
    const bool loaded = playback_control::get()->get_now_playing(now);
    auto records = media::PlayStats::Get().Recent(50);
    for (auto it = records.rbegin(); it != records.rend(); ++it) { // oldest first
        QueueView::Row row;
        if (!it->provider.empty()) {
            if (auto* provider = stream::FindProvider(it->provider))
                row.handle = provider->TrackItem(it->trackId);
        } else {
            row.handle = metadb::get()->handle_create(it->path.c_str(), it->subsong);
        }
        if (!row.handle.is_valid()) continue;
        row.title = Wide(it->title.c_str());
        row.artist = Wide(it->artist.c_str());
        row.badges = stream::TrackBadges(row.handle);
        history.push_back(std::move(row));
    }
    // The newest record is the song playing now: it is the card, not history.
    if (loaded && !history.empty() && history.back().handle == now) history.pop_back();
    m_pane->Queue().SetHistory(std::move(history));
    m_pane->Queue().SetUpNext(std::move(upNext), playbackChanged);
    m_pane->Queue().SetShuffleRepeat(m_shuffle, static_cast<int>(m_repeat), media::radio::Active());
    m_pane->Queue().SetRepeatCycle(media::PlaybackScheduler::Get().RepeatCycleSize());
}

void MainWindow::ConnectPaneQueue() {
    auto& queue = m_pane->Queue();
    queue.onPlay = [this](size_t index) {
        if (media::PlaybackScheduler::Get().ScheduleRevision() != m_paneScheduleRevision) {
            OnCoreQueueChanged();
            return;
        }
        auto pm = playlist_manager::get();
        if (QueueView::IsExplicitQueue(index)) {
            pfc::list_t<t_playback_queue_item> queued;
            pm->queue_get_contents(queued);
            const size_t at = QueueView::ExplicitQueueIndex(index);
            if (at >= queued.get_count()) return;
            const auto target = queued[at];
            if (target.m_playlist != SIZE_MAX && target.m_item != SIZE_MAX) {
                pm->set_playing_playlist(target.m_playlist);
                pm->playlist_execute_default_action(target.m_playlist, target.m_item);
            } else if (target.m_handle.is_valid()) {
                metadb_handle_list items;
                items.add_item(target.m_handle);
                const bool wasPlaying = playback_control::get()->is_playing();
                media::PlayNext(items);
                if (wasPlaying) playback_control::get()->start(play_control::track_command_next);
            }
            return;
        }
        const t_size p = media::QueuePlaylist(false);
        if (p == SIZE_MAX || index >= pm->playlist_get_item_count(p)) return;
        pm->set_playing_playlist(p);
        pm->playlist_execute_default_action(p, index);
    };
    queue.onPlayHistory = [](const metadb_handle_ptr& track) {
        // History activation uses the same priority insertion as every other Play Next action.
        metadb_handle_list items;
        items.add_item(track);
        const bool wasPlaying = playback_control::get()->is_playing();
        media::PlayNext(items);
        if (wasPlaying) playback_control::get()->start(play_control::track_command_next);
    };
    queue.onClearHistory = [this] {
        ClearPlayStats(true);
        RefreshPaneQueue();
    };
    queue.onShuffle = [this] {
        if (!media::radio::Active()) SetShuffleRepeat(!m_shuffle, m_repeat); // no shuffle on a station
    };
    queue.onRepeat = [this] { CycleRepeat(); };
    queue.onReorder = [this](const std::vector<size_t>& slots, const std::vector<size_t>& items) {
        std::vector<media::ScheduledUpcomingEntry> order;
        std::unordered_set<size_t> seen;
        bool valid = slots.size() == items.size() && items.size() == m_paneUpcoming.size();
        order.reserve(items.size());
        for (const size_t item : items) {
            const auto found = m_paneUpcoming.find(item);
            if (found == m_paneUpcoming.end() || !seen.insert(item).second) {
                valid = false;
                break;
            }
            order.push_back(found->second);
        }
        if (valid) media::PlaybackScheduler::Get().ReorderUpcomingSnapshot(m_paneScheduleRevision, order);
        // A stale drag is discarded and repainted from the current committed schedule.
        OnCoreQueueChanged();
    };
    queue.onRowMenu = [this](size_t index, POINT screen) {
        std::vector<MenuAction> actions;
        actions.push_back({Tr(L"Remove from Now Playing"), [this, index, revision = m_paneScheduleRevision] {
                               if (media::PlaybackScheduler::Get().ScheduleRevision() != revision) {
                                   OnCoreQueueChanged();
                                   return;
                               }
                               auto pm = playlist_manager::get();
                               if (QueueView::IsExplicitQueue(index)) {
                                   const size_t at = QueueView::ExplicitQueueIndex(index);
                                   if (at < pm->queue_get_count())
                                       pm->queue_remove_mask(pfc::bit_array_one(at));
                                   return;
                               }
                               const t_size p = media::QueuePlaylist(false);
                               if (p == SIZE_MAX || index >= pm->playlist_get_item_count(p)) return;
                               pm->playlist_undo_backup(p);
                               pm->playlist_remove_items(p, pfc::bit_array_one(index));
                           }});
        ShowActionMenu(m_hwnd, screen, actions);
    };
}

void MainWindow::ShowNowPlayingSongMenu(POINT screen) {
    const HWND owner = m_player && m_player->Shown() ? m_player->Hwnd() : m_hwnd;
    metadb_handle_ptr now;
    if (!playback_control::get()->get_now_playing(now)) return;
    RunContextMenu(owner, pfc::list_single_ref_t<metadb_handle_ptr>(now), screen, true);
}

void MainWindow::ShowLyricsMenu(POINT screen) {
    // While the player window shows (the library window is hidden), it owns the menu and dialogs.
    const HWND owner = m_player && m_player->Shown() ? m_player->Hwnd() : m_hwnd;
    metadb_handle_ptr now;
    std::vector<MenuAction> actions;
    if (m_npLyrics && !m_npLyrics->source.empty())
        actions.push_back({Tr(L"Lyric source") + L"  " + Tr(m_npLyrics->source), nullptr});
    if (playback_control::get()->get_now_playing(now)) {
        AddLyricsDocumentActions(actions, owner, now);
        const auto search = [this, now, owner](std::wstring source) {
            ShowLyricsSearch(
                owner, now, std::move(source),
                [this, now, alive = std::weak_ptr<int>(m_alive)](const lyrics::Candidate& candidate) {
                    if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity)
                        return;
                    OnLyrics(lyrics::Service::Get().Select(now, candidate));
                });
        };
        actions.push_back({Tr(L"Search lyrics..."), [search] { search(L""); }});
        std::vector<MenuAction> sources;
        for (const auto& source : lyrics::scripts::Ordered(lyrics::Service::QueryFor(now).provider))
            sources.push_back({Tr(source->Name()), [search, key = source->Key()] { search(key); }});
        if (!sources.empty()) actions.push_back({Tr(L"Search lyric source"), nullptr, std::move(sources)});
        actions.push_back({Tr(L"Load lyrics from file..."), [this, now, owner, alive = std::weak_ptr<int>(m_alive)] {
                               const auto path = PickLyricsFile(owner);
                               if (alive.expired() || path.empty() ||
                                   lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity)
                                   return;
                               const auto result = lyrics::Service::Get().LoadFile(now, path);
                               if (result.status == lyrics::Status::Found)
                                   OnLyrics(result);
                               else
                                   MessageBoxW(owner, Tr(L"The file contains no readable lyrics.").c_str(),
                                               Tr(L"Lyrics").c_str(), MB_OK | MB_ICONINFORMATION);
                           }});
        actions.push_back({Tr(L"Reload lyrics"), [this, now] { RequestLyrics(now, false); }});
        actions.push_back({Tr(L"Search for lyrics again"), [this, now] { RequestLyrics(now, true); }});
        actions.push_back({Tr(L"Use automatic lyrics"), [this, now] {
                               lyrics::Service::Get().UseAutomatic(now);
                               RequestLyrics(now, false);
                           }});
        actions.push_back({});
    }
    // The single line under the song, everywhere it shows; switched back on from here too (the
    // menu also opens on the song's title and artist when the line is hidden).
    const bool lineShown = settings::LyricLineShown();
    actions.push_back({Tr(lineShown ? L"Hide lyric line" : L"Show lyric line"),
                       [this, lineShown] { SetLyricLineShown(!lineShown); }});
    // How the lyrics look, as in the preferences: their size in steps of 10 %, and the side
    // their translations stand on.
    const auto restyle = [this] {
        if (m_pane) m_pane->RestyleLyrics();
    };
    std::vector<MenuAction> size{
        {Tr(L"Larger"), [restyle] {
             settings::SetLyricSizePercent(settings::LyricSizePercent() + 10);
             restyle();
         }},
        {Tr(L"Smaller"), [restyle] {
             settings::SetLyricSizePercent(settings::LyricSizePercent() - 10);
             restyle();
         }},
        {Tr(L"Default size"), [restyle] {
             settings::SetLyricSizePercent(settings::kLyricSize.defaultValue);
             restyle();
         }}};
    actions.push_back({Tr(L"Lyrics text size"), nullptr, std::move(size)});
    const bool swapped = settings::SwapTranslation();
    MenuAction swap{Tr(L"Swap original and translation"), [this, swapped] {
                        settings::SetSwapTranslation(!swapped);
                        OnLyrics(m_lyricsResult);
                    }};
    swap.checked = swapped;
    swap.checkbox = true;
    actions.push_back(std::move(swap));
    // The small line under each lyric: the translation, or the original when swapped.
    const bool hidden = settings::HideTranslation();
    MenuAction hide{Tr(swapped ? L"Hide original" : L"Hide translation"), [this, hidden] {
                        settings::SetHideTranslation(!hidden);
                        OnLyrics(m_lyricsResult);
                    }};
    hide.checked = hidden;
    hide.checkbox = true;
    actions.push_back(std::move(hide));
    if (m_lyricsOpen && owner == m_hwnd) actions.push_back({Tr(L"Close lyrics pane"), [this] { ToggleLyrics(); }});
    ShowActionMenu(owner, screen, actions);
}

// ESLyric's tools for the lyrics on show (owner, 2026-11-13): save (to the tag, the profile,
// the track's folder or a chosen file), select another found lyric, copy, read the file in
// another code page, the document itself, and the timing offset. They act on the lyrics as
// found; the offset is saved with them as [offset:] and copying writes the adjusted times.
void MainWindow::AddLyricsDocumentActions(std::vector<MenuAction>& actions, HWND owner, const metadb_handle_ptr& now) {
    using lyrics_tools::SaveTarget;
    const bool current = lyrics::Service::TrackIdentity(now) == m_lyricsTrackIdentity;
    const auto found = current && m_lyricsResult.status == lyrics::Status::Found ? m_lyricsResult.lyrics : nullptr;
    const int offset = found ? lyrics::Service::Get().Offset(now, *found) : 0;
    const auto alive = std::weak_ptr<int>(m_alive);
    // Each action checks that the song (and its lyrics) are still the ones the menu was for.
    const auto still = [this, alive, now, found] {
        return !alive.expired() && lyrics::Service::TrackIdentity(now) == m_lyricsTrackIdentity &&
               m_lyricsResult.lyrics == found;
    };
    const auto document = [found, offset] {
        if (!found) return std::wstring();
        lyrics::WriteOptions all{true, true, false, true, false};
        return lyrics::AddOffsetTag(found->document.empty() ? lyrics::WriteLrc(*found, all) : found->document, offset);
    };
    const auto save = [owner, now, document, still](SaveTarget target) {
        if (still()) lyrics_tools::Save(owner, now, document(), target);
    };
    const auto disabled = [](MenuAction a, bool off) {
        a.disabled = off;
        return a;
    };

    actions.push_back(disabled({Tr(L"Save lyrics"), [save] { save(SaveTarget::Default); }}, !found));
    std::vector<MenuAction> saveTo;
    for (const auto& [target, label] : {std::pair{SaveTarget::Tag, L"Tag"}, std::pair{SaveTarget::Profile, L"Profile folder"},
                                        std::pair{SaveTarget::TrackFolder, L"Track's folder"},
                                        std::pair{SaveTarget::Browse, L"Browse..."}})
        saveTo.push_back(disabled({Tr(label), [save, target] { save(target); }},
                                  !found || !lyrics_tools::CanSave(now, target)));
    actions.push_back(disabled({Tr(L"Save lyrics to"), nullptr, std::move(saveTo)}, !found));

    // Select lyrics: what the searches for this song found, the shown one checked.
    std::vector<MenuAction> select;
    for (const auto& c : lyrics::Service::Get().Candidates(now)) {
        std::wstring label = c.title.empty() ? Tr(L"(untitled)") : c.title;
        if (!c.artist.empty()) label += L" - " + c.artist;
        if (label.size() > 60) label = label.substr(0, 57) + L"...";
        label += L"  (" + Tr(c.source) + L")";
        MenuAction pick{std::move(label), [this, now, c, alive] {
                            if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity) return;
                            OnLyrics(lyrics::Service::Get().Select(now, c));
                        }};
        pick.checked = found && found->document == c.lrc;
        select.push_back(std::move(pick));
    }
    if (select.empty()) select.push_back(disabled({Tr(L"No other lyrics found yet"), nullptr}, true));
    {
        // None at all, until "Use automatic lyrics" (owner, 2026-11-14).
        MenuAction none{Tr(L"No lyrics"), [this, now, alive] {
                            if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity) return;
                            OnLyrics(lyrics::Service::Get().SelectNone(now));
                        }};
        none.checked = current && m_lyricsResult.status == lyrics::Status::NotFound;
        select.push_back(std::move(none));
    }
    select.push_back({});
    select.push_back({Tr(L"Search lyrics..."), [this, now, owner, alive] {
                          ShowLyricsSearch(owner, now, L"", [this, now, alive](const lyrics::Candidate& candidate) {
                              if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity)
                                  return;
                              OnLyrics(lyrics::Service::Get().Select(now, candidate));
                          });
                      }});
    actions.push_back({Tr(L"Select lyrics"), nullptr, std::move(select)});

    // Copy: with the remembered options at once, or choosing them first. The times are the
    // ones shown (offset applied), the lines the original's.
    const auto copy = [owner, found, offset, still](bool ask) {
        if (!still() || !found) return;
        const int flags = settings::LyricsCopyOptions();
        lyrics::WriteOptions options{(flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0, (flags & 8) != 0,
                                     (flags & 16) != 0};
        if (ask) {
            if (!lyrics_tools::ShowCopyDialog(owner, options, found->karaoke)) return;
            settings::SetLyricsCopyOptions((options.metadata ? 1 : 0) | (options.timestamps ? 2 : 0) |
                                           (options.mergeSame ? 4 : 0) | (options.references ? 8 : 0) |
                                           (options.standard ? 16 : 0));
        }
        lyrics::Lyrics adjusted = *found;
        lyrics::ApplyOffset(adjusted, offset);
        lyrics_tools::CopyText(owner, lyrics::WriteLrc(adjusted, options));
    };
    actions.push_back(disabled({Tr(L"Copy lyrics"), nullptr,
                                {{Tr(L"Copy lyrics"), [copy] { copy(false); }}, {Tr(L"Copy..."), [copy] { copy(true); }}}},
                               !found));

    // Encoding: only lyrics read from a file can be read again.
    std::vector<MenuAction> encodings;
    const auto reencode = [this, now, found, owner, still](unsigned codePage) {
        if (!still()) return;
        const auto result = lyrics::Service::Get().Reencode(now, *found, codePage);
        if (result.status == lyrics::Status::Found)
            OnLyrics(result);
        else
            MessageBoxW(owner, Tr(L"The lyric file could not be read in this encoding.").c_str(), Tr(L"Lyrics").c_str(),
                        MB_OK | MB_ICONINFORMATION);
    };
    MenuAction autoDetect{Tr(L"Auto detect"), [reencode] { reencode(0); }};
    autoDetect.checked = found && found->codePage == 0;
    encodings.push_back(std::move(autoDetect));
    encodings.push_back({});
    for (const auto& [codePage, name] : lyrics_tools::CodePages()) {
        MenuAction item{name, [reencode, cp = codePage] { reencode(cp); }};
        item.checked = found && found->codePage == codePage;
        encodings.push_back(std::move(item));
    }
    actions.push_back(disabled({Tr(L"Lyrics encoding"), nullptr, std::move(encodings)}, !found || found->file.empty()));

    actions.push_back(disabled({Tr(L"Lyric info"), [owner, found, offset, still] {
                                    if (!still()) return;
                                    std::wstring header = Tr(L"Source") + L": " +
                                                          (found->source.empty() ? L"-" : Tr(found->source));
                                    if (!found->file.empty()) header += L"\n" + Tr(L"File") + L": " + found->file;
                                    if (!found->file.empty())
                                        header += L"\n" + Tr(L"Encoding") + L": " +
                                                  (found->codePage ? std::to_wstring(found->codePage) : Tr(L"Auto detect"));
                                    header += L"\n" + Tr(L"Offset") + L": " + std::to_wstring(offset) + L" ms";
                                    lyrics_tools::ShowTextWindow(owner, Tr(L"Lyric info"), header, found->document);
                                }},
                               !found));

    // Offset in steps of 100 ms or 1 s, or as typed; positive shows the lyrics earlier ([offset:]).
    const auto setOffset = [this, now, found, still](int ms) {
        if (!still() || !found) return;
        lyrics::Service::Get().SetOffset(now, *found, ms);
        OnLyrics(m_lyricsResult);
    };
    std::vector<MenuAction> offsets{
        {Tr(L"+100 ms (earlier)"), [setOffset, offset] { setOffset(offset + 100); }},
        {Tr(L"-100 ms (later)"), [setOffset, offset] { setOffset(offset - 100); }},
        {Tr(L"+1 s (earlier)"), [setOffset, offset] { setOffset(offset + 1000); }},
        {Tr(L"-1 s (later)"), [setOffset, offset] { setOffset(offset - 1000); }},
        {Tr(L"Custom offset..."), [setOffset, offset, owner] {
             int ms = offset;
             if (lyrics_tools::AskOffset(owner, ms)) setOffset(ms);
         }},
        disabled({Tr(L"Reset offset"), [setOffset] { setOffset(0); }}, offset == 0)};
    std::wstring offsetLabel = Tr(L"Adjust offset");
    if (offset != 0) offsetLabel += L"  (" + std::wstring(offset > 0 ? L"+" : L"") + std::to_wstring(offset) + L" ms)";
    actions.push_back(disabled({std::move(offsetLabel), nullptr, std::move(offsets)}, !found || !found->synced));
    actions.push_back({});
}

void MainWindow::SetLyricLineShown(bool shown) {
    settings::SetLyricLineShown(shown);
    m_npLyricLine = -2;
    RepaintNowPlaying();
    if (m_player) m_player->OnLyricLineSetting();
}

int MainWindow::NowPlayingPartAt(POINT pt) const {
    if (PtInRect(&m_npTitleRect, pt)) return 1;
    if (PtInRect(&m_npArtistRect, pt)) return 2;
    if (PtInRect(&m_npStarRect, pt)) return 4;
    if (const RECT d = DeviceRect(); PtInRect(&d, pt)) return 3;
    if (PtInRect(&m_npArtRect, pt)) return 5; // like Apple Music: the cover opens the mini player
    return 0;
}

void MainWindow::UpdateNowPlayingHover(POINT pt) {
    const int part = NowPlayingPartAt(pt);
    if (part == m_npHot) return;
    const int old = m_npHot;
    m_npHot = part;
    if (old == 3 || part == 3) RepaintStreamInfo();
    if (old == 1 || old == 2 || old == 4 || old == 5 || part == 1 || part == 2 || part == 4 || part == 5)
        RepaintNowPlaying();
}

void MainWindow::OnNowPlayingClick(int part) {
    switch (part) {
    case 5:
        ShowPlayer(false);
        break;
    case 1:
        JumpToPlaying();
        break;
    case 2: { // the playing song's artist: its provider's artist page or the local group
        metadb_handle_ptr now;
        if (!playback_control::get()->get_now_playing(now)) break;
        metadb_handle_list items;
        items.add_item(now);
        auto links = BuildArtistLinks(items);
        if (links.size() == 1) {
            links.front().run();
        } else if (links.size() > 1) {
            RECT anchor = m_npArtistRect;
            MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
            ShowActionMenu(m_hwnd, anchor, links);
        }
        break;
    }
    case 3:
        ShowDeviceMenu();
        break;
    case 4: { // Favorite star next to the title
        metadb_handle_ptr now;
        if (playback_control::get()->get_now_playing(now)) {
            auto* provider = stream::ProviderOf(now);
            const bool next = !media::IsFavorite(now);
            if (!provider || provider->CanSetFavorite(next)) media::ToggleFavorite(now);
        }
        UpdateFavoriteState();
        break;
    }
    default:
        break;
    }
}

void MainWindow::JumpToPlaying() {
    metadb_handle_ptr now;
    if (!playback_control::get()->get_now_playing(now)) return;
    size_t row = m_source ? m_source->FindRow(now) : SIZE_MAX;
    if (row == SIZE_MAX) { // not on this page: open the playlist it plays from
        t_size playlist = SIZE_MAX, index = SIZE_MAX;
        if (!playlist_manager::get()->get_playing_item_location(&playlist, &index)) return;
        View v;
        v.library = View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = playlist;
        Navigate(v, true);
        row = m_source ? m_source->FindRow(now) : SIZE_MAX;
    }
    if (row == SIZE_MAX) return;
    m_source->SelectOnly(row);
    m_source->SetFocus(row);
    m_list->EnsureVisible(row);
    m_list->Refresh();
}

void MainWindow::ShowDeviceMenu() {
    struct Device {
        GUID output, device;
    };
    std::vector<Device> devices;
    std::vector<MenuAction> actions;
    size_t current = SIZE_MAX;
    try {
        const auto cfg = output_manager::get()->getCoreConfig();
        output_manager_v2::get()->listDevices([&](const char* name, const GUID& output, const GUID& device) {
            if (output == cfg.m_output && device == cfg.m_device) current = devices.size();
            devices.push_back({output, device});
            actions.push_back({Wide(name), [this, output, device] {
                                   output_manager_v2::get()->setCoreConfigDevice(output, device);
                                   RefreshDeviceName();
                                   RepaintStreamInfo();
                               }});
        });
    } catch (const std::exception& e) {
        popup_message::g_show(e.what(), "Output device");
        return;
    }
    RECT d = DeviceRect();
    MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&d), 2);
    ShowActionMenu(m_hwnd, d, actions, current);
}

void MainWindow::RepaintBreadcrumb() {
    auto path = BuildCrumbPath(m_view);
    // The path in the log as it changes, so every page's place can be read back.
    std::wstring text = L"path:";
    for (const auto& level : path)
        if (!level.text.empty()) text += L" > " + (level.literal ? level.text : Tr(level.text));
    if (text != m_loggedPath) {
        m_loggedPath = text;
        core::Log(text);
    }
    m_crumbBar->SetPath(std::move(path));
    UpdateLayout();
    // The view options glyph follows the current source, which asynchronous loads replace.
    if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
}

std::vector<MenuAction> MainWindow::LibraryMenu() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    View all;
    all.library = View::Library::All;
    all.kind = View::Kind::AllMusic;
    View local;
    local.library = View::Library::Local;
    local.kind = View::Kind::AllMusic;
    // The navigation pane's top level, in its order (NavPane::Rebuild): the root of every path.
    std::vector<MenuAction> out;
    if (std::any_of(stream::Providers().begin(), stream::Providers().end(),
                    [](const stream::Provider* p) { return p->Can(stream::Capability::Home); })) {
        View home;
        home.library = View::Library::All;
        home.kind = View::Kind::Home;
        out.push_back({L"Home", go(home)});
    }
    if (const t_size queue = media::QueuePlaylist(false); queue != SIZE_MAX) {
        View now;
        now.library = View::Library::All;
        now.kind = View::Kind::Playlist;
        now.playlist = queue;
        out.push_back({L"Now Playing", go(now)});
    }
    out.push_back({L"All Music", go(all)});
    out.push_back({L"Local Music", go(local)});
    for (auto* provider : stream::Providers())
        out.push_back({provider->Name(), go(ProviderLandingView(*provider))});
    return out;
}

std::vector<MenuAction> MainWindow::AllSectionsMenu() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    View favorites;
    favorites.library = View::Library::All;
    favorites.kind = View::Kind::AllFavorites;
    View recent;
    recent.library = View::Library::All;
    recent.kind = View::Kind::AllRecentPlayed;
    View most;
    most.library = View::Library::All;
    most.kind = View::Kind::AllMostPlayed;
    View downloads;
    downloads.library = View::Library::All;
    downloads.kind = View::Kind::Downloads;
    View playlists;
    playlists.library = View::Library::All;
    playlists.kind = View::Kind::Playlists;
    return {{L"Favorites", go(favorites)},
            {L"Recently Played", go(recent)},
            {L"Most Played", go(most)},
            {L"Downloads", go(downloads)},
            {L"Playlists", go(playlists)}};
}

// A local album's or (not the queue's) playlist's page: Play on the command bar.
bool MainWindow::LocalPlayShown() const {
    if (m_page || !m_source) return false;
    return (m_view.kind == View::Kind::Group && m_view.group == media::GroupKind::Album) ||
           (m_view.kind == View::Kind::Playlist && !media::IsQueue(m_view.playlist));
}

std::vector<MenuAction> MainWindow::LocalSectionsMenu() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    const auto kinds = m_nav->Categories();
    const int count = static_cast<int>(kinds.size());
    const int at = std::clamp(m_nav->SmartPosition(), 0, count);
    std::vector<MenuAction> out;
    for (int i = 0; i <= count; ++i) {
        if (i == at) { // the same interleaving the navigation pane shows
            View smart;
            smart.library = View::Library::Local;
            smart.kind = View::Kind::SmartPlaylists;
            out.push_back({L"Smart playlists", go(smart)});
        }
        if (i < count) {
            View v;
            v.library = View::Library::Local;
            v.kind = View::Kind::Category;
            v.group = kinds[static_cast<size_t>(i)];
            out.push_back({CategoryLabel(v.group), go(v)});
        }
    }
    return out;
}

std::vector<MenuAction> MainWindow::ProviderSectionsMenu(stream::Provider& provider) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& section : provider.Sections())
        if (!section.id.empty()) out.push_back({Tr(section.label), go(SectionView(provider, section))});
    if (provider.Can(stream::Capability::CatalogSearch))
        out.push_back({provider.NamedText(L"Search {0}"), [this, key = std::wstring(provider.Key())] {
                           BeginSearch({SearchScope::Kind::Provider, key});
                       }});
    return out;
}

std::vector<MenuAction> MainWindow::ShelfMenu(stream::Provider& provider,
                                              const stream::NavigationSection& section) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& playlist : provider.SectionPlaylists(section.id)) {
        if (out.size() >= kMenuItemLimit) break;
        out.push_back({playlist.name.empty() ? provider.Text(L"Unnamed playlist") : playlist.name,
                       go(PlaylistView(provider, playlist.id, playlist.name, section.shelf, section.id))});
    }
    if (out.empty()) out.push_back({Tr(section.label), go(SectionView(provider, section))});
    return out;
}

std::vector<MenuAction> MainWindow::FollowedArtistsMenu(stream::Provider& provider,
                                                        const stream::Id& sectionId) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& artist : provider.FollowedArtists()) {
        if (out.size() >= kMenuItemLimit) break;
        out.push_back({artist.name, go(ArtistView(provider, artist.id, artist.name, sectionId))});
    }
    if (out.empty()) {
        const auto section = FindProviderSection(provider, sectionId);
        if (section) out.push_back({Tr(section->label), go(SectionView(provider, *section))});
    }
    return out;
}

std::vector<MenuAction> MainWindow::SavedAlbumsMenu(stream::Provider& provider, const stream::Id& sectionId) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& album : provider.SavedAlbums()) {
        if (out.size() >= kMenuItemLimit) break;
        out.push_back({album.name, go(AlbumView(provider, album.id, album.name, sectionId))});
    }
    if (out.empty()) {
        const auto section = FindProviderSection(provider, sectionId);
        if (section) out.push_back({Tr(section->label), go(SectionView(provider, *section))});
    }
    return out;
}

// A provider's levels: the library, then its section and the page's subject.
void MainWindow::AddRemoteCrumbs(std::vector<Crumb>& path, stream::Provider& provider, const View& view) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    stream::Provider* p = &provider;
    // A user's name is shown as is; an unnamed page gets a translated fallback.
    auto named = [&path](const std::wstring& name, const wchar_t* fallback, std::function<void()> open) {
        path.push_back(name.empty() ? Crumb{fallback, std::move(open), {}}
                                    : Crumb{name, std::move(open), {}, true});
    };
    auto addSection = [&](const stream::NavigationSection& section) {
        std::function<std::vector<MenuAction>()> children;
        if (section.kind == stream::SectionKind::PlaylistShelf)
            children = [this, p, section] { return ShelfMenu(*p, section); };
        else if (section.kind == stream::SectionKind::Artists)
            children = [this, p, id = section.id] { return FollowedArtistsMenu(*p, id); };
        else if (section.kind == stream::SectionKind::Albums)
            children = [this, p, id = section.id] { return SavedAlbumsMenu(*p, id); };
        path.push_back({section.label, go(SectionView(provider, section)), std::move(children)});
    };
    const auto section = SectionForView(provider, view);
    path.push_back(
        {provider.Name(), go(ProviderLandingView(provider)), [this, p] { return ProviderSectionsMenu(*p); }});
    switch (view.kind) {
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
        if (section) addSection(*section);
        break;
    case View::Kind::RemoteGroupTracks:
        if (section) addSection(*section);
        named(view.name, L"Group", go(view));
        break;
    case View::Kind::RemoteShelf:
        if (section) addSection(*section);
        break;
    case View::Kind::RemotePlaylist:
        if (section) addSection(*section);
        named(view.name, L"Playlist", go(view));
        break;
    case View::Kind::RemoteArtists:
        if (section) addSection(*section);
        break;
    case View::Kind::RemoteAlbums:
        if (section) addSection(*section);
        break;
    case View::Kind::RemoteArtist:
        if (section) addSection(*section);
        named(view.name, L"Artist", go(view));
        break;
    case View::Kind::RemoteAlbum:
        if (section) addSection(*section);
        named(view.name, L"Album", go(view));
        break;
    case View::Kind::RemoteTopTracks:
        if (section) addSection(*section);
        named(view.name, L"Artist",
              go(ArtistView(provider, view.remoteId, view.name, view.sectionId)));
        path.push_back({provider.ArtistTracksLabel(view.remoteId), go(view), {}});
        break;
    case View::Kind::Search:
        // As the pages opened from it name it: "Search results (term)".
        path.push_back({Tr(L"Search results") + L" (" + view.remoteId + L")", go(view), {}, true});
        break;
    case View::Kind::RemoteSong:
        named(view.name, L"Song", go(view));
        break;
    default:
        break;
    }
}

std::vector<MenuAction> MainWindow::PlaylistListMenu(bool smart) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    auto pm = playlist_manager::get();
    auto isSmart = [](t_size p) { return p == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(p); };
    for (t_size i = 0, n = pm->get_playlist_count(); i < n; ++i) {
        if (media::IsQueue(i) || isSmart(i) != smart) continue;
        pfc::string8 name;
        pm->playlist_get_name(i, name);
        View v;
        v.library = smart ? View::Library::Local : View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = i;
        out.push_back({Wide(name), go(v)});
    }
    if (out.empty()) {
        View page;
        page.library = smart ? View::Library::Local : View::Library::All;
        page.kind = smart ? View::Kind::SmartPlaylists : View::Kind::Playlists;
        out.push_back({smart ? L"Smart playlists" : L"Playlists", go(page)});
    }
    return out;
}

std::vector<MenuAction> MainWindow::GroupListMenu(media::GroupKind kind) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& g : m_library.Groups(kind)) {
        if (out.size() >= kMenuItemLimit) break; // a menu, not a list view
        View v;
        v.kind = View::Kind::Group;
        v.group = kind;
        v.key = g.key;
        v.name = g.name;
        out.push_back({g.name.empty() ? L"Unknown" : g.name, go(v)});
    }
    if (out.empty()) {
        View page;
        page.kind = View::Kind::Category;
        page.group = kind;
        out.push_back({CategoryLabel(kind), go(page)});
    }
    return out;
}


// A page's own place: its library or section from the root, as the navigation pane has it.
std::vector<MainWindow::Crumb> MainWindow::PlacePath(const View& view) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<Crumb> path;
    path.push_back({L"", {}, [this] { return LibraryMenu(); }});
    // Playlists: the queue is a root page, smart playlists sit flat under Local Music,
    // and the regular ones keep their section under All Music.
    if (view.kind == View::Kind::AudioCd) { // Local Music > Discs > the disc
        View home;
        home.library = View::Library::Local;
        home.kind = View::Kind::AllMusic;
        path.push_back({L"Local Music", go(home), [this] { return LocalSectionsMenu(); }});
        path.push_back({L"Discs", {}, [this] {
                            std::vector<MenuAction> drives;
                            for (const auto& drive : cd::Drives()) {
                                View v;
                                v.library = View::Library::Local;
                                v.kind = View::Kind::AudioCd;
                                v.playlist = drive.drive;
                                drives.push_back({cd::DriveTitle(drive), [this, v] { Navigate(v, true); }});
                            }
                            return drives;
                        }});
        path.push_back({view.name, go(view), {}, true});
        return path;
    }
    if (view.kind == View::Kind::Home || view.kind == View::Kind::RemoteHomeList) {
        View home;
        home.library = View::Library::All;
        home.kind = View::Kind::Home;
        path.push_back({L"Home", go(home), {}});
        if (view.kind == View::Kind::RemoteHomeList) path.push_back({view.name, go(view), {}, true});
        return path;
    }
    if (view.kind == View::Kind::Playlist) {
        if (media::IsQueue(view.playlist)) {
            path.push_back({L"Now Playing", go(view), {}});
            return path;
        }
        const bool smart =
            view.playlist == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(view.playlist);
        if (smart) {
            View home;
            home.library = View::Library::Local;
            home.kind = View::Kind::AllMusic;
            path.push_back({L"Local Music", go(home), [this] { return LocalSectionsMenu(); }});
            View section;
            section.library = View::Library::Local;
            section.kind = View::Kind::SmartPlaylists;
            path.push_back({L"Smart playlists", go(section), [this] { return PlaylistListMenu(true); }});
        } else {
            View home;
            home.library = View::Library::All;
            home.kind = View::Kind::AllMusic;
            path.push_back({L"All Music", go(home), [this] { return AllSectionsMenu(); }});
            View section;
            section.library = View::Library::All;
            section.kind = View::Kind::Playlists;
            path.push_back({L"Playlists", go(section), [this] { return PlaylistListMenu(false); }});
        }
        path.push_back({view.name, go(view), {}, true});
        return path;
    }
    switch (view.library) {
    case View::Library::All: {
        View home;
        home.library = View::Library::All;
        home.kind = View::Kind::AllMusic;
        path.push_back({L"All Music", go(home), [this] { return AllSectionsMenu(); }});
        if (view.kind == View::Kind::AllFavorites) {
            path.push_back({L"Favorites", go(view), {}});
        } else if (view.kind == View::Kind::AllRecentPlayed) {
            path.push_back({L"Recently Played", go(view), {}});
        } else if (view.kind == View::Kind::AllMostPlayed) {
            path.push_back({L"Most Played", go(view), {}});
        } else if (view.kind == View::Kind::Downloads) {
            path.push_back({L"Downloads", go(view), {}});
        } else if (view.kind == View::Kind::Search) {
            path.push_back({Tr(L"Search results") + L" (" + view.remoteId + L")", go(view), {}, true});
        } else if (view.kind == View::Kind::Playlists) {
            path.push_back({L"Playlists", go(view), [this] { return PlaylistListMenu(false); }});
        }
        break;
    }
    case View::Library::Local: {
        View home;
        home.library = View::Library::Local;
        home.kind = View::Kind::AllMusic;
        path.push_back({L"Local Music", go(home), [this] { return LocalSectionsMenu(); }});
        if (view.kind == View::Kind::SmartPlaylists) {
            path.push_back({L"Smart playlists", go(view), [this] { return PlaylistListMenu(true); }});
        } else if (view.kind == View::Kind::Category) {
            path.push_back({CategoryLabel(view.group), go(view),
                            [this, kind = view.group] { return GroupListMenu(kind); }});
        } else if (view.kind == View::Kind::Group) {
            View category;
            category.library = View::Library::Local;
            category.kind = View::Kind::Category;
            category.group = view.group;
            path.push_back({CategoryLabel(view.group), go(category),
                            [this, kind = view.group] { return GroupListMenu(kind); }});
            path.push_back({view.name, go(view), {}, true});
        } else if (view.kind == View::Kind::Search) {
            path.push_back({Tr(L"Search results") + L" (" + view.remoteId + L")", go(view), {}, true});
        }
        break;
    }
    case View::Library::Remote:
        if (auto* provider = stream::FindProvider(view.provider)) AddRemoteCrumbs(path, *provider, view);
        break;
    }
    return path;
}

// The breadcrumb: the place of the page it was opened from (and so on up), then this page; or,
// opened from no page, its own place. A page adds only its own level: everything above it is
// its opener's path, unchanged (owner, 2026-11-20: a playlist opened from Home showed "my
// playlists" above it).
std::vector<MainWindow::Crumb> MainWindow::BuildCrumbPath(const View& view) {
    if (!view.from) return PlacePath(view);
    std::vector<Crumb> path = BuildCrumbPath(*view.from);
    View own = view;
    own.from = nullptr;
    auto place = PlacePath(own);
    if (place.size() < 2) return path;
    Crumb level = std::move(place.back());
    level.open = [this, view] { Navigate(view, true); }; // back here, with its own path
    path.push_back(std::move(level));
    return path;
}

// ---- Navigation --------------------------------------------------------------------------

namespace {
// A section of the navigation pane (or a page it lists directly): it always stands in its own
// place, whatever page links to it.
bool SectionPage(const View& view) {
    switch (view.kind) {
    case View::Kind::Home:
    case View::Kind::AllMusic:
    case View::Kind::Playlists:
    case View::Kind::SmartPlaylists:
    case View::Kind::AllFavorites:
    case View::Kind::AllRecentPlayed:
    case View::Kind::AllMostPlayed:
    case View::Kind::Downloads:
    case View::Kind::Category:
    case View::Kind::AudioCd:
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteShelf:
    case View::Kind::RemoteArtists:
    case View::Kind::RemoteAlbums:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
        return true;
    case View::Kind::Playlist:
        return media::IsQueue(view.playlist);
    default:
        return false;
    }
}
} // namespace

void MainWindow::Open(const View& view) {
    View next = view;
    if (!next.from && !SectionPage(next)) {
        // Opened from the page on screen. A page already on its path (a link back up) is
        // reached the way it was: the path is cut there, never looped.
        std::shared_ptr<const View> from = std::make_shared<View>(m_view);
        for (auto at = from; at; at = at->from)
            if (SameView(*at, next)) {
                from = at->from;
                break;
            }
        next.from = std::move(from);
    }
    Navigate(next, true);
}

void MainWindow::Replace(const View& view) {
    View next = view;
    if (!next.from) next.from = m_view.from;
    Navigate(next, false);
}


// The page that was open when the window closed, and its top row:
// version, library, kind, playlist, name, group, key, provider, shelf, shelfKnown,
// remoteId, sectionId, groupId, top row; tab-separated. (Where it was opened from is not kept:
// restored, a page shows its own place.)
void MainWindow::SaveLastView() {
    if (!m_list) return;
    const size_t top = m_list->TopRow();
    std::wstring text = L"4";
    auto add = [&text](const std::wstring& value) {
        text += L'\t';
        text += CleanConfigField(value);
    };
    add(std::to_wstring(static_cast<int>(m_view.library)));
    add(std::to_wstring(static_cast<int>(m_view.kind)));
    add(std::to_wstring(m_view.playlist));
    add(m_view.name);
    add(std::to_wstring(static_cast<int>(m_view.group)));
    add(m_view.key);
    add(m_view.provider);
    add(std::to_wstring(static_cast<int>(m_view.shelf)));
    add(m_view.shelfKnown ? L"1" : L"0");
    add(m_view.remoteId);
    add(m_view.sectionId);
    add(m_view.groupId);
    add(top == SIZE_MAX ? L"-1" : std::to_wstring(top));
    cfg_lastView.set(pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr());
}

bool MainWindow::LoadLastView(View& view, size_t& top) {
    const pfc::string8 stored = cfg_lastView.get();
    if (stored.is_empty()) return false;
    const auto f = SplitConfigFields(pfc::stringcvt::string_wide_from_utf8(stored.get_ptr()).get_ptr());
    if (f.empty() || f[0] != L"4" || f.size() < 14u) return false;
    auto inRange = [](const std::wstring& text, int last) {
        const int value = _wtoi(text.c_str());
        return value >= 0 && value <= last ? value : -1;
    };
    const int library = inRange(f[1], static_cast<int>(View::Library::Remote));
    int kind = inRange(f[2], static_cast<int>(View::Kind::RemoteGroupTracks));
    if (_wtoi(f[2].c_str()) == static_cast<int>(View::Kind::Home)) kind = static_cast<int>(View::Kind::Home);
    const int group = inRange(f[5], static_cast<int>(media::kGroupKinds) - 1);
    const int shelf = inRange(f[8], static_cast<int>(stream::Shelf::Recommended));
    if (library < 0 || kind < 0 || group < 0 || shelf < 0) return false;
    View v;
    v.library = static_cast<View::Library>(library);
    v.kind = static_cast<View::Kind>(kind);
    v.name = f[4];
    v.group = static_cast<media::GroupKind>(group);
    v.key = f[6];
    v.provider = f[7];
    v.shelf = static_cast<stream::Shelf>(shelf);
    v.shelfKnown = f[9] == L"1";
    v.remoteId = f[10];
    v.sectionId = f[11];
    v.groupId = f[12];
    const std::wstring& scrollText = f[13];
    const long long scroll = _wcstoi64(scrollText.c_str(), nullptr, 10);
    top = scroll < 0 ? SIZE_MAX : static_cast<size_t>(scroll);
    if (v.kind == View::Kind::Group && v.key.empty()) return false;
    if (v.kind != View::Kind::Home && v.IsRemote() != (v.kind >= View::Kind::RemoteFavorites)) return false;
    if (v.IsRemote()) {
        auto* provider = stream::FindProvider(v.provider);
        if (!provider) return false;
        if (v.kind == View::Kind::Search && !provider->Can(stream::Capability::CatalogSearch)) return false;
        v = NormalizeRemoteView(*provider, std::move(v), false);
    }
    switch (v.kind) {
    case View::Kind::RemotePlaylist:
    case View::Kind::RemoteAlbum:
    case View::Kind::RemoteArtist:
    case View::Kind::RemoteTopTracks:
    case View::Kind::Search:
    case View::Kind::RemoteSong:
        if (v.remoteId.empty()) return false;
        break;
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
        if (v.sectionId.empty()) return false;
        break;
    case View::Kind::RemoteGroupTracks:
        if (v.sectionId.empty() || v.groupId.empty()) return false;
        break;
    default:
        break;
    }
    if (v.kind == View::Kind::Playlist) {
        // By name first: reordering playlists must not open a different one.
        auto pm = playlist_manager::get();
        t_size p = v.name.empty()
                       ? SIZE_MAX
                       : pm->find_playlist(pfc::stringcvt::string_utf8_from_wide(v.name.c_str()).get_ptr());
        const auto index = static_cast<t_size>(_wcstoui64(f[3].c_str(), nullptr, 10));
        if (p == SIZE_MAX && index < pm->get_playlist_count()) p = index;
        if (p == SIZE_MAX) return false;
        v.playlist = p;
        pfc::string8 name;
        pm->playlist_get_name(p, name);
        v.name = Wide(name);
    }
    view = std::move(v);
    return true;
}

void MainWindow::ApplyPendingScroll() {
    if (m_pendingScrollKey.empty() || !m_list || !m_source || m_source->Count() == 0) return;
    // Back at the same distance from the top: restored by its row alone, the first row of a group
    // came back at the very top and the group's header (or the album line above it) was hidden.
    if (auto it = m_scrollPositions.find(m_pendingScrollKey); it != m_scrollPositions.end()) {
        m_list->SetShelfPositions(it->second.shelves);
        if (it->second.atTop)
            m_list->ScrollToTop();
        else
            m_list->ScrollToRow(std::min(it->second.top, m_source->Count() - 1), it->second.offset);
    }
    m_pendingScrollKey.clear();
}

void MainWindow::Navigate(const View& requested, bool push) {
    View view = requested;
    auto pm = playlist_manager::get();
    stream::Provider* provider = view.IsRemote() ? stream::FindProvider(view.provider) : nullptr;
    if ((view.kind == View::Kind::Playlist && view.playlist >= pm->get_playlist_count()) ||
        (view.IsRemote() && !provider)) {
        view = {}; // the page is gone: Local Music
    }
    // Resolve older or now-missing section ids before identity and scroll bookkeeping.
    if (provider) view = NormalizeRemoteView(*provider, std::move(view), false);
    // A link to the page on screen (the playing song's artist while on that artist) leaves it
    // as it is; Refresh asks for a reload with m_forcePageRefresh.
    if (push && !m_forcePageRefresh && SameView(view, m_view) && (m_page || m_source)) return;
    // Remember where the page we are leaving was scrolled; ApplyPendingScroll puts the new
    // source back there (also after an asynchronous page replaces its loading placeholder).
    if (m_list && m_source && m_source->Count()) {
        const size_t top = m_list->TopRow();
        if (top != SIZE_MAX)
            m_scrollPositions[ViewKey(m_view)] = {m_view.IsRemote() ? m_view.provider : stream::Id{}, top,
                                                  m_list->RowOffset(top), m_list->AtTop(),
                                                  m_list->ShelfPositions()};
    }
    m_pendingScrollKey = ViewKey(view);
    m_pendingSelect = view.select; // "View album" from a song: that song, once its row shows
    view.select = nullptr;
    const bool sameView = SameView(view, m_view);
    m_detailHeaderSize = {}; // repaint the band for this page, not the previous one
    if (!sameView) {
        m_detailBriefExpanded = false;
        m_detailBriefScroll = 0;
        m_detailBriefHot = 0;
        m_detailBriefPressed = 0;
        m_detailBriefBlend.fill(0.0f);
    }
    if (push && !sameView) {
        // A new view clears the search, like WMP.
        m_filter.clear();
        if (m_search) {
            auto keep = std::move(m_search->onChange);
            m_search->Clear();
            m_search->onChange = std::move(keep);
        }
    }
    // A search page shows its scope and term in the box; another page the list scope.
    if (m_search && (!sameView || view.kind == View::Kind::Search)) SyncSearchBox(view);
    // The page on screen goes first; results still in flight for it are dropped with it.
    m_page.reset();
    const bool reuse = !push && !m_forcePageRefresh;
    m_forcePageRefresh = false;
    m_view = view; // pages read the current view (its saved style) while they start

    std::unique_ptr<media::ListSource> source;
    NavPane::Target target = NavPane::Target::None;
    size_t navIndex = 0;
    stream::Id navId;
    if (provider) {
        m_page = MakeRemotePage(*this, *provider, view);
        switch (view.kind) {
        case View::Kind::RemoteFavorites:
        case View::Kind::RemoteArtists:
        case View::Kind::RemoteAlbums:
        case View::Kind::RemoteShelf:
        case View::Kind::RemoteArtist:
        case View::Kind::RemoteAlbum:
        case View::Kind::RemoteTopTracks:
        case View::Kind::RemoteSectionTracks:
        case View::Kind::RemoteGroups:
        case View::Kind::RemoteGroupTracks:
            target = NavPane::Target::RemoteSection;
            navId = view.sectionId;
            break;
        case View::Kind::Search:
            target = NavPane::Target::LibraryRemote;
            break;
        case View::Kind::RemoteHomeList: // opened from Home
            target = NavPane::Target::Home;
            break;
        case View::Kind::RemotePlaylist:
            target = NavPane::Target::RemotePlaylist;
            navId = view.remoteId;
            break;
        default:
            break;
        }
    }
    switch (view.kind) {
    case View::Kind::Search:
        m_page = MakeSearchPage(*this, view);
        if (!provider)
            target = view.library == View::Library::All ? NavPane::Target::LibraryAll : NavPane::Target::LibraryLocal;
        break;
    case View::Kind::AllFavorites:
        m_page = MakeAllFavoritesPage(*this, view);
        target = NavPane::Target::AllFavorites;
        break;
    case View::Kind::AudioCd: {
        const wchar_t letter = static_cast<wchar_t>(view.playlist);
        // A drive not read yet (auto-loading off) is read when shown; one found empty only when
        // its row is clicked again (OnNavItem), not on every refresh this page gets.
        if (const auto* unread = cd::FindDrive(letter); unread && unread->state == cd::CdDrive::State::Unread)
            cd::ReadDrive(letter);
        const auto* drive = cd::FindDrive(letter);
        const bool reading = drive && drive->state == cd::CdDrive::State::Reading;
        view.name = m_view.name = drive ? cd::DriveTitle(*drive) : Tr(L"Audio CD");
        source = std::make_unique<DiscSource>(
            drive && drive->state == cd::CdDrive::State::Loaded ? drive->tracks : metadb_handle_list(),
            reading ? L"Reading the disc..." : L"No disc in the drive.");
        target = NavPane::Target::AudioCd;
        navIndex = view.playlist;
        break;
    }
    case View::Kind::AllRecentPlayed: // songs, albums and playlists tabs (ui/recent_page.h)
        m_page = MakeRecentPage(*this, view);
        target = NavPane::Target::AllRecentPlayed;
        break;
    case View::Kind::Home: // the sources' home pages (ui/home_page.h)
        m_page = MakeHomePage(*this, view);
        target = NavPane::Target::Home;
        break;
    case View::Kind::Downloads: // downloading and downloaded tabs (ui/downloads_page.h)
        m_page = MakeDownloadsPage(*this, view);
        target = NavPane::Target::AllDownloads;
        break;
    case View::Kind::AllMostPlayed: {
        const bool recent = view.kind == View::Kind::AllRecentPlayed;
        const size_t rows = static_cast<size_t>(settings::PlayHistoryRows());
        source = std::make_unique<PlayStatsSource>(
            recent ? media::PlayStats::Get().Recent(rows) : media::PlayStats::Get().MostPlayed(rows),
            recent ? L"No recently played songs." : L"No most played songs.");
        target = recent ? NavPane::Target::AllRecentPlayed : NavPane::Target::AllMostPlayed;
        break;
    }
    case View::Kind::Playlist: {
        pfc::string8 name;
        pm->playlist_get_name(view.playlist, name);
        view.name = m_view.name = Wide(name);
        KeepQueueActive();
        source = std::make_unique<media::PlaylistSource>(view.playlist);
        target = media::IsQueue(view.playlist) ? NavPane::Target::Queue : NavPane::Target::Playlist;
        navIndex = view.playlist;
        break;
    }
    case View::Kind::AllMusic: {
        std::vector<size_t> all(m_library.Tracks().size());
        for (size_t i = 0; i < all.size(); ++i)
            all[i] = i;
        source = std::make_unique<media::TrackSource>(
            m_library, std::move(all),
            L"Your library is empty. Use Organize ▸ Manage music library to add your music folders.");
        target =
            view.library == View::Library::All ? NavPane::Target::LibraryAll : NavPane::Target::LibraryLocal;
        break;
    }
    case View::Kind::Playlists:        // WMP's Playlists page: every playlist as an icon
    case View::Kind::SmartPlaylists: { // the same for Favorites and the auto playlists
        const bool smart = view.kind == View::Kind::SmartPlaylists;
        source = std::make_unique<media::PlaylistsSource>(
            smart,
            [this, smart](size_t playlist) {
                View v;
                v.library = smart ? View::Library::Local : View::Library::All;
                v.kind = View::Kind::Playlist;
                v.playlist = playlist;
                Navigate(v, true);
            },
            [this](size_t playlist, POINT screen) { ShowPlaylistMenu(playlist, screen); });
        target = smart ? NavPane::Target::SmartHeader : NavPane::Target::PlaylistsHeader;
        break;
    }
    case View::Kind::Category: {
        const auto kind = view.group;
        source = std::make_unique<media::GroupSource>(m_library, kind, [this, kind](const media::Group& g) {
            View v;
            v.kind = View::Kind::Group;
            v.group = kind;
            v.key = g.key;
            v.name = g.secondary.empty() ? g.name : g.name + L" (" + g.secondary + L")";
            Navigate(v, true);
        });
        target = NavPane::Target::Category;
        navIndex = static_cast<size_t>(kind);
        break;
    }
    case View::Kind::Group: {
        const media::Group* g = m_library.FindGroup(view.group, view.key);
        source = std::make_unique<media::TrackSource>(m_library, g ? g->tracks : std::vector<size_t>{},
                                                      L"No songs.");
        target = NavPane::Target::Category;
        navIndex = static_cast<size_t>(view.group);
        break;
    }
    default:
        break; // a provider's page
    }
    m_nav->Select(target, navIndex, provider, navId);
    if (m_page) source = m_page->Start(reuse);

    if (auto* style = ViewStyleSetting(view);
        style && source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
        source->SetViewStyle(static_cast<media::ViewStyle>(style->get()));
    if (!m_filter.empty()) source->SetFilter(m_filter);
    m_source = std::move(source);
    m_list->SetSource(m_source.get(), !sameView);
    if (m_page) m_page->OnPresentationChanged();

    if (push && !sameView) {
        if (!m_history.empty()) m_history.resize(m_historyPos + 1);
        m_history.push_back(view);
        m_historyPos = m_history.size() - 1;
    } else if (!m_history.empty()) {
        m_history[m_historyPos] = view;
    } else {
        m_history.push_back(view);
        m_historyPos = 0;
    }
    for (int id : {kBack, kForward, kViewOptions})
        if (auto* w = WidgetById(id)) w->Invalidate();
    RepaintBreadcrumb(); // also re-applies the layout (view options button, detail band)
    SelectPending();
}

// A page opened for one of its songs selects it and scrolls to it as soon as its row is there
// (an album page reads its songs after it opens).
void MainWindow::SelectPending() {
    if (!m_pendingSelect.is_valid() || !m_source || !m_list) return;
    const size_t row = m_source->FindRow(m_pendingSelect);
    if (row == SIZE_MAX) return;
    m_pendingSelect = nullptr;
    m_pendingScrollKey.clear();
    m_source->SelectOnly(row);
    m_source->SetFocus(row);
    m_list->EnsureVisible(row);
    m_list->Refresh();
}

void MainWindow::PaintDetailHeader() {
    if (!m_detailHeader || !m_detail.visible) return;
    const RECT rect = m_layout.detailHeader;
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return;
    m_detailHeaderSize = {w, h};

    winrt::com_ptr<IWICBitmapSource> art;
    if (!m_detail.artKey.empty() && m_detail.artItem.is_valid())
        ArtCache::Get().Find(m_detail.artKey, m_detail.artItem, PxI(kDetailArt), &art);

    auto& host = gfx::CompositionHost::Get();
    const float s = m_scale;
    host.Repaint(m_detailHeader, w, h, s, [&](ID2D1DeviceContext* dc) {
        const float W = w / s, H = h / s;
        const float px = LinePx() / s;
        dc->FillRectangle({-1, 0, W + 1, H}, VGrad(dc, 0, H, {{0.0f, 0xF6FBFF}, {1.0f, 0xEBF3FC}}).get());
        com_ptr<ID2D1SolidColorBrush> rule;
        dc->CreateSolidColorBrush(Hex(0xC3CCCF), rule.put());
        dc->FillRectangle({0, H - px, W, H}, rule.get());

        const float pad = 18, side = kDetailArt;
        const float ax = pad, ay = 14; // the artwork stays at the top-left of the band
        const bool made = !m_detail.artBadge.empty() || !m_detail.artCaption.empty();
        if (art) {
            winrt::com_ptr<ID2D1Bitmap> bitmap;
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(art.get(), nullptr, bitmap.put())))
                dc->DrawBitmap(bitmap.get(), D2D1::RectF(ax, ay, ax + side, ay + side), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else if (made) {
            dc->FillRectangle({ax, ay, ax + side, ay + side},
                              VGrad(dc, ay, ay + side, {{0.0f, 0x5B7DB8}, {1.0f, 0x2A3F66}}).get());
        } else if (!win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUM_0.PNG", {ax, ay, ax + side, ay + side})) {
            // WMP's own "no album art" picture, as the tiles show it; a plain plate without it.
            wp::RowPlate(dc, {ax, ay, ax + side, ay + side}, wp::RowState::Normal, false);
        }
        if (made) { // the made cover: a shade from the top and the foot, the mark and the caption
            const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0, 0.38f)}, {0.42f, D2D1::ColorF(0, 0.0f)},
                                                {0.58f, D2D1::ColorF(0, 0.0f)}, {1.0f, D2D1::ColorF(0, 0.55f)}};
            com_ptr<ID2D1GradientStopCollection> list;
            com_ptr<ID2D1LinearGradientBrush> shade;
            dc->CreateGradientStopCollection(stops, 4, list.put());
            if (list && SUCCEEDED(dc->CreateLinearGradientBrush({{0, ay}, {0, ay + side}}, list.get(), shade.put())))
                dc->FillRectangle({ax, ay, ax + side, ay + side}, shade.get());
            com_ptr<ID2D1SolidColorBrush> white, shadow;
            dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.put());
            dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.35f), shadow.put());
            if (!m_detail.artBadge.empty()) {
                auto badge = MakeText(m_detail.artBadge, side * 0.36f, DWRITE_FONT_WEIGHT_SEMI_BOLD, side, side * 0.5f);
                dc->DrawTextLayout({ax + 8, ay + 3}, badge.get(), shadow.get());
                dc->DrawTextLayout({ax + 7, ay + 2}, badge.get(), white.get());
            }
            if (!m_detail.artCaption.empty()) {
                auto caption = MakeText(m_detail.artCaption, 12.0f, DWRITE_FONT_WEIGHT_BOLD, side - 14, 18);
                dc->DrawTextLayout({ax + 7, ay + side - 22}, caption.get(), white.get());
            }
        }
        com_ptr<ID2D1SolidColorBrush> edge;
        dc->CreateSolidColorBrush(Hex(0x9AA7B8), edge.put());
        dc->DrawRectangle({ax + 0.5f, ay + 0.5f, ax + side - 0.5f, ay + side - 0.5f}, edge.get(), 1.0f);

        com_ptr<ID2D1SolidColorBrush> ink, sub, faint;
        dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
        dc->CreateSolidColorBrush(Hex(0x3E5878), sub.put());
        dc->CreateSolidColorBrush(Hex(0x6B7B8E), faint.put());
        const float textX = ax + side + 18;
        const float textW = DetailHeaderTextWidth();
        auto nameLayout = MakeText(m_detail.title, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, textW, 30);
        dc->DrawTextLayout({textX, 10}, nameLayout.get(), ink.get());
        if (!m_detail.subtitle.empty()) {
            auto statsLayout = MakeText(m_detail.subtitle, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 18);
            dc->DrawTextLayout({textX, 42}, statsLayout.get(), sub.get());
        }
        DrawDetailFacts(dc, textX, DetailTextTop(), textW);
        if (!m_detail.tagline.empty()) {
            const float top = DetailTextTop() + DetailFactsHeight(textW);
            auto tagline = MakeWrappedText(m_detail.tagline, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW,
                                           DetailTaglineHeight(textW));
            dc->DrawTextLayout({textX, top}, tagline.get(), sub.get());
        }
        const float briefTop = m_detailBriefTop;
        if (!m_detail.brief.empty()) {
            const float drawW = m_detailBriefTextW > 0 ? m_detailBriefTextW : textW;
            if (m_detailBriefScrollable) {
                const D2D1_RECT_F clip{textX, briefTop, textX + drawW, briefTop + m_detailBriefViewH};
                dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                auto full = MakeWrappedText(m_detail.brief, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, drawW, 4000.0f);
                dc->DrawTextLayout({textX, briefTop - m_detailBriefScroll}, full.get(), faint.get());
                dc->PopAxisAlignedClip();
            } else {
                // The collapsed text is already three lines at most: it gets their whole height.
                // A 48 DIP box dropped the third line (each is a hair over 16 DIP), and Show more
                // then stood alone after the line that was not drawn (owner, 2026-11-14).
                const bool collapsed = !m_detailBriefExpanded && !m_detailBriefCollapsed.empty();
                const float maxH = m_detailBriefExpanded ? std::max(24.0f, m_detailBriefViewH) : collapsed ? 4000.0f : 48.0f;
                const std::wstring& shown = collapsed ? m_detailBriefCollapsed : m_detail.brief;
                auto briefLayout = MakeWrappedText(shown, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, drawW, maxH);
                dc->DrawTextLayout({textX, briefTop}, briefLayout.get(), faint.get());
            }
            if (m_detailBriefScrollable && m_layout.detailBriefBar.right > m_layout.detailBriefBar.left) {
                const RECT& bar = m_layout.detailBriefBar;
                const float barX = (bar.left - rect.left) / s;
                const float barY = (bar.top - rect.top) / s;
                const float barH = (bar.bottom - bar.top) / s;
                float thumbTop = 0, thumbH = 0;
                if (DetailBriefThumb(&thumbTop, &thumbH)) {
                    // Keep the surface's DPI transform; replacing it would draw the bar at
                    // device scale and make it jump around at 125 %/150 %.
                    D2D1_MATRIX_3X2_F base{};
                    dc->GetTransform(&base);
                    const D2D1::Matrix3x2F baseM(base._11, base._12, base._21, base._22, base._31, base._32);
                    dc->SetTransform(D2D1::Matrix3x2F::Translation(barX, barY) * baseM);
                    wp::ScrollBar(dc, {kDetailScrollWidth, barH}, thumbTop, thumbH,
                                  m_detailBriefBlend.data());
                    dc->SetTransform(baseM);
                }
            }
        }
    });
    Place(m_detailHeader, rect); // Repaint resets the visual to its surface origin
    LogDetail(art != nullptr);
}

// The band's text, written to the log when it changes, so what a page shows from a service can
// be read back without looking at the screen (see ui/dev_commands.cpp).
void MainWindow::LogDetail(bool art) {
    std::wstring text = L"page: " + m_detail.title + (art ? L" [art]" : L" [no art]");
    if (!m_detail.subtitle.empty()) text += L" | " + m_detail.subtitle;
    for (const auto& fact : m_detail.facts) text += L" | " + fact.label + L": " + fact.value;
    if (!m_detail.tagline.empty()) text += L" | tagline: " + m_detail.tagline;
    if (m_source) {
        text += L" | rows: " + std::to_wstring(m_source->Count());
        if (const auto* groups = m_source->Groups())
            for (const auto& group : *groups)
                text += L" | " + group.header + L" (" + std::to_wstring(group.count) + L")";
    }
    if (!m_detail.brief.empty())
        text += L" | brief: " + std::to_wstring(m_detail.brief.size()) + L" chars, \"" +
                m_detail.brief.substr(0, 24) + L"...\"";
    if (text == m_loggedDetail) return;
    m_loggedDetail = text;
    core::Log(text);
}

void MainWindow::RepaintDetailHeader() {
    if (!m_detailHeader) return;
    m_detail = m_page ? m_page->Detail() : DetailInfo{};
    if (!m_detail.visible) return;
    PaintDetailHeader();
    if (Widget* action = WidgetById(kDetailAction)) action->Invalidate();
    if (Widget* brief = WidgetById(kDetailBriefToggle)) brief->Invalidate();
}

// Without a stats line (an artist the service gives no counts for) the facts close up under
// the name instead of leaving its row empty (owner, 2026-11-14).
float MainWindow::DetailTextTop() const {
    return m_detail.subtitle.empty() ? kDetailTextTop - kDetailStatsLine : kDetailTextTop;
}

float MainWindow::DetailHeaderTextWidth() const {
    const Layout& L = m_layout;
    const float s = m_scale;
    const float textX = 18 + kDetailArt + 18;
    const float right = m_detail.action != DetailInfo::Action::None
                            ? (L.detailAction.left - L.detailHeader.left) / s - 18.0f
                            : (L.detailHeader.right - L.detailHeader.left) / s - 18.0f;
    return std::max(120.0f, right - textX);
}

size_t MainWindow::DetailFactColumns(float textW) const {
    const size_t n = m_detail.facts.size();
    if (n == 0) return 0;
    const auto fit = static_cast<size_t>(std::clamp(static_cast<int>(textW / kDetailFactColumnMinW), 1, 3));
    return std::min(n, fit);
}

float MainWindow::DetailFactsHeight(float textW) const {
    const size_t columns = DetailFactColumns(textW);
    if (columns == 0) return 0;
    const size_t rows = (m_detail.facts.size() + columns - 1) / columns;
    return rows * kDetailFactRowH + 4;
}

float MainWindow::DetailTaglineHeight(float textW) const {
    if (m_detail.tagline.empty()) return 0;
    auto layout = MakeWrappedText(m_detail.tagline, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 2 * kDetailFactRowH);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::min(std::ceil(metrics.height), 2 * kDetailFactRowH) + 6;
}

// The facts in columns: each column's labels start at one line and its values at another
// (owner, 2026-11-14: right-aligned labels made the rows look crooked); long values end in an
// ellipsis. A column takes the width its text needs; when they do not all fit, the narrow
// ones keep theirs and the wide ones share the rest equally.
void MainWindow::DrawDetailFacts(ID2D1DeviceContext* dc, float x, float y, float textW) {
    m_detailCut.clear();
    const size_t columns = DetailFactColumns(textW);
    if (columns == 0) return;
    const size_t n = m_detail.facts.size();
    const size_t rows = (n + columns - 1) / columns;
    const auto width = [](const std::wstring& text) {
        DWRITE_TEXT_METRICS metrics{};
        MakeText(text, 12.0f)->GetMetrics(&metrics);
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    };
    std::vector<float> labelW(columns, 0), need(columns, 0);
    for (size_t c = 0; c < columns; ++c) {
        float valueW = 0;
        for (size_t i = c * rows; i < std::min(n, (c + 1) * rows); ++i) {
            labelW[c] = std::max(labelW[c], width(m_detail.facts[i].label + L":"));
            valueW = std::max(valueW, width(m_detail.facts[i].value));
        }
        need[c] = labelW[c] + kDetailFactGap + valueW;
    }
    // Water-filling: share what is left among the columns that need more than their share.
    std::vector<float> columnW = need;
    float room = textW - kDetailFactColumnGap * (columns - 1);
    std::vector<bool> settled(columns, false);
    for (size_t open = columns; open > 0;) {
        const float share = room / open;
        bool changed = false;
        for (size_t c = 0; c < columns; ++c)
            if (!settled[c] && need[c] <= share) {
                settled[c] = true;
                room -= need[c];
                --open;
                changed = true;
            }
        if (!changed) {
            for (size_t c = 0; c < columns; ++c)
                if (!settled[c]) columnW[c] = share;
            break;
        }
    }
    // A label is never cut (owner, 2026-11-14: "唱片公..." said nothing); its value keeps at
    // least a few characters.
    for (size_t c = 0; c < columns; ++c)
        columnW[c] = std::max(columnW[c], std::min(need[c], labelW[c] + kDetailFactGap + 60));
    com_ptr<ID2D1SolidColorBrush> label, value;
    dc->CreateSolidColorBrush(Hex(0x6B7B8E), label.put());
    dc->CreateSolidColorBrush(Hex(0x1E395B), value.put());
    float cx = x;
    for (size_t c = 0; c < columns; ++c) {
        const float nameW = labelW[c];
        const float valueW = std::max(1.0f, columnW[c] - nameW - kDetailFactGap);
        for (size_t i = c * rows; i < std::min(n, (c + 1) * rows); ++i) {
            const float top = y + (i - c * rows) * kDetailFactRowH;
            auto name = MakeText(m_detail.facts[i].label + L":", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, nameW, kDetailFactRowH);
            dc->DrawTextLayout({cx, top}, name.get(), label.get());
            const float vx = cx + nameW + kDetailFactGap;
            auto text = MakeText(m_detail.facts[i].value, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, valueW, kDetailFactRowH);
            dc->DrawTextLayout({vx, top}, text.get(), value.get());
            if (width(m_detail.facts[i].value) > valueW + 0.5f) {
                // Cut short: the tooltip shows it whole (UpdateDetailTip).
                const RECT& band = m_layout.detailHeader;
                m_detailCut.push_back({{band.left + PxI(vx), band.top + PxI(top), band.left + PxI(vx + valueW),
                                        band.top + PxI(top + kDetailFactRowH)},
                                       m_detail.facts[i].value});
            }
        }
        cx += columnW[c] + kDetailFactColumnGap;
    }
}

void MainWindow::OnDetailAction() {
    if (m_page) m_page->DetailAction();
}

bool MainWindow::DetailBriefThumb(float* top, float* height) const {
    if (!m_detailBriefScrollable) return false;
    const float arrow = 17.0f;
    const float barH = m_detailBriefViewH;
    const float trackH = barH - arrow * 2;
    if (trackH <= 0 || m_detailBriefContentH <= m_detailBriefViewH + 0.5f) return false;
    *height = std::max(24.0f, std::min(trackH, trackH * m_detailBriefViewH / m_detailBriefContentH));
    const float maxScroll = std::max(0.001f, m_detailBriefContentH - m_detailBriefViewH);
    *top = arrow + (trackH - *height) * (m_detailBriefScroll / maxScroll);
    return true;
}

int MainWindow::DetailBriefPartAt(POINT pt) const {
    if (!m_detailBriefScrollable || !PtInRect(&m_layout.detailBriefBar, pt)) return 0;
    const RECT& bar = m_layout.detailBriefBar;
    const float y = (pt.y - bar.top) / m_scale;
    const float h = (bar.bottom - bar.top) / m_scale;
    if (y < 17.0f) return 1;
    if (y >= h - 17.0f) return 2;
    float top = 0, height = 0;
    if (DetailBriefThumb(&top, &height) && y >= top && y < top + height) return 4;
    return 3;
}

void MainWindow::ScrollDetailBrief(float delta) {
    const float maxScroll = std::max(0.0f, m_detailBriefContentH - m_detailBriefViewH);
    const float next = std::clamp(m_detailBriefScroll + delta, 0.0f, maxScroll);
    if (next == m_detailBriefScroll) return;
    m_detailBriefScroll = next;
    RepaintDetailHeader();
}

void MainWindow::UpdateDetailBriefHover(POINT pt) {
    const int part = DetailBriefPartAt(pt);
    if (part != m_detailBriefHot) {
        m_detailBriefHot = part;
        StartScrollAnimation();
    }
}

void MainWindow::OnDetailBriefBarDown(POINT pt) {
    const int part = DetailBriefPartAt(pt);
    m_detailBriefPressed = part;
    m_detailBriefPressPt = pt;
    if (part == 1) {
        ScrollDetailBrief(-54.0f);
    } else if (part == 2) {
        ScrollDetailBrief(54.0f);
    } else if (part == 3) {
        float top = 0, height = 0;
        if (DetailBriefThumb(&top, &height)) {
            const float y = (pt.y - m_layout.detailBriefBar.top) / m_scale;
            const float page = std::max(18.0f, m_detailBriefViewH - 36.0f);
            m_detailBriefTrackDir = y < top ? 1 : 2;
            ScrollDetailBrief(y < top ? -page : page);
        }
    } else if (part == 4) {
        float top = 0, height = 0;
        if (DetailBriefThumb(&top, &height))
            m_detailBriefDragOffset = pt.y - m_layout.detailBriefBar.top - PxI(top);
    }
    StartScrollAnimation();
    if (part >= 1 && part <= 3) StartScrollRepeat();
}

void MainWindow::OnDetailBriefBarMove(POINT pt) {
    if (m_detailBriefPressed != 4) return;
    float top = 0, height = 0;
    if (!DetailBriefThumb(&top, &height)) return;
    const float arrow = 17.0f;
    const float trackH = m_detailBriefViewH - arrow * 2;
    if (trackH <= height) return;
    const float y =
        (pt.y - m_layout.detailBriefBar.top) / m_scale - arrow - m_detailBriefDragOffset / m_scale;
    const float maxScroll = std::max(0.0f, m_detailBriefContentH - m_detailBriefViewH);
    const float frac = std::clamp(y / (trackH - height), 0.0f, 1.0f);
    const float next = frac * maxScroll;
    if (next != m_detailBriefScroll) {
        m_detailBriefScroll = next;
        RepaintDetailHeader();
    }
}

void MainWindow::OnDetailBriefBarUp() {
    if (!m_detailBriefPressed) return;
    m_detailBriefPressed = 0;
    m_detailBriefTrackDir = 0;
    StartScrollAnimation();
}

bool MainWindow::AnimateDetailScrollBar() {
    bool active = false, changed = false;
    for (int part = 1; part <= 4; ++part) {
        const float target = m_detailBriefPressed == part ? 2.0f : m_detailBriefHot == part ? 1.0f : 0.0f;
        float& blend = m_detailBriefBlend[part];
        if (blend < target) {
            blend = std::min(target, blend + 0.12f);
            changed = true;
        } else if (blend > target) {
            blend = std::max(target, blend - 0.12f);
            changed = true;
        }
        if (blend != target) active = true;
    }
    if (changed) RepaintDetailHeader();
    return active;
}

bool MainWindow::OnDetailBriefRepeat() {
    const float maxScroll = std::max(0.0f, m_detailBriefContentH - m_detailBriefViewH);
    const float page = std::max(18.0f, m_detailBriefViewH - 36.0f);
    switch (m_detailBriefPressed) {
    case 1:
        if (m_detailBriefScroll <= 0.0f) return false;
        ScrollDetailBrief(-54.0f);
        return m_detailBriefScroll > 0.0f;
    case 2:
        if (m_detailBriefScroll >= maxScroll) return false;
        ScrollDetailBrief(54.0f);
        return m_detailBriefScroll < maxScroll;
    case 3: {
        float top = 0, height = 0;
        if (!DetailBriefThumb(&top, &height)) return false;
        const float y = (m_detailBriefPressPt.y - m_layout.detailBriefBar.top) / m_scale;
        if (m_detailBriefTrackDir == 1) {
            if (y >= top) return false;
            if (m_detailBriefScroll <= 0.0f) return false;
            ScrollDetailBrief(-page);
            return true;
        }
        if (m_detailBriefTrackDir == 2) {
            if (y < top + height) return false;
            if (m_detailBriefScroll >= maxScroll) return false;
            ScrollDetailBrief(page);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

void MainWindow::StartScrollAnimation() {
    if (m_scrollAnimActive) return;
    m_scrollAnimActive = true;
    SetTimer(m_hwnd, kScrollAnimTimer, 16, nullptr);
}

void MainWindow::StartScrollRepeat() {
    if (m_scrollRepeatActive) return;
    m_scrollRepeatActive = true;
    m_scrollRepeatSince = GetTickCount64();
    SetTimer(m_hwnd, kScrollRepeatTimer, 50, nullptr);
}

std::vector<MenuAction> MainWindow::BuildArtistLinks(metadb_handle_list_cref items) {
    std::vector<MenuAction> links;
    if (items.get_count() == 0 || !items[0].is_valid()) return links;
    const metadb_handle_ptr& first = items[0];

    if (auto* provider = stream::ProviderOf(first)) {
        const auto track = provider->FindTrack(provider->TrackId(first));
        if (!track) return links;
        const stream::Id sectionId = m_view.provider == provider->Key() ? m_view.sectionId : stream::Id{};
        std::vector<stream::Id> seen;
        for (const auto& artist : track->artists) {
            if (artist.id.empty() || artist.name.empty()) continue;
            if (std::find(seen.begin(), seen.end(), artist.id) != seen.end()) continue;
            seen.push_back(artist.id);
            links.push_back({artist.name, [this, provider, artist, sectionId] {
                                 Open(ArtistView(*provider, artist.id, artist.name, sectionId));
                             }});
        }
        return links;
    }

    const media::Track* track = m_library.FindTrack(first);
    if (!track) return links;
    auto add = [this, &links](media::GroupKind kind, const std::wstring& key, const std::wstring& label) {
        for (const auto& link : links)
            if (link.label == label) return;
        links.push_back({label, [this, kind, key, label] {
                             View view;
                             view.library = View::Library::Local;
                             view.kind = View::Kind::Group;
                             view.group = kind;
                             view.key = key;
                             view.name = label;
                             Open(view);
                         }});
    };
    size_t pos = 0;
    while (pos <= track->artist.size()) {
        const size_t end = track->artist.find(L"; ", pos);
        std::wstring name =
            track->artist.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        const size_t begin = name.find_first_not_of(L" \t");
        const size_t last = name.find_last_not_of(L" \t");
        name = begin == std::wstring::npos ? std::wstring() : name.substr(begin, last - begin + 1);
        if (!name.empty() && m_library.FindGroup(media::GroupKind::ContributingArtist, name))
            add(media::GroupKind::ContributingArtist, name, name);
        if (end == std::wstring::npos) break;
        pos = end + 2;
    }
    if (links.empty() && !track->albumArtist.empty() &&
        m_library.FindGroup(media::GroupKind::Artist, track->albumArtist))
        add(media::GroupKind::Artist, track->albumArtist, track->albumArtist);
    return links;
}

// Our commands above foobar2000's in a song's context menu: where the song leads (artist,
// album), what it can be added to, then what this page can do with it.
std::vector<ContextAction> MainWindow::BuildContextActions(metadb_handle_list_cref items) {
    std::vector<ContextAction> actions;
    if (items.get_count() == 0) return actions;
    metadb_handle_list selected;
    selected.add_items(items);

    // The same groups as an album's or a playlist's menu (ui/item_menu.h): where it leads (opened
    // from this page: the path goes on from here), what to do with it, searches, the page's own.
    const metadb_handle_ptr& first = items[0];
    stream::Provider* provider = stream::ProviderOf(first);
    std::wstring title, albumName;
    std::vector<std::wstring> artistNames; // each credited artist, for "Search for artist"
    if (provider) {
        if (const auto track = provider->FindTrack(provider->TrackId(first))) {
            title = track->title;
            albumName = track->album;
            for (const auto& artist : track->artists)
                if (!artist.name.empty()) artistNames.push_back(artist.name);
        }
    } else if (const media::Track* track = first.is_valid() ? m_library.FindTrack(first) : nullptr) {
        title = track->title;
        albumName = track->album;
        if (!track->artist.empty()) artistNames.push_back(track->artist);
    }
    if (provider) {
        const auto track = provider->FindTrack(provider->TrackId(first));
        const bool onAlbumPage = track && m_view.kind == View::Kind::RemoteAlbum &&
                                 m_view.provider == provider->Key() && m_view.remoteId == track->albumId;
        const stream::Id sectionId = m_view.provider == provider->Key() ? m_view.sectionId : stream::Id{};
        if (track && !track->albumId.empty() && !onAlbumPage)
            actions.push_back(
                {L"Go to album", [this, provider, id = track->albumId, name = track->album, sectionId, first] {
                     View v = AlbumView(*provider, id, name, sectionId);
                     v.select = first;
                     Open(v);
                 }});
    } else if (const media::Track* track = first.is_valid() ? m_library.FindTrack(first) : nullptr;
               track && track->albumIndex < m_library.Groups(media::GroupKind::Album).size()) {
        const auto& album = m_library.Groups(media::GroupKind::Album)[track->albumIndex];
        const bool onAlbumPage = m_view.kind == View::Kind::Group &&
                                 m_view.group == media::GroupKind::Album && m_view.key == album.key;
        if (!onAlbumPage)
            actions.push_back({L"Go to album", [this, key = album.key, name = album.name, first] {
                                   View v;
                                   v.kind = View::Kind::Group;
                                   v.group = media::GroupKind::Album;
                                   v.key = key;
                                   v.name = name;
                                   v.select = first;
                                   Open(v);
                               }});
    }
    auto artistLinks = BuildArtistLinks(items);
    if (artistLinks.size() == 1)
        actions.push_back({L"Go to artist", artistLinks.front().run});
    else if (!artistLinks.empty())
        actions.push_back({L"Go to artist", nullptr, std::move(artistLinks)});
    // One song of a source that has song details: its page.
    if (provider && selected.get_count() == 1 && provider->Can(stream::Capability::SongDetails) &&
        !(m_view.kind == View::Kind::RemoteSong && m_view.remoteId == provider->TrackId(selected[0]))) {
        const stream::Id id = provider->TrackId(selected[0]);
        if (!id.empty())
            actions.push_back({L"Song info", [this, provider, id, title] { Open(SongView(*provider, id, title)); }});
    }
    if (!actions.empty()) actions.push_back({});

    const bool favorite = media::AllFavorite(selected);
    bool favoriteWritable = true;
    for (size_t i = 0; i < selected.get_count() && favoriteWritable; ++i)
        if (auto* owner = stream::ProviderOf(selected[i]))
            favoriteWritable = owner->CanSetFavorite(!favorite);
    if (favoriteWritable) {
        actions.push_back({favorite ? L"Remove from Favorites" : L"Add to Favorites",
                           [selected, favorite] { media::SetFavorite(selected, !favorite); }});
    }
    // A source whose library takes single songs (Apple Music's Add to Library): the songs
    // themselves, never their albums. Remove only when every one is known to be there.
    if (provider && provider->AccountVerified() && provider->Can(stream::Capability::SaveSongs)) {
        std::vector<stream::Id> ids;
        for (size_t i = 0; i < selected.get_count(); ++i)
            if (stream::ProviderOf(selected[i]) == provider)
                if (stream::Id id = provider->TrackId(selected[i]); !id.empty()) ids.push_back(std::move(id));
        if (!ids.empty()) {
            const bool saved = provider->SongsSavedState(ids).value_or(false);
            actions.push_back({provider->Text(saved ? L"Remove from Library" : L"Add to Library"),
                               [provider, ids, saved] {
                                   provider->SetSongsSaved(ids, !saved, [provider](std::wstring error) {
                                       if (!error.empty()) ShowProviderError(*provider, error);
                                   });
                               }});
        }
    }
    // A foobar2000 playlist takes local and provider tracks alike; a provider's own
    // playlists only its tracks.
    std::vector<MenuAction> local;
    for (auto& target :
         LocalPlaylistTargets(m_view.kind == View::Kind::Playlist ? m_view.playlist : SIZE_MAX)) {
        const std::wstring label = target.name.empty() ? L"Unnamed playlist" : target.name;
        local.push_back(
            {label, [selected, target = std::move(target)] { AddToLocalPlaylist(target, selected); }});
    }
    if (local.empty()) local.push_back({L"No playlists", nullptr});
    actions.push_back({L"Add to playlist", nullptr, std::move(local)});
    if (provider) {
        const bool onPlaylist =
            m_view.kind == View::Kind::RemotePlaylist && m_view.provider == provider->Key();
        AddRemotePlaylistMenu(actions, *provider, Ready(selected),
                              onPlaylist ? m_view.remoteId : stream::Id());
    }
    AddDownloadActions(actions, selected, m_hwnd);

    // Searches name what they look for (owner, 2026-11-21: a song offered only 搜索此唱片集).
    actions.push_back({});
    const auto quoted = [](const std::wstring& name) { return L" \u201C" + name + L"\u201D"; };
    if (!title.empty() && selected.get_count() == 1)
        actions.push_back({Tr(L"Search for song") + quoted(title), [this, title] { Search(title); }});
    for (const auto& artist : artistNames)
        actions.push_back({Tr(L"Search for artist") + quoted(artist), [this, artist] { Search(artist); }});
    if (!albumName.empty())
        actions.push_back({Tr(L"Search for album") + quoted(albumName), [this, albumName] { Search(albumName); }});

    // Removals that belong to the page go last.
    std::vector<MenuAction> page;
    if (m_page) m_page->AddSongActions(page, selected);
    if (m_view.kind == View::Kind::Playlist && media::IsQueue(m_view.playlist))
        page.push_back({L"Remove from Now Playing", [this] { RemoveFromNowPlaying(); }});
    else if (m_view.kind == View::Kind::AllRecentPlayed)
        page.push_back(
            {L"Remove from Recently Played", [this, selected] { ForgetPlayStats(selected, true); }});
    else if (m_view.kind == View::Kind::AllMostPlayed)
        page.push_back({L"Remove from Most Played", [this, selected] { ForgetPlayStats(selected, false); }});
    if (!page.empty() && !page.front().label.empty()) actions.push_back({});
    for (auto& action : page)
        actions.push_back(std::move(action));
    return actions;
}

void MainWindow::RemoveFromNowPlaying() {
    // The selected rows, not every row with the same song: the queue can hold a song twice.
    const t_size queue = media::QueuePlaylist(false);
    if (queue == SIZE_MAX) return;
    auto pm = playlist_manager::get();
    const t_size count = pm->playlist_get_item_count(queue);
    if (!count) return;
    pfc::bit_array_bittable mask(count);
    pm->playlist_get_selection_mask(queue, mask);
    bool any = false;
    for (t_size i = 0; i < count && !any; ++i)
        any = mask[i];
    if (!any) { // a right-click on an unselected row acts on that row
        const t_size focus = pm->playlist_get_focus_item(queue);
        if (focus >= count) return;
        mask.set(focus, true);
    }
    t_size playingPlaylist = SIZE_MAX, playingIndex = SIZE_MAX;
    const bool playing = pm->get_playing_item_location(&playingPlaylist, &playingIndex);
    const bool removesPlaying =
        playing && playingPlaylist == queue && playingIndex < count && mask[playingIndex];
    const bool paused = playback_control::get()->is_paused();
    pm->playlist_undo_backup(queue);
    pm->playlist_remove_items(queue, mask);
    if (!removesPlaying) return;
    // Find the first surviving song after the old playing position and map its index
    // through the removals; if the playing song was last, continue with the new last.
    const t_size remaining = pm->playlist_get_item_count(queue);
    if (!remaining) {
        playback_control::get()->stop();
        return;
    }
    t_size removedBefore = 0;
    for (t_size i = 0; i <= playingIndex && i < count; ++i)
        if (mask[i]) ++removedBefore;
    t_size next = SIZE_MAX;
    for (t_size i = playingIndex + 1; i < count; ++i) {
        if (!mask[i]) {
            next = i - removedBefore;
            break;
        }
        ++removedBefore;
    }
    if (next == SIZE_MAX) next = remaining - 1;
    pm->playlist_set_focus_item(queue, next);
    pm->playlist_execute_default_action(queue, next);
    if (paused) playback_control::get()->pause(true); // the next song waits like the removed one did
}

void MainWindow::ClearNowPlaying() {
    const t_size queue = media::QueuePlaylist(false);
    if (queue == SIZE_MAX) return;
    auto pm = playlist_manager::get();
    t_size playingPlaylist = SIZE_MAX, playingIndex = SIZE_MAX;
    const bool playing =
        pm->get_playing_item_location(&playingPlaylist, &playingIndex) && playingPlaylist == queue;
    pm->playlist_undo_backup(queue);
    pm->playlist_clear(queue);
    if (playing) playback_control::get()->stop();
}

void MainWindow::ForgetPlayStats(metadb_handle_list_cref items, bool recent) {
    if (recent)
        media::PlayStats::Get().ForgetRecent(items);
    else
        media::PlayStats::Get().ClearCounts(items);
    const size_t top = m_list ? m_list->TopRow() : SIZE_MAX;
    Navigate(m_view, false);
    if (m_list && top != SIZE_MAX && m_source && m_source->Count())
        m_list->ScrollToRow(std::min(top, m_source->Count() - 1));
}

void MainWindow::ClearPlayStats(bool recent) {
    if (recent)
        media::PlayStats::Get().ForgetAllRecent();
    else
        media::PlayStats::Get().ClearAllCounts();
    if (m_view.kind == View::Kind::AllRecentPlayed || m_view.kind == View::Kind::AllMostPlayed)
        Navigate(m_view, false);
}

void MainWindow::RefreshPlayCounts() {
    if (!m_list) return;
    if (auto* source = dynamic_cast<ui::SongTableSource*>(m_source.get())) source->RefreshPlayCounts();
    m_list->Refresh(); // the local sources read the count from the store while painting
}

void MainWindow::ShowPageContextMenu(POINT screen) {
    std::vector<MenuAction> actions;
    // Local song pages act on their whole (filtered) contents; provider pages and the
    // combined favorites add the same commands themselves.
    const bool localPlaylist = m_view.kind == View::Kind::Playlist && !media::IsQueue(m_view.playlist);
    const bool songPage = localPlaylist || m_view.kind == View::Kind::AllRecentPlayed ||
                          m_view.kind == View::Kind::AllMostPlayed || m_view.kind == View::Kind::AllMusic ||
                          m_view.kind == View::Kind::Category || m_view.kind == View::Kind::Group;
    if (songPage && m_source && m_source->Count()) {
        metadb_handle_list items;
        for (size_t row = 0; row < m_source->Count(); ++row)
            if (const auto track = m_source->Item(row); track.is_valid()) items.add_item(track);
        if (items.get_count()) {
            AddBulkActions(actions, Ready(items), nullptr, {}, localPlaylist ? m_view.playlist : SIZE_MAX);
            if (download::AnyDownloadable(items)) AddBulkDownloadActions(actions, Ready(items), m_hwnd);
            actions.push_back({});
        }
    }
    if (m_source && m_source->Count()) // every list page can take the whole selection
        actions.push_back({L"Select all", [this] {
                               m_source->SelectAll();
                               m_list->Refresh();
                           }});
    std::vector<MenuAction> page;
    if (m_page) m_page->AddPageActions(page);
    if (m_view.kind == View::Kind::Playlist && media::IsQueue(m_view.playlist))
        page.push_back({L"Clear Now Playing", [this] { ClearNowPlaying(); }});
    else if (m_view.kind == View::Kind::AllRecentPlayed)
        page.push_back({L"Clear Recently Played", [this] { ClearPlayStats(true); }});
    else if (m_view.kind == View::Kind::AllMostPlayed)
        page.push_back({L"Clear Most Played", [this] { ClearPlayStats(false); }});
    actions.push_back({});
    for (auto& action : page)
        actions.push_back(std::move(action));
    if (CanRefreshPage()) actions.insert(actions.end(), {{}, {L"Refresh", [this] { RefreshCurrentPage(); }}});

    // No leading, trailing or doubled separators.
    std::vector<MenuAction> tidy;
    for (auto& action : actions) {
        const bool separator = action.label.empty() && !action.run && action.children.empty();
        if (separator && (tidy.empty() || tidy.back().label.empty())) continue;
        tidy.push_back(std::move(action));
    }
    while (!tidy.empty() && tidy.back().label.empty())
        tidy.pop_back();
    if (!tidy.empty()) ShowActionMenu(m_hwnd, screen, tidy);
}

void MainWindow::OnProviderAccount(stream::Provider& provider) {
    const stream::Id providerKey{provider.Key()};
    InvalidateRemotePageCache(providerKey);
    for (stream::ArtKind kind :
         {stream::ArtKind::Track, stream::ArtKind::Album, stream::ArtKind::Artist, stream::ArtKind::Playlist})
        ArtCache::Get().Invalidate(provider.ArtKeyPrefix(kind));
    RefreshProviderSections();
    UpdateFavoriteState();
    const bool currentProvider = m_view.IsRemote() && m_view.provider == providerKey;
    const bool currentAllFavorites = m_view.kind == View::Kind::AllFavorites;
    View replacement = m_view;
    if (currentProvider) {
        if (provider.SignedIn()) {
            replacement = ProviderLandingView(provider);
        } else {
            replacement = {};
            replacement.library = View::Library::All;
            replacement.kind = View::Kind::AllMusic;
        }
    }
    // Drop another account's private pages and names from Back/Forward, replacing the
    // current entry when it belongs to the provider that changed.
    std::vector<View> history;
    size_t historyPos = SIZE_MAX;
    auto sameProvider = [&providerKey](const View& view) {
        return view.IsRemote() && view.provider == providerKey;
    };
    for (size_t i = 0; i < m_history.size(); ++i) {
        if (i == m_historyPos) {
            historyPos = history.size();
            history.push_back(sameProvider(m_history[i]) ? replacement : m_history[i]);
        } else if (!sameProvider(m_history[i])) {
            history.push_back(m_history[i]);
        }
    }
    if (history.empty()) {
        history.push_back(replacement);
        historyPos = 0;
    } else if (historyPos == SIZE_MAX) {
        historyPos = std::min(m_historyPos, history.size() - 1);
    }
    m_history = std::move(history);
    m_historyPos = historyPos;
    m_pendingScrollKey.clear();
    DiscardProviderScrollState(providerKey);

    // Signing in or out changes what the provider's pages and the combined favorites show.
    if (currentProvider || currentAllFavorites) {
        m_forcePageRefresh = true;
        Navigate(currentProvider ? replacement : m_view, false);
        DiscardProviderScrollState(providerKey);
        m_pendingScrollKey.clear();
    }
    if (m_list) m_list->Refresh();
    RepaintBreadcrumb();
}

void MainWindow::OnProviderAccountObserved(stream::Provider& provider) {
    RefreshProviderSections();
    UpdateFavoriteState();
    const bool currentProvider = m_view.IsRemote() && m_view.provider == provider.Key();
    // The combined favorites list the providers that are signed in now.
    if ((currentProvider || m_view.kind == View::Kind::AllFavorites) && m_page) m_page->OnLibraryChanged();
    if ((currentProvider || m_view.kind == View::Kind::AllFavorites) && m_list) m_list->Refresh();
    RepaintBreadcrumb();
    RepaintDetailHeader();
}

void MainWindow::OnProviderLibrary(stream::Provider& provider) {
    // Background count updates repeat this event; retry only art that never loaded.
    for (stream::ArtKind kind :
         {stream::ArtKind::Track, stream::ArtKind::Album, stream::ArtKind::Artist, stream::ArtKind::Playlist})
        ArtCache::Get().InvalidateMissing(provider.ArtKeyPrefix(kind));
    RefreshProviderSections();
    metadb_handle_ptr now;
    if (playback_control::get()->get_now_playing(now)) {
        if (const auto id = provider.TrackId(now); !id.empty()) {
            if (const auto track = provider.FindTrack(id);
                track &&
                (track->title != m_trackTitle || stream::JoinArtists(track->artists) != m_trackArtist))
                OnTrackChanged();
        }
    }
    if (m_page && m_view.provider == provider.Key()) m_page->OnLibraryChanged();
    if (m_list) m_list->Refresh();
    RepaintBreadcrumb();
    RepaintDetailHeader();
}

void MainWindow::OnProviderFavorites(stream::Provider&) {
    UpdateFavoriteState(); // the taskbar and now-playing star follow the playing song
    if (!m_source || !m_list) return;
    if (m_page) {
        m_page->OnFavoritesChanged();
    } else if (auto* source = dynamic_cast<SongTableSource*>(m_source.get())) {
        source->RefreshFavorites();
        m_list->Refresh();
    } else {
        m_list->Refresh();
    }
}

void MainWindow::OnProviderLanguage(stream::Provider& provider) {
    const stream::Id providerKey{provider.Key()};
    // Names change with the language; pictures do not, so the art stays as it is.
    InvalidateRemotePageCache(providerKey);
    RefreshProviderSections();
    auto refresh = [&provider, &providerKey](View& view) {
        if (view.IsRemote() && view.provider == providerKey)
            view = NormalizeRemoteView(provider, std::move(view), true);
    };
    refresh(m_view);
    for (auto& view : m_history)
        refresh(view);
    DiscardProviderScrollState(providerKey);
    m_pendingScrollKey.clear();
    if ((m_view.IsRemote() && m_view.provider == providerKey) || m_view.kind == View::Kind::AllFavorites) {
        m_forcePageRefresh = true;
        Navigate(m_view, false);
        DiscardProviderScrollState(providerKey);
        m_pendingScrollKey.clear();
    } else if (m_list) {
        m_list->Refresh();
    }
    RepaintBreadcrumb();
}

void MainWindow::OnProviderDisplay(stream::Provider& provider) {
    const stream::Id providerKey{provider.Key()};
    // Drop cached page objects from the old projection, but keep account, library and artwork
    // caches. The current page reopens from canonical IDs and publishes its cached prefix first.
    InvalidateRemotePageCache(providerKey);
    if ((m_view.IsRemote() && m_view.provider == providerKey) || m_view.kind == View::Kind::AllFavorites) {
        m_forcePageRefresh = true;
        Navigate(m_view, false);
    } else if (m_list) {
        m_list->Refresh();
    }
    OnTrackChanged();
    RepaintBreadcrumb();
}

void MainWindow::DiscardProviderScrollState(const stream::Id& provider) {
    if (const auto pending = m_scrollPositions.find(m_pendingScrollKey);
        pending != m_scrollPositions.end() && pending->second.provider == provider)
        m_pendingScrollKey.clear();
    std::erase_if(m_scrollPositions,
                  [&provider](const auto& entry) { return entry.second.provider == provider; });
}

stream::Provider* MainWindow::ViewProvider() const {
    return m_view.IsRemote() ? stream::FindProvider(m_view.provider) : nullptr;
}

void MainWindow::RefreshProviderSections() {
    if (!m_nav) return;
    std::vector<NavPane::ProviderSection> sections;
    for (auto* provider : stream::Providers()) {
        NavPane::ProviderSection section;
        section.provider = provider;
        section.signedIn = provider->SignedIn() || provider->SessionRestoring();
        section.sections = provider->Sections();
        if (section.signedIn) {
            for (const auto& navigation : section.sections)
                if (navigation.kind == stream::SectionKind::PlaylistShelf &&
                    section.playlists.find(navigation.id) == section.playlists.end())
                    section.playlists[navigation.id] = provider->SectionPlaylists(navigation.id);
        }
        sections.push_back(std::move(section));
    }
    m_nav->SetProviders(std::move(sections));
}

void MainWindow::OnDiscsChanged() {
    if (!m_nav) return;
    std::vector<NavPane::DiscRow> rows;
    for (const auto& drive : cd::Drives())
        rows.push_back({drive.drive, cd::DriveTitle(drive)});
    m_nav->SetDiscs(std::move(rows));
    if (m_view.kind != View::Kind::AudioCd) return;
    if (cd::FindDrive(static_cast<wchar_t>(m_view.playlist))) {
        Navigate(m_view, false); // read, tagged or taken out: the disc's name and rows
        return;
    }
    View queue; // the drive itself went (an external drive unplugged)
    queue.library = View::Library::All;
    queue.kind = View::Kind::Playlist;
    queue.playlist = media::QueuePlaylist(true);
    Navigate(queue, true);
}

void MainWindow::ShowDiscMenu(wchar_t drive, POINT screen) {
    const auto* disc = cd::FindDrive(drive);
    if (!disc) return;
    std::vector<MenuAction> actions;
    if (disc->state == cd::CdDrive::State::Loaded) {
        const metadb_handle_list tracks = disc->tracks;
        actions.push_back({Tr(L"Play"), [tracks] { media::PlayQueue(tracks, 0); }});
        actions.push_back({});
    }
    actions.push_back({Tr(L"Eject"), [drive] { cd::Eject(drive); }});
    ShowActionMenu(m_hwnd, screen, actions);
}

void MainWindow::ShowRemoteNavMenu(const NavPane::Item& item, POINT screen) {
    stream::Provider& provider = *item.provider;
    std::vector<MenuAction> actions;
    switch (item.target) {
    case NavPane::Target::RemotePlaylist:
        ShowRemotePlaylistMenu(*this, provider, item.remoteId, item.text, item.sectionShelf, item.sectionId,
                               screen);
        return;
    default:
        if (item.target == NavPane::Target::LibraryRemote && provider.Can(stream::Capability::CatalogSearch))
            actions.push_back({provider.NamedText(L"Search {0}"), [this, key = std::wstring(provider.Key())] {
                                   BeginSearch({SearchScope::Kind::Provider, key});
                               }});
        if (provider.Can(stream::Capability::CreatePlaylists) && provider.AccountVerified() &&
            FindProviderSection(provider, stream::SectionKind::PlaylistShelf, stream::Shelf::Created) &&
            (item.target == NavPane::Target::LibraryRemote ||
             (item.target == NavPane::Target::RemoteSection &&
              item.sectionKind == stream::SectionKind::PlaylistShelf &&
              item.sectionShelf == stream::Shelf::Created)))
            actions.push_back({provider.NamedText(L"New {0} playlist"),
                               [this, &provider] { CreateRemotePlaylist(*this, provider); }});
        if (item.target == NavPane::Target::LibraryRemote) {
            if (provider.AccountVerified())
                actions.push_back({Tr(L"Sign out"), [&provider] { provider.SignOut(); }});
            else if (!provider.SessionRestoring())
                actions.push_back({Tr(L"Sign in..."), [this, &provider] { provider.SignIn(m_hwnd); }});
        }
        if (provider.AccountVerified()) // re-read the right-clicked section, not just the page on screen
            actions.push_back({L"Refresh", [this, item] {
                                   ArtCache::Get().Invalidate(std::wstring(item.provider->Key()) + L":");
                                   item.provider->RefreshLibrary();
                                   m_forcePageRefresh = true;
                                   OnNavItem(item);
                               }});
        break;
    }
    if (!actions.empty()) ShowActionMenu(m_hwnd, screen, actions);
}

void MainWindow::RefreshCurrentPage() {
    if (!CanRefreshPage()) return;
    m_page->RefreshRequested();
    // Only the refreshed source's pictures are revalidated, once: they keep showing while a
    // conditional request confirms them, and a cover whose URL changed is invalidated by the
    // page that learns the new URL. Local art and other sources are left alone.
    if (auto* provider = ViewProvider()) {
        ArtCache::Get().Invalidate(std::wstring(provider->Key()) + L":");
        provider->RefreshLibrary();
    } else {
        ArtCache::Get().Invalidate(L"");
    }
    m_forcePageRefresh = true;
    Navigate(m_view, false);
    if (m_list) m_list->Refresh();
}

cfg_var_modern::cfg_int* MainWindow::ViewStyleSetting(const View& view) {
    switch (view.kind) {
    case View::Kind::AllMusic:
    case View::Kind::Group:
        return &cfg_viewSongs;
    case View::Kind::Playlist:
    case View::Kind::AudioCd:
        return &cfg_viewPlaylist;
    case View::Kind::AllFavorites:
        return &cfg_viewAllFavorites;
    case View::Kind::AllRecentPlayed:
    case View::Kind::AllMostPlayed:
        return &cfg_viewPlayStats;
    case View::Kind::RemoteShelf:
        return &cfg_viewRemotePlaylists;
    case View::Kind::RemotePlaylist:
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteAlbum:
    case View::Kind::RemoteTopTracks:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroupTracks:
    case View::Kind::RemoteHomeList:
        return &cfg_viewRemoteSongs;
    case View::Kind::Search:
        if (view.sectionId == L"songs" || view.sectionId == L"lyrics") return &cfg_viewSearchSongs;
        return view.sectionId.empty() ? nullptr : &cfg_viewSearchTiles;
    case View::Kind::RemoteArtist:
        return &cfg_viewRemoteArtist;
    case View::Kind::RemoteArtists:
    case View::Kind::RemoteGroups:
    case View::Kind::RemoteSong:
        return nullptr;
    case View::Kind::RemoteAlbums:
        return &cfg_viewRemoteAlbums;
    case View::Kind::Category:
        switch (view.group) {
        case media::GroupKind::Album:
            return &cfg_viewAlbums;
        case media::GroupKind::Artist:
            return &cfg_viewArtists;
        case media::GroupKind::Genre:
            return &cfg_viewGenres;
        default:
            return &cfg_viewCategories;
        }
    default:
        return nullptr;
    }
}

int MainWindow::ViewStyles() const {
    int n = 0;
    for (auto s : {media::ViewStyle::Icons, media::ViewStyle::Tiles, media::ViewStyle::Details})
        if (m_source && m_source->SupportsViewStyle(s)) ++n;
    return n;
}

void MainWindow::ShowViewMenu() {
    if (!m_source) return;
    HMENU menu = CreatePopupMenu();
    const std::pair<media::ViewStyle, const wchar_t*> items[] = {{media::ViewStyle::Icons, L"&Icon"},
                                                                 {media::ViewStyle::Tiles, L"&Expanded tile"},
                                                                 {media::ViewStyle::Details, L"&Details"}};
    const bool browse = m_page && m_page->StyleRebuildsPage();
    for (const auto& [style, label] : items) {
        if (browse && style == media::ViewStyle::Details) continue; // not an album-browsing layout
        UINT flags = MF_STRING;
        if (!browse && !m_source->SupportsViewStyle(style)) flags |= MF_GRAYED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(style) + 1, label);
    }
    // WMP 12: each view with its icon (strip 331 slots 57 icon, 56 expanded tile, 55 details),
    // the current one in bold.
    const UINT current = static_cast<UINT>(m_source->GetViewStyle()) + 1;
    SetMenuDefaultItem(menu, current, FALSE);
    const std::map<UINT, int> icons{{static_cast<UINT>(media::ViewStyle::Icons) + 1, 57},
                                    {static_cast<UINT>(media::ViewStyle::Tiles) + 1, 56},
                                    {static_cast<UINT>(media::ViewStyle::Details) + 1, 55}};
    POINT pt{m_layout.viewOptions.left, m_layout.viewOptions.bottom};
    ClientToScreen(m_hwnd, &pt);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x,
                                   pt.y, m_hwnd, &icons);
    DestroyMenu(menu);
    if (cmd == 0 || cmd == current) return;
    ApplyViewStyle(static_cast<media::ViewStyle>(cmd - 1));
}

void MainWindow::ApplyViewStyle(media::ViewStyle style) {
    // The artist and collected-albums pages swap between cover tiles and a song list
    // grouped by album, so they rebuild the view instead of restyling the current source.
    if (m_page && m_page->StyleRebuildsPage()) {
        if (style == media::ViewStyle::Details) return;
        if (auto* setting = ViewStyleSetting(m_view)) setting->set(static_cast<int>(style));
        Navigate(m_view, false);
        if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
        return;
    }
    m_source->SetViewStyle(style);
    if (auto* setting = ViewStyleSetting(m_view)) setting->set(static_cast<int>(style));
    m_list->SetSource(m_source.get(), true);
    if (m_page) m_page->OnPresentationChanged();
    if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
}

void MainWindow::CycleViewStyle() {
    // WMP 12: clicking the view icon steps Icon -> Expanded tile -> Details -> Icon,
    // skipping styles the current view does not offer.
    if (!m_source) return;
    if (m_page && m_page->StyleRebuildsPage()) {
        const bool expanded = m_source->GetViewStyle() == media::ViewStyle::Tiles;
        ApplyViewStyle(expanded ? media::ViewStyle::Icons : media::ViewStyle::Tiles);
        return;
    }
    int s = static_cast<int>(m_source->GetViewStyle());
    for (int i = 0; i < 3; ++i) {
        s = (s + 1) % 3;
        if (m_source->SupportsViewStyle(static_cast<media::ViewStyle>(s))) {
            ApplyViewStyle(static_cast<media::ViewStyle>(s));
            return;
        }
    }
}

// foobar2000 puts files opened from outside (Explorer, the command line) into the active
// playlist, and into a new "New Playlist" when that one is locked (a smart playlist). They
// belong in Now Playing, so the queue stays active; only an explicit Add files / folder /
// location while a regular playlist is shown targets that playlist.
void MainWindow::KeepQueueActive() {
    auto pm = playlist_manager::get();
    const t_size queue = media::QueuePlaylist(true);
    if (queue != SIZE_MAX && pm->get_active_playlist() != queue) pm->set_active_playlist(queue);
}

void MainWindow::ActivateAddTarget() {
    if (m_view.kind == View::Kind::Playlist &&
        m_view.playlist < playlist_manager::get()->get_playlist_count() &&
        !media::IsAutoPlaylist(m_view.playlist))
        playlist_manager::get()->set_active_playlist(m_view.playlist);
    else
        KeepQueueActive();
}

void MainWindow::CreatePlaylist(bool autoPlaylist) {
    auto pm = playlist_manager::get();
    const t_size p =
        pm->create_playlist(autoPlaylist ? "New Auto Playlist" : "New Playlist", SIZE_MAX, SIZE_MAX);
    if (p == SIZE_MAX) return;
    pm->set_active_playlist(p);
    View v;
    v.library = View::Library::All;
    v.kind = View::Kind::Playlist;
    v.playlist = p;
    Navigate(v, true);
    if (!autoPlaylist) return;
    // foobar2000 auto playlist: starts as the whole library, its own dialog edits the query.
    try {
        auto apm = autoplaylist_manager::get();
        apm->add_client_simple("ALL", "", p, 0);
        if (auto client = apm->query_client(p); client.is_valid()) client->show_ui(p);
    } catch (const std::exception& e) {
        popup_message::g_show(e.what(), "Create auto playlist");
    }
}

void MainWindow::OnAlbumLink(size_t group, size_t line) {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (!groups || group >= groups->size() || line > 3) return;
    const auto& g = (*groups)[group];
    const media::Link& link = g.links[line];
    using Kind = media::Link::Kind;
    View v;
    switch (link.kind) {
    case Kind::None:
        return;
    case Kind::RemoteAlbum:
    case Kind::RemoteArtist: {
        auto* provider = stream::FindProvider(link.provider);
        if (!provider) return;
        const stream::Id sectionId = m_view.provider == provider->Key() ? m_view.sectionId : stream::Id{};
        v = link.kind == Kind::RemoteAlbum ? AlbumView(*provider, link.key, g.lines[0], sectionId)
                                           : ArtistView(*provider, link.key, g.lines[line], sectionId);
        break;
    }
    case Kind::LocalAlbum: { // by name; the group's artist line picks between namesakes
        const media::Group* found = nullptr;
        for (const auto& album : m_library.Groups(media::GroupKind::Album)) {
            if (album.name != link.key) continue;
            if (!found) found = &album;
            if (!g.lines[1].empty() && album.secondary == g.lines[1]) {
                found = &album;
                break;
            }
        }
        if (!found) return;
        v.kind = View::Kind::Group;
        v.group = media::GroupKind::Album;
        v.key = found->key;
        v.name = found->name;
        break;
    }
    case Kind::LocalArtist: {
        media::GroupKind kind = media::GroupKind::ContributingArtist;
        if (!m_library.FindGroup(kind, link.key)) {
            kind = media::GroupKind::Artist;
            if (!m_library.FindGroup(kind, link.key)) return;
        }
        v.kind = View::Kind::Group;
        v.group = kind;
        v.key = v.name = link.key;
        break;
    }
    case Kind::Group:
        v.kind = View::Kind::Group;
        v.group = link.group;
        v.key = link.key;
        v.name = g.lines[line];
        break;
    }
    Navigate(v, true);
}

void MainWindow::PlaceImeWindows() {
    HIMC imc = ImmGetContext(m_hwnd);
    if (!imc) return;
    const RECT box = m_search->Bounds();
    const POINT caret = m_search->CaretClientPoint();
    COMPOSITIONFORM composition{CFS_POINT, caret, {}};
    ImmSetCompositionWindow(imc, &composition);
    // CFS_EXCLUDE keeps the list off the box: below it, or above near the screen bottom.
    CANDIDATEFORM candidate{0, CFS_EXCLUDE, {caret.x, box.bottom}, box};
    ImmSetCandidateWindow(imc, &candidate);
    ImmReleaseContext(m_hwnd, imc);
}

void MainWindow::GoBack() {
    if (m_historyPos == 0) return;
    --m_historyPos;
    Navigate(m_history[m_historyPos], false);
}

void MainWindow::GoForward() {
    if (m_historyPos + 1 >= m_history.size()) return;
    ++m_historyPos;
    Navigate(m_history[m_historyPos], false);
}

void MainWindow::OnNavItem(const NavPane::Item& item) {
    View v;
    switch (item.target) {
    case NavPane::Target::Playlist:
        v.library = item.playlist == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(item.playlist)
                        ? View::Library::Local
                        : View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = item.playlist;
        break;
    case NavPane::Target::Queue:
        v.library = View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = media::QueuePlaylist(true);
        break;
    case NavPane::Target::AudioCd:
    case NavPane::Target::DiscsHeader: // the first drive
        if (cd::Drives().empty()) return;
        v.library = View::Library::Local;
        v.kind = View::Kind::AudioCd;
        v.playlist = item.target == NavPane::Target::AudioCd ? item.playlist : cd::Drives().front().drive;
        cd::ReadDrive(static_cast<wchar_t>(v.playlist)); // looks again in a drive found empty
        break;
    case NavPane::Target::SmartHeader:
        v.library = View::Library::Local;
        v.kind = View::Kind::SmartPlaylists;
        break;
    case NavPane::Target::Category:
        v.kind = View::Kind::Category;
        v.group = static_cast<media::GroupKind>(item.playlist);
        break;
    case NavPane::Target::PlaylistsHeader:
        v.library = View::Library::All;
        v.kind = View::Kind::Playlists; // WMP's Playlists page
        break;
    case NavPane::Target::LibraryAll:
        v.library = View::Library::All;
        v.kind = View::Kind::AllMusic;
        break;
    case NavPane::Target::AllFavorites:
        v.library = View::Library::All;
        v.kind = View::Kind::AllFavorites;
        break;
    case NavPane::Target::AllRecentPlayed:
        v.library = View::Library::All;
        v.kind = View::Kind::AllRecentPlayed;
        break;
    case NavPane::Target::AllMostPlayed:
        v.library = View::Library::All;
        v.kind = View::Kind::AllMostPlayed;
        break;
    case NavPane::Target::AllDownloads:
        v.library = View::Library::All;
        v.kind = View::Kind::Downloads;
        break;
    case NavPane::Target::Home:
        v.library = View::Library::All;
        v.kind = View::Kind::Home;
        break;
    case NavPane::Target::LibraryLocal:
        v.library = View::Library::Local;
        v.kind = View::Kind::AllMusic;
        break;
    case NavPane::Target::LibraryRemote:
        v = ProviderLandingView(*item.provider);
        break;
    case NavPane::Target::RemoteSection:
        if (const auto section = FindProviderSection(*item.provider, item.sectionId))
            v = SectionView(*item.provider, *section);
        else
            return;
        break;
    case NavPane::Target::RemotePlaylist:
        v = PlaylistView(*item.provider, item.remoteId, item.text, item.sectionShelf, item.sectionId);
        break;
    default:
        return;
    }
    Navigate(v, true);
}

void MainWindow::OnNavMove(const NavPane::Item& dragged, const NavPane::Item& target, bool after) {
    if (dragged.target == NavPane::Target::SmartHeader && target.target == NavPane::Target::Category) {
        const auto kinds = m_nav->Categories();
        const auto it = std::find(kinds.begin(), kinds.end(), static_cast<media::GroupKind>(target.playlist));
        if (it == kinds.end()) return;
        SetNavSmartPosition(static_cast<int>(it - kinds.begin()) + (after ? 1 : 0));
        return;
    }
    if (dragged.target == NavPane::Target::Category && target.target == NavPane::Target::SmartHeader) {
        auto kinds = m_nav->Categories();
        const auto it =
            std::find(kinds.begin(), kinds.end(), static_cast<media::GroupKind>(dragged.playlist));
        if (it == kinds.end()) return;
        const auto kind = *it;
        int at = m_nav->SmartPosition();
        if (static_cast<int>(it - kinds.begin()) < at) --at; // removing it shifts the boundary
        kinds.erase(it);
        at = std::clamp(at, 0, static_cast<int>(kinds.size()));
        kinds.insert(kinds.begin() + at, kind);
        SetNavCategories(std::move(kinds));
        // Above the header row keeps the category before the section, below puts it after.
        SetNavSmartPosition(at + (after ? 0 : 1));
        return;
    }
    if (dragged.target == NavPane::Target::Category && target.target == NavPane::Target::Category) {
        auto kinds = m_nav->Categories();
        const auto kind = static_cast<media::GroupKind>(dragged.playlist);
        const auto destination = static_cast<media::GroupKind>(target.playlist);
        auto from = std::find(kinds.begin(), kinds.end(), kind);
        if (from == kinds.end()) return;
        kinds.erase(from);
        auto to = std::find(kinds.begin(), kinds.end(), destination);
        if (to == kinds.end()) return;
        const size_t index = static_cast<size_t>(to - kinds.begin()) + (after ? 1 : 0);
        kinds.insert(kinds.begin() + static_cast<std::ptrdiff_t>(index), kind);
        SetNavCategories(std::move(kinds));
        return;
    }
    if (dragged.target == NavPane::Target::Playlist && target.target == NavPane::Target::Playlist) {
        auto pm = playlist_manager::get();
        const t_size count = pm->get_playlist_count();
        if (!count) return;
        auto smart = [](t_size p) {
            return p == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(p);
        };
        const bool groupSmart = smart(dragged.playlist);
        if (smart(target.playlist) != groupSmart) return;
        auto inGroup = [&](t_size p) {
            return groupSmart ? smart(p) : !media::IsSpecial(p) && !media::IsAutoPlaylist(p);
        };
        std::vector<t_size> group;
        for (t_size p = 0; p < count; ++p)
            if (inGroup(p)) group.push_back(p);
        auto from = std::find(group.begin(), group.end(), dragged.playlist);
        if (from == group.end()) return;
        const t_size moving = *from;
        group.erase(from);
        auto to = std::find(group.begin(), group.end(), target.playlist);
        if (to == group.end()) return;
        const size_t index = static_cast<size_t>(to - group.begin()) + (after ? 1 : 0);
        group.insert(group.begin() + static_cast<std::ptrdiff_t>(index), moving);
        std::vector<t_size> full(count);
        for (t_size i = 0; i < count; ++i)
            full[i] = i;
        size_t next = 0;
        for (auto& value : full)
            if (inGroup(value)) {
                if (next >= group.size()) return;
                value = group[next++];
            }
        if (next != group.size()) return;
        pm->reorder(full.data(), count); // on_playlists_reorder moves the views along
        return;
    }
    // A provider's playlists: within the own or the saved ones, synced to the service.
    if (dragged.target != NavPane::Target::RemotePlaylist ||
        target.target != NavPane::Target::RemotePlaylist || dragged.provider != target.provider ||
        dragged.sectionId != target.sectionId || dragged.sectionKind != stream::SectionKind::PlaylistShelf ||
        !dragged.reorderChildren || !dragged.provider->Can(stream::Capability::ReorderPlaylists))
        return;
    stream::Provider& provider = *dragged.provider;
    std::vector<stream::Id> ids;
    std::set<stream::Id> seenSections;
    bool moved = false;
    for (const auto& section : provider.Sections()) {
        if (section.kind != stream::SectionKind::PlaylistShelf || !seenSections.emplace(section.id).second)
            continue;
        auto playlists = provider.SectionPlaylists(section.id);
        if (section.id == dragged.sectionId) {
            auto byId = [](const stream::Id& wanted) {
                return [&wanted](const stream::Playlist& playlist) { return playlist.id == wanted; };
            };
            auto from = std::find_if(playlists.begin(), playlists.end(), byId(dragged.remoteId));
            if (from == playlists.end()) return;
            stream::Playlist value = std::move(*from);
            playlists.erase(from);
            auto to = std::find_if(playlists.begin(), playlists.end(), byId(target.remoteId));
            if (to == playlists.end()) return;
            playlists.insert(to + (after ? 1 : 0), std::move(value));
            moved = true;
        }
        for (const auto& playlist : playlists)
            ids.push_back(playlist.id);
    }
    if (!moved) return;
    provider.ReorderPlaylists(std::move(ids)); // the provider shows the new order at once
    RefreshProviderSections();
}

void MainWindow::ShowOrganizeMenu() {
    POINT pt{m_layout.organize.left, m_layout.organize.bottom};
    ClientToScreen(m_hwnd, &pt);
    ShowMainMenu(
        m_hwnd, pt,
        {{L"Manage music library...", [] { library_manager::get()->show_preferences(); }},
         {L"Rescan library", [] { library_manager::get()->rescan(); }},
         {L"", nullptr},
         {L"Add files...",
          [this] {
              ActivateAddTarget();
              standard_commands::main_add_files();
          }},
         {L"Add folder...",
          [this] {
              ActivateAddTarget();
              standard_commands::main_add_directory();
          }},
         {L"Add location...",
          [this] {
              ActivateAddTarget();
              standard_commands::run_main(standard_commands::guid_main_add_location);
          }},
         {L"", nullptr},
         {L"Language",
          nullptr,
          {{L"Follow Windows",
            [this] { ApplyLanguage(Language::Auto); },
            {},
            CurrentLanguageSetting() == Language::Auto},
           {L"English",
            [this] { ApplyLanguage(Language::English); },
            {},
            CurrentLanguageSetting() == Language::English},
           {L"中文 (Chinese)",
            [this] { ApplyLanguage(Language::Chinese); },
            {},
            CurrentLanguageSetting() == Language::Chinese}}},
         {L"", nullptr},
         {L"Preferences...", [] { standard_commands::run_main(standard_commands::guid_main_preferences); }}});
    SetHoverWidget(nullptr); // the menu loop swallowed the mouse leave
}

// WMP: right-click Local Music > Customize navigation pane picks the categories listed under it.
void MainWindow::ShowNavCustomizeMenu(POINT screen) {
    std::vector<MenuAction> categories;
    const auto shown = m_nav->Categories();
    for (size_t i = 0; i < media::kGroupKinds; ++i) {
        const auto kind = static_cast<media::GroupKind>(i);
        const bool on = std::find(shown.begin(), shown.end(), kind) != shown.end();
        MenuAction a{CategoryLabel(kind), [this, kind, on, shown] {
                         auto next = shown;
                         if (on)
                             next.erase(std::find(next.begin(), next.end(), kind));
                         else
                             next.push_back(kind);
                         std::sort(next.begin(), next.end()); // always listed in WMP order
                         SetNavCategories(std::move(next));
                     }};
        a.checked = on;
        a.checkbox = true;
        categories.push_back(std::move(a));
    }
    categories.push_back({});
    categories.push_back(
        {L"Restore defaults", [this] {
             SetNavCategories({media::GroupKind::Artist, media::GroupKind::Album, media::GroupKind::Genre});
         }});
    View all;
    all.library = View::Library::Local;
    all.kind = View::Kind::AllMusic;
    std::vector<MenuAction> actions{{L"Open", [this, all] { Navigate(all, true); }},
                                    {L"Customize navigation pane", {}, std::move(categories)}};
    ShowActionMenu(m_hwnd, screen, actions);
}

void MainWindow::SetNavCategories(std::vector<media::GroupKind> kinds) {
    std::string digits;
    for (auto k : kinds)
        digits += static_cast<char>('0' + static_cast<int>(k));
    cfg_navCategories.set(digits.c_str());
    m_nav->SetCategories(std::move(kinds));
    cfg_navSmartAt.set(m_nav->SmartPosition()); // clamped when a category disappeared
    RepaintBreadcrumb();
}

void MainWindow::SetNavSmartPosition(int position) {
    position = std::clamp(position, 0, static_cast<int>(m_nav->Categories().size()));
    cfg_navSmartAt.set(position);
    m_nav->SetSmartPosition(position);
    RepaintBreadcrumb();
}

void MainWindow::ShowPlaylistMenu(size_t playlist, POINT screen) {
    auto pm = playlist_manager::get();
    std::vector<MenuAction> actions;
    if (playlist != SIZE_MAX && playlist < pm->get_playlist_count()) {
        actions.push_back({L"Play", [this, playlist] {
                               auto p = playlist_manager::get();
                               p->set_active_playlist(playlist);
                               const t_size focus = p->playlist_get_focus_item(playlist);
                               if (p->playlist_get_item_count(playlist))
                                   p->playlist_execute_default_action(playlist,
                                                                      focus == SIZE_MAX ? 0 : focus);
                               KeepQueueActive();
                           }});
        if (media::IsQueue(playlist))
            actions.push_back({TrMenu(L"Clear Now Playing"), [this] { ClearNowPlaying(); }});
        else {
            auto contents = [playlist] {
                metadb_handle_list items;
                playlist_manager::get()->playlist_get_all_items(playlist, items);
                return items;
            };
            actions.push_back({L"Play next", [contents] { media::PlayNext(contents()); }});
            actions.push_back({L"Add to queue", [contents] { media::AddToQueue(contents()); }});
            std::vector<MenuAction> targets;
            for (const auto& [id, name] : LocalPlaylistTargets(playlist)) {
                targets.push_back({name.empty() ? L"Unnamed playlist" : name, [contents, id] {
                                       auto items = contents();
                                       if (items.get_count())
                                           playlist_manager::get()->playlist_add_items(
                                               static_cast<t_size>(id), items, pfc::bit_array_false());
                                   }});
            }
            if (targets.empty()) targets.push_back({L"No playlists", nullptr});
            actions.push_back({L"Add to playlist", nullptr, std::move(targets)});
        }
        if (media::IsSmart(playlist))
            actions.push_back({L"Edit smart playlist...", [playlist] {
                                   if (auto c = autoplaylist_manager::get()->query_client(playlist);
                                       c.is_valid())
                                       c->show_ui(playlist);
                               }});
        // The queue and Favorites are built in: no renaming or removing.
        if (!media::IsQueue(playlist) && playlist != media::FavoritesPlaylist(false)) {
            actions.push_back({L"Rename...", [this, playlist] {
                                   playlist_manager::get()->set_active_playlist(playlist);
                                   standard_commands::run_main(standard_commands::guid_main_rename_playlist);
                                   KeepQueueActive();
                               }});
            actions.push_back(
                {L"Remove", [playlist] { playlist_manager::get()->remove_playlist(playlist); }});
        }
        actions.push_back({L"", nullptr});
    }
    for (auto& a : AddPlaylistActions())
        actions.push_back(std::move(a));
    actions.push_back({L"", nullptr});
    actions.push_back({L"Load playlist...", [] { standard_commands::main_load_playlist(); }});
    actions.push_back({L"Save all playlists...", [] { standard_commands::main_save_all_playlists(); }});
    actions.push_back({L"Restore smart playlists", [this] {
                           media::EnsureSpecialPlaylists(true);
                           m_nav->Rebuild();
                       }});
    ShowActionMenu(m_hwnd, screen, actions);
}

std::vector<MenuAction> MainWindow::AddPlaylistActions() {
    // As in the owner's localized foobar2000: Add > New playlist / New smart playlist /
    // Preset smart playlists > (library, never played, history, most played, recently
    // added | Favorites | rated 5..1, unrated).
    auto open = [this](t_size p) {
        if (p == SIZE_MAX) return;
        m_nav->Rebuild();
        View v;
        v.library = View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = p;
        Navigate(v, true);
    };
    std::vector<MenuAction> presets;
    int group = 0;
    for (const auto& preset : media::SmartPresets()) {
        if (preset.group != group && !presets.empty()) presets.push_back({L"", nullptr});
        group = preset.group;
        presets.push_back({Wide(preset.name), [open, &preset] { open(media::AddSmartPreset(preset)); }});
    }
    return {{L"Add",
             nullptr,
             {{L"New playlist", [this] { CreatePlaylist(false); }},
              {L"New smart playlist", [open] { open(media::NewSmartPlaylist()); }},
              {L"Preset smart playlists", nullptr, std::move(presets)}}}};
}

void MainWindow::AddDroppedFiles(IDataObject* data) {
    // Drop target: the playlist on screen, else the active playlist (created if needed).
    auto pm = playlist_manager::get();
    t_size target = m_view.kind == View::Kind::Playlist ? m_view.playlist : pm->get_active_playlist();
    if (target == SIZE_MAX || target >= pm->get_playlist_count())
        target = pm->find_or_create_playlist("Default");
    auto notify = process_locations_notify::create([target](metadb_handle_list_cref items) {
        auto p = playlist_manager::get();
        if (target < p->get_playlist_count()) {
            p->playlist_undo_backup(target);
            p->playlist_add_items(target, items, pfc::bit_array_false());
        }
    });
    playlist_incoming_item_filter_v2::get()->process_dropped_files_async(
        data, playlist_incoming_item_filter_v2::op_flag_delay_ui, m_hwnd, notify);
}

void MainWindow::SetSearchFocus(bool focused) {
    m_search->SetFocused(focused);
    m_list->SetFocused(!focused);
    m_nav->SetFocused(false);
    if (focused)
        ShowSearchPopup();
    else
        HideSearchPopup();
}

void MainWindow::FocusNav() {
    HideSearchPopup();
    m_search->SetFocused(false);
    m_list->SetFocused(false);
    m_nav->SetFocused(true);
}

// ---- Core state ------------------------------------------------------------------------

void MainWindow::OnPlaybackStateChanged() {
    auto pc = playback_control::get();
    const bool playing = pc->is_playing(), paused = pc->is_paused();
    if (playing == m_playing && paused == m_paused) return;
    m_playing = playing;
    m_paused = paused;
    for (int id : {kPlay, kStop, kPrev, kNext})
        if (auto* w = WidgetById(id)) w->Invalidate();
    if (m_taskbar) m_taskbar->SetPlayState(playing, paused);
    if (m_playerTaskbar) m_playerTaskbar->SetPlayState(playing, paused);
    if (m_list) m_list->Refresh(); // playing marker
    if (m_pane) m_pane->SetPlaying(playing, paused);
    if (playing && !paused && m_npLyrics && m_npLyrics->synced) StartLyricsFrames();
    StartSeekFrames();
    OnTrackChanged(); // paused resume still has codec/bitrate information
}

// A track's title and artist as the lists show them: the provider's record, a repaired cue sheet.
void MainWindow::TrackText(const metadb_handle_ptr& track, std::wstring& title, std::wstring& artist) const {
    if (auto* provider = stream::ProviderOf(track)) {
        if (const auto record = provider->FindTrack(provider->TrackId(track))) {
            if (!record->title.empty()) title = record->title;
            if (!record->artists.empty()) artist = stream::JoinArtists(record->artists);
        }
    }
    if (const media::RepairedText* r = media::CueCatalog::Get().Repair(track)) { // mis-decoded cue sheet
        if (!r->title.empty()) title = r->title;
        if (!r->artist.empty()) artist = r->artist;
    }
}

// Shows a song: with its cover at once when the cover is at hand (or known missing), else the
// song shown before stays whole until the cover has come or kArtHoldMs has passed.
void MainWindow::ShowTrack(const metadb_handle_ptr& track, std::wstring title, std::wstring artist, bool starting) {
    HeldTrack next{track, lyrics::Service::TrackIdentity(track), std::move(title), std::move(artist)};
    if (next.identity == m_artTrackIdentity && !starting) return;
    winrt::com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(track, std::max(PxI(TransportHeight()), PxI(64)), &art);
    const bool shownBefore = !m_artTrackIdentity.empty();
    if (state == ArtCache::State::Pending && !art && shownBefore) {
        const bool already = m_held.track.is_valid();
        m_held = std::move(next);
        if (!already) SetTimer(m_hwnd, kArtHoldTimer, kArtHoldMs, nullptr);
        return;
    }
    if (m_held.track.is_valid()) {
        KillTimer(m_hwnd, kArtHoldTimer);
        m_held = {};
    }
    ApplyShownTrack(next, state == ArtCache::State::Missing ? nullptr : art);
}

void MainWindow::ReleaseHeldTrack(bool timedOut) {
    if (!m_held.track.is_valid()) return;
    winrt::com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(m_held.track, std::max(PxI(TransportHeight()), PxI(64)), &art);
    if (state == ArtCache::State::Pending && !art && !timedOut) return;
    KillTimer(m_hwnd, kArtHoldTimer);
    const HeldTrack held = std::move(m_held);
    m_held = {};
    ApplyShownTrack(held, state == ArtCache::State::Missing ? nullptr : art); // its cover, or the placeholder
}

void MainWindow::ApplyShownTrack(const HeldTrack& track, winrt::com_ptr<IWICBitmapSource> art) {
    m_trackTitle = track.title;
    m_trackArtist = track.artist;
    m_artTrackIdentity = track.identity;
    ++m_artGeneration;
    m_art = std::move(art);
    m_lyricsArt = m_art;
    if (m_pane) m_pane->SetTrack(m_trackTitle, m_trackArtist, m_lyricsArt);
    const std::optional<uint32_t> tint = CoverTint(m_art.get());
    if (m_pane) m_pane->SetCoverTint(tint);
    if (tint && tint != m_windowCoverRgb) {
        m_windowCoverRgb = tint;
        ApplyTint(true);
    }
    RepaintNowPlaying();
    if (m_taskbar) m_taskbar->SetCoverTrack(track.track);
    if (m_playerTaskbar) m_playerTaskbar->SetCoverTrack(track.track);
    if (m_player) m_player->OnPlayback();
    std::wstring caption = m_trackTitle.empty() ? std::wstring(kAppName)
                                                : m_trackTitle + (m_trackArtist.empty() ? L"" : L" - " + m_trackArtist) +
                                                      L" - " + kAppName;
    SetWindowTextW(m_hwnd, caption.c_str());
}

// Playback is starting towards a song (Next, Previous, a list started): it shows now, while its
// audio is still being opened.
void MainWindow::PreviewStartingTrack(play_control::t_track_command command) {
    auto* scheduler = media::PlaybackScheduler::Existing();
    const metadb_handle_ptr track = scheduler ? scheduler->StartingTrack(command) : metadb_handle_ptr();
    if (!track.is_valid()) return;
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    pfc::string8 title, artist;
    track->format_title(nullptr, title, s_title, nullptr);
    track->format_title(nullptr, artist, s_artist, nullptr);
    std::wstring t = Wide(title), a = Wide(artist);
    TrackText(track, t, a);
    if (lyrics::Service::TrackIdentity(track) != m_artTrackIdentity) m_startingFrom = m_artTrackIdentity;
    ShowTrack(track, std::move(t), std::move(a), true);
}

// Stopped: the song shown goes with its cover and text (owner, 2026-11-20: no placeholder under
// the last song's name).
void MainWindow::ClearShownTrack() {
    KillTimer(m_hwnd, kArtHoldTimer);
    m_held = {};
    m_startingFrom.clear();
    ResetLyrics();
    m_trackTitle.clear();
    m_trackArtist.clear();
    m_artTrackIdentity.clear();
    ++m_artGeneration;
    m_art = nullptr;
    m_lyricsArt = nullptr;
    if (m_pane) m_pane->SetTrack({}, {}, nullptr);
    if (m_taskbar) m_taskbar->SetCoverTrack({});
    if (m_playerTaskbar) m_playerTaskbar->SetCoverTrack({});
    RepaintNowPlaying();
}

void MainWindow::OnTrackChanged(bool playbackChanged) {
    struct NotifyPlayer { // after this window's own state is current
        MainWindow& window;
        ~NotifyPlayer() {
            if (window.m_player) window.m_player->OnPlayback();
        }
    } notifyPlayer{*this};
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    auto pc = playback_control::get();
    metadb_handle_ptr now;
    const bool loaded = pc->get_now_playing(now);
    const std::wstring trackIdentity = loaded ? lyrics::Service::TrackIdentity(now) : std::wstring();
    // Output-device restarts and playlist rebuilds can briefly unload the current item.
    // Keep the last complete presentation until the core reports the same or another track.
    const bool retainStoppedTrack = !loaded && m_lyricsTrack.is_valid();
    pfc::string8 title, artist;
    if (loaded) {
        pc->playback_format_title(nullptr, title, s_title, nullptr, playback_control::display_level_all);
        pc->playback_format_title(nullptr, artist, s_artist, nullptr, playback_control::display_level_all);
    } else if (!retainStoppedTrack) {
        m_art = nullptr;
        m_lyricsArt = nullptr;
    }
    std::wstring newTitle = Wide(title), newArtist = Wide(artist);
    if (loaded) TrackText(now, newTitle, newArtist);
    if (!loaded && !retainStoppedTrack) {
        m_trackTitle.clear();
        m_trackArtist.clear();
    } else if (loaded && trackIdentity == m_artTrackIdentity) { // the song shown: its text may have changed
        m_trackTitle = newTitle;
        m_trackArtist = newArtist;
        m_startingFrom.clear();
    } else if (loaded && trackIdentity != m_startingFrom) {
        // Another song (not the last one still reported while the next one starts).
        ShowTrack(now, newTitle, newArtist, false);
    }
    if (loaded || !retainStoppedTrack) {
        m_length = loaded ? pc->playback_get_length() : 0;
        m_position = loaded ? pc->playback_get_position() : 0;
    }

    // Stream format: codec, bit rate, sample rate, bit depth, channels. A paused resumed
    // track still has all of this; gate on a loaded track, not on is_playing().
    static titleformat_object::ptr s_stream;
    if (!s_stream)
        titleformat_compiler::get()->compile_safe(
            s_stream, "[%codec%]|[%bitrate%]|[%samplerate%]|[$info(bitspersample)]|[%channels%]");
    if (!retainStoppedTrack) m_streamInfo.clear();
    if (loaded) {
        pfc::string8 raw;
        pc->playback_format_title(nullptr, raw, s_stream, nullptr, playback_control::display_level_all);
        std::vector<std::wstring> f;
        std::wstringstream in(Wide(raw));
        for (std::wstring part; std::getline(in, part, L'|');)
            f.push_back(part);
        f.resize(5);
        auto add = [&](const std::wstring& s) {
            if (s.empty()) return;
            if (!m_streamInfo.empty()) m_streamInfo += L"   ";
            m_streamInfo += s;
        };
        add(f[0]);
        if (!f[1].empty()) add(f[1] + L" kbps");
        if (!f[2].empty()) {
            wchar_t khz[32];
            const double rate = _wtof(f[2].c_str()) / 1000.0;
            swprintf_s(khz, std::fmod(rate, 1.0) == 0 ? L"%.0f kHz" : L"%.1f kHz", rate);
            add(khz);
        }
        if (!f[3].empty()) add(f[3] + L" bit");
        if (!f[4].empty()) {
            std::wstring ch = f[4];
            ch[0] = static_cast<wchar_t>(::towupper(ch[0]));
            add(ch);
        }
    }
    const bool lyricsTrackChanged = loaded && trackIdentity != m_lyricsTrackIdentity;
    const bool trackChanged = loaded && (playbackChanged || lyricsTrackChanged);
    RefreshNowPlayingArt(true);
    if (lyricsTrackChanged)
        RequestLyrics(now, false);
    else if (loaded)
        m_lyricsTrack = now; // a new handle object can represent the same logical track
    SchedulePaneQueueRefresh(trackChanged);
    RepaintStreamInfo();
    UpdateFavoriteState();
    m_seekPaint.Request();
    FlushPlaybackPaints();
    if (m_list) m_list->Refresh();

    std::wstring caption =
        m_trackTitle.empty()
            ? std::wstring(kAppName)
            : m_trackTitle + (m_trackArtist.empty() ? L"" : L" - " + m_trackArtist) + L" - " + kAppName;
    SetWindowTextW(m_hwnd, caption.c_str());
}

void MainWindow::OnPlaybackTime() {
    auto pc = playback_control::get();
    metadb_handle_ptr track;
    if (pc->get_now_playing(track)) {
        m_position = pc->playback_get_position();
        m_length = pc->playback_get_length();
    } else if (!m_lyricsTrack.is_valid()) {
        m_position = 0;
        m_length = 0;
    }
    if (!m_seeking) {
        m_seekPaint.Request();
        FlushPlaybackPaints();
    }
    RepaintNowPlaying();
    if (m_pane) m_pane->OnPlaybackTime();
    StartSeekFrames(); // after a seek, a restore or a song change
    if (m_player) m_player->OnTime();
}

// The seek bar's head follows the music on the compositor's frames, a device pixel at a time,
// instead of jumping with the playback time callback, which comes once a second.
void MainWindow::StartSeekFrames() {
    if (!m_playing || m_paused || !PlaybackPaintAllowed()) return;
    if (!m_frameClock) m_frameClock = std::make_unique<gfx::FrameClock>(m_hwnd, kFrameMessage);
    m_frameClock->Start();
}

bool MainWindow::TickSeek() {
    if (!m_playing || m_paused || m_seeking || m_length <= 0) return false;
    auto* seek = WidgetById(kSeek);
    if (!seek) return false;
    const RECT& r = seek->Bounds();
    if (r.right <= r.left) return false;
    const double position = playback_control::get()->playback_get_position();
    const long head = std::lround((r.right - r.left) * std::clamp(position / m_length, 0.0, 1.0));
    if (head != m_seekHead) {
        m_seekHead = head;
        m_position = position;
        seek->Invalidate();
    }
    return true;
}

void MainWindow::OnVolumeChanged() {
    auto pc = playback_control::get();
    const bool muted = pc->is_muted();
    const float v = muted ? m_volume : SliderFromDb(pc->get_volume());
    if (muted != m_muted) {
        m_muted = muted;
        if (auto* w = WidgetById(kMute)) w->Invalidate();
    }
    if (std::abs(v - m_volume) > 1e-4f) {
        m_volume = v;
        for (int id : {kVolume, kMute}) // the speaker icon shows the level
            if (auto* w = WidgetById(id)) w->Invalidate();
        if (m_volumeTipShown) ShowVolumeTip(VolumeThumbX()); // keep a visible bubble current
    }
    if (m_player) m_player->OnVolume();
}

void MainWindow::OnPlaybackOrderChanged() {
    auto& scheduler = media::PlaybackScheduler::Get();
    scheduler.CorePlaybackOrderChanged();
    if (m_applyingOrder) return;
    fb2k::inMainThread([this, alive = std::weak_ptr<int>(m_alive)] {
        if (alive.expired()) return;
        if (auto* host = media::PlaybackScheduler::Existing())
            UpdateShuffleRepeat(host->QueueShuffleEnabled(), static_cast<Repeat>(host->QueueRepeatMode()));
    });
}

void MainWindow::UpdateShuffleRepeat(bool shuffle, Repeat repeat) {
    struct NotifyPlayer {
        MainWindow& window;
        ~NotifyPlayer() {
            if (window.m_player) window.m_player->OnPlayback();
        }
    } notifyPlayer{*this};
    m_shuffle = shuffle;
    m_repeat = repeat;
    // Repaint even when the values did not change: at startup they come from cfg after the
    // buttons were first painted, and the toggles must show the restored state.
    for (int id : {kShuffle, kRepeat})
        if (auto* w = WidgetById(id)) w->Invalidate();
    cfg_shuffle = shuffle ? 1 : 0;
    cfg_repeat = static_cast<int>(repeat);
    SchedulePaneQueueRefresh();
}

void MainWindow::SetShuffleRepeat(bool shuffle, Repeat repeat) {
    if (media::radio::Active()) { // no shuffle and no list repeat on a station
        shuffle = false;
        if (repeat == Repeat::All) repeat = Repeat::Off;
    }
    UpdateShuffleRepeat(shuffle, repeat);
    ApplyPlaybackOrder();
}

// A station started: shuffle and list repeat go (kept for after it); it ended: they come back.
// Its songs moving on can change whether Previous does anything, so the buttons follow each call.
void MainWindow::OnRadioChanged() {
    const bool active = media::radio::Active();
    if (active != m_radioActive) {
        m_radioActive = active;
        if (active) {
            if (!m_beforeRadio) m_beforeRadio = std::make_pair(m_shuffle, static_cast<int>(m_repeat));
            SetShuffleRepeat(false, m_repeat == Repeat::One ? Repeat::One : Repeat::Off);
        } else if (m_beforeRadio) {
            const auto [shuffle, repeat] = *std::exchange(m_beforeRadio, std::nullopt);
            SetShuffleRepeat(shuffle, static_cast<Repeat>(repeat));
        }
        SchedulePaneQueueRefresh();
    }
    for (int id : {kShuffle, kRepeat, kPrev})
        if (auto* w = WidgetById(id)) w->Invalidate();
    if (m_player) m_player->RadioChanged();
}

void MainWindow::CycleRepeat() {
    const auto modes = media::transport::RepeatPressed({m_shuffle, static_cast<int>(m_repeat)});
    SetShuffleRepeat(modes.shuffle, static_cast<Repeat>(modes.repeat));
}

void MainWindow::SelectPlaybackMode(bool shuffle, int repeat) {
    SetShuffleRepeat(shuffle, static_cast<Repeat>(std::clamp(repeat, 0, 2)));
}

void MainWindow::ApplyPlaybackOrder() {
    m_applyingOrder = true;
    media::PlaybackScheduler::Get().SetPlaybackMode(m_shuffle, static_cast<int>(m_repeat));
    m_applyingOrder = false;
}

void MainWindow::ApplyLanguage(Language language) {
    SetLanguageSetting(language);
    for (auto& w : m_widgets)
        w->Invalidate();
    if (m_nav) m_nav->Rebuild();
    RepaintBreadcrumb();
    RepaintStreamInfo();
    if (m_list) m_list->Refresh();
    UpdateLayout(); // button widths follow the text
}

void MainWindow::UpdateFavoriteState() {
    metadb_handle_ptr now;
    const bool fav = playback_control::get()->get_now_playing(now) && media::IsFavorite(now);
    if (m_taskbar) m_taskbar->SetFavorite(fav);
    if (m_playerTaskbar) m_playerTaskbar->SetFavorite(fav);
    if (fav != m_npFavorite) {
        m_npFavorite = fav;
        RepaintNowPlaying();
        if (m_player) m_player->OnPlayback();
    }
}

void MainWindow::OnPlaylistContentChanged(size_t playlist) {
    if (playlist == media::FavoritesPlaylist(false)) UpdateFavoriteState();
    if (m_player && playlist == playlist_manager::get()->get_playing_playlist()) m_player->WallAlbumsChanged();
    if (media::IsQueue(playlist)) SchedulePaneQueueRefresh();
    if (m_view.kind == View::Kind::Playlist && m_view.playlist == playlist && m_source) {
        static_cast<media::PlaylistSource*>(m_source.get())->Rebuild();
        m_list->Refresh();
    } else if (m_list) {
        m_list->Refresh(); // playing marker may have moved
    }
}

void MainWindow::OnPlaylistSelectionChanged(size_t playlist) {
    if (m_view.kind == View::Kind::Playlist && m_view.playlist == playlist && m_list) m_list->Refresh();
}

void MainWindow::OnPlaylistsMoved(const std::function<size_t(size_t)>& remap) {
    // A page whose playlist was deleted becomes that section's Playlists page.
    auto fix = [&remap](View& view) {
        if (view.kind != View::Kind::Playlist) return;
        if (const size_t moved = remap(view.playlist); moved != SIZE_MAX) {
            view.playlist = moved;
            return;
        }
        const bool smart = view.library == View::Library::Local;
        view = {};
        view.library = smart ? View::Library::Local : View::Library::All;
        view.kind = smart ? View::Kind::SmartPlaylists : View::Kind::Playlists;
    };
    fix(m_view);
    for (auto& view : m_history)
        fix(view);
    OnPlaylistsChanged();
}

void MainWindow::OnPlaylistsChanged() {
    SchedulePaneQueueRefresh();
    if (!m_nav) return;
    m_nav->Rebuild();
    if (m_view.kind == View::Kind::Playlist || m_view.kind == View::Kind::Playlists ||
        m_view.kind == View::Kind::SmartPlaylists)
        Navigate(m_view, false); // index, name or the set of playlists may have changed
}

void MainWindow::OnPlaylistLockChanged() {
    // foobar2000 restores auto playlists after the UI is up; each one shows up as a lock.
    // The lock's filter mask tells the nav pane it is an auto playlist before the client is
    // registered, so this one rebuild is enough.
    if (!m_nav) return;
    m_nav->Rebuild();
    if (m_view.kind == View::Kind::Playlists || m_view.kind == View::Kind::SmartPlaylists)
        Navigate(m_view, false);
    else
        RepaintBreadcrumb();
}

void MainWindow::ScheduleLibraryReload() {
    if (m_hwnd) SetTimer(m_hwnd, kLibraryTimer, 400, nullptr); // debounce bursts of library events
}

RECT MainWindow::ClusterRect(const D2D1_RECT_F& r) const {
    const POINT o = m_layout.cluster;
    return {o.x + PxI(r.left), o.y + PxI(r.top), o.x + PxI(r.right), o.y + PxI(r.bottom)};
}

D2D1_RECT_F MainWindow::ClusterRegion(const RECT& r) const {
    const POINT o = m_layout.cluster;
    return {(r.left - o.x) / m_scale, (r.top - o.y) / m_scale, (r.right - o.x) / m_scale,
            (r.bottom - o.y) / m_scale};
}

// With the original artwork the bars follow WMP 12's bitmaps (27-DIP orbs, 51-DIP cluster).
// WMP 12 at 150 %: the nav bar is 35 px with the 26 px orbs nearly filling it; ours keeps the larger
// orbs (22 DIP) with 3-4 DIP above and below.
float MainWindow::NavBarHeight() const {
    return m_win7Art ? 29.0f : m::kNavBarHeight;
}
float MainWindow::TransportHeight() const {
    return m_win7Art ? wt::kBandHeight : m::kTransportHeight;
}

void MainWindow::SetVolumeFromX(int x) {
    const RECT& r = m_layout.volume;
    float x0, x1;
    if (m_win7Art) { // from the slider's own place: it may stand in the flyout
        x0 = r.left + Px(wt::kThumbMin - wt::kVolumeRect.left);
        x1 = r.left + Px(wt::kThumbMax - wt::kVolumeRect.left);
    } else {
        const float thumb = Px(11.0f);
        x0 = r.left + thumb / 2;
        x1 = r.right - thumb / 2;
    }
    const float v = std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    m_volume = v;
    for (int id : {kVolume, kMute})
        if (auto* w = WidgetById(id)) w->Invalidate();
    playback_control::get()->set_volume(DbFromSlider(v));
    KillTimer(m_hwnd, kVolumeTipTimer); // dragging shows at once, no hover delay
    m_volumeHover = true;
    ShowVolumeTip(VolumeThumbX());
}

float MainWindow::VolumeThumbX() const {
    float x0, x1;
    if (m_win7Art) {
        x0 = m_layout.volume.left + Px(wt::kThumbMin - wt::kVolumeRect.left);
        x1 = m_layout.volume.left + Px(wt::kThumbMax - wt::kVolumeRect.left);
    } else {
        const float thumb = Px(11.0f);
        x0 = m_layout.volume.left + thumb / 2;
        x1 = m_layout.volume.right - thumb / 2;
    }
    return x0 + m_volume * std::max(1.0f, x1 - x0);
}

void MainWindow::ShowVolumeTip(float thumbX) {
    const std::wstring text = std::to_wstring(static_cast<int>(std::lround(m_volume * 100))) + L"%";
    ShowTip(text, thumbX, static_cast<float>(m_layout.volume.top - PxI(2)), true);
}

// Windows 7's tooltip: the volume while it is dragged or hovered, and text cut short (a detail
// value) after the pointer rests on it. One bubble for the window; long text wraps.
// `above`: centred over (anchorX, anchorY), else its left edge at anchorX below anchorY.
void MainWindow::ShowTip(const std::wstring& text, float anchorX, float anchorY, bool above) {
    // Win7 tooltip: white to #E4E5F0, #767676 border, small radius, soft shadow, grey text.
    auto& host = gfx::CompositionHost::Get();
    if (!m_volumeTip) {
        m_volumeTip = host.Compositor().CreateSpriteVisual();
        m_volumeTip.Opacity(0.0f);
        m_windowRoot.Children().InsertAtTop(m_volumeTip); // above the clipped content and frame
    }
    constexpr float kMaxTextW = 420;
    auto measure = MakeWrappedText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kMaxTextW, 2000);
    DWRITE_TEXT_METRICS tm{};
    measure->GetMetrics(&tm);
    const float textW = std::ceil(tm.widthIncludingTrailingWhitespace), textH = std::ceil(tm.height);
    const float shadow = 3, w = textW + 14, h = std::max(21.0f, textH + 5);
    auto layout = MakeWrappedText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW + 1, textH + 1);
    const int pw = PxI(w + shadow), ph = PxI(h + shadow);
    host.Repaint(m_volumeTip, pw, ph, m_scale, [&](ID2D1DeviceContext* dc) {
        const float px = 1.0f / m_scale;
        for (int i = 3; i >= 1; --i) {
            com_ptr<ID2D1SolidColorBrush> s;
            dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.06f), s.put());
            dc->FillRoundedRectangle(
                {{static_cast<float>(i), static_cast<float>(i), w - 1 + i, h - 1 + i}, 3, 3}, s.get());
        }
        com_ptr<ID2D1SolidColorBrush> border, ink;
        dc->CreateSolidColorBrush(Hex(0x767676), border.put());
        dc->CreateSolidColorBrush(Hex(0x575757), ink.put());
        // Win7's own tooltip (aero.msstyles Tooltip part 1, image 881, sizing margins 4).
        if (!win7::DrawNineGrid(dc, win7::Module::Aero, 881, {0, 0, 9, 22}, {4, 4, 4, 4}, {0, 0, w, h})) {
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = {{0.0f, Hex(0xFFFFFF)}, {1.0f, Hex(0xE4E5F0)}};
            dc->CreateGradientStopCollection(gs, 2, stops.put());
            com_ptr<ID2D1LinearGradientBrush> fill;
            dc->CreateLinearGradientBrush({{0, 0}, {0, h}}, stops.get(), fill.put());
            const D2D1_ROUNDED_RECT box{{px / 2, px / 2, w - px / 2, h - px / 2}, 2.5f, 2.5f};
            dc->FillRoundedRectangle(box, fill.get());
            dc->DrawRoundedRectangle(box, border.get(), px);
        }
        dc->DrawTextLayout({7, std::round((h - textH) / 2)}, layout.get(), ink.get());
    });
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const float left = std::clamp(above ? anchorX - Px(w) / 2 : anchorX, 0.0f,
                                  static_cast<float>(std::max<LONG>(0, client.right - pw)));
    const float top = std::clamp(above ? anchorY - ph : anchorY, 0.0f,
                                 static_cast<float>(std::max<LONG>(0, client.bottom - ph)));
    m_volumeTip.Offset({std::round(left), std::round(top), 0});
    m_volumeTip.Size({static_cast<float>(pw), static_cast<float>(ph)});
    if (!m_volumeTipShown) {
        m_volumeTipShown = true;
        m_volumeTip.Opacity(0.0f);
        // Explicit 0 -> 1: AnimateOpacity starts from the current value, which can already
        // be 1 when the bubble is shown again before the fade-out finished.
        auto fade = host.Compositor().CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f);
        fade.Duration(std::chrono::milliseconds(180));
        m_volumeTip.StartAnimation(L"Opacity", fade);
    }
}

void MainWindow::HideVolumeTip() {
    KillTimer(m_hwnd, kVolumeTipTimer);
    HideTip();
}

void MainWindow::HideTip() {
    if (!m_volumeTipShown) return;
    m_volumeTipShown = false;
    gfx::CompositionHost::Get().AnimateOpacity(m_volumeTip, 0.0f, 300);
}

// A detail value cut short shows in full in the tooltip once the pointer rests on it for the
// system's tooltip delay (the double-click time, as Windows' own tooltips).
// The list's own text cut short (a tile's title, a cell) shows the same way.
void MainWindow::UpdateDetailTip(POINT pt) {
    std::wstring text;
    RECT rect{};
    if (m_detail.visible)
        for (const auto& cut : m_detailCut)
            if (PtInRect(&cut.rect, pt)) {
                text = cut.text;
                rect = cut.rect;
            }
    bool above = false;
    for (const auto* cuts : {&m_npCut, &m_lyricCut, &m_streamCut})
        for (const auto& cut : *cuts)
            if (text.empty() && PtInRect(&cut.rect, pt)) {
                text = cut.text;
                rect = cut.rect;
                above = true; // the control bar is at the window's bottom
            }
    if (text.empty() && m_list && m_list->HitTest(pt)) m_list->CutTextAt(pt, &text, &rect);
    if (text == m_tipText && EqualRect(&rect, &m_tipRect)) return;
    m_tipText = std::move(text);
    m_tipRect = rect;
    m_tipAbove = above;
    KillTimer(m_hwnd, kDetailTipTimer);
    if (!m_volumeHover) HideTip();
    if (!m_tipText.empty()) {
        m_detailTipAt = pt;
        SetTimer(m_hwnd, kDetailTipTimer, GetDoubleClickTime(), nullptr);
    }
}

float MainWindow::SeekFractionAt(int x) const {
    const RECT& r = m_layout.seek;
    return std::clamp(static_cast<float>(x - r.left) / std::max<LONG>(r.right - r.left, 1), 0.0f, 1.0f);
}

// ---- Hit testing and interaction -----------------------------------------------------

Widget* MainWindow::WidgetAt(POINT pt) const {
    for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it) // later widgets are on top
        if ((*it)->HitTest(pt)) return it->get();
    return nullptr;
}

Widget* MainWindow::WidgetById(int id) const {
    for (auto& w : m_widgets)
        if (w->Id() == id) return w.get();
    return nullptr;
}

LRESULT MainWindow::HitTest(POINT pt) const {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const LONG W = rc.right, H = rc.bottom;
    const auto& L = m_layout;

    if (!IsWindowMaximized()) {
        const LONG b = L.border, corner = L.border * 2;
        const bool left = pt.x < b, right = pt.x >= W - b, top = pt.y < b, bottom = pt.y >= H - b;
        const bool nearL = pt.x < corner, nearR = pt.x >= W - corner, nearT = pt.y < corner,
                   nearB = pt.y >= H - corner;
        if ((top && nearL) || (left && nearT)) return HTTOPLEFT;
        if ((top && nearR) || (right && nearT)) return HTTOPRIGHT;
        if ((bottom && nearL) || (left && nearB)) return HTBOTTOMLEFT;
        if ((bottom && nearR) || (right && nearB)) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
    }
    // Real caption-button hit codes: Windows 11 shows Snap Layouts over HTMAXBUTTON.
    if (PtInRect(&L.capClose, pt)) return HTCLOSE;
    if (PtInRect(&L.capMax, pt)) return HTMAXBUTTON;
    if (PtInRect(&L.capMin, pt)) return HTMINBUTTON;
    if (!IsWindowMaximized() && pt.y < PxI(4)) return HTTOP;
    if (PtInRect(&L.icon, pt)) return HTSYSMENU;
    if (pt.y < L.client.top) return HTCAPTION;
    return HTCLIENT;
}

void MainWindow::SetHoverWidget(Widget* w) {
    if (w == m_hover) return;
    if (m_hover) m_hover->SetHover(false);
    m_hover = w;
    if (m_hover) m_hover->SetHover(true);
    UpdateSplitPlate();
}

bool MainWindow::IsSplit(const Widget* w) const {
    return w && (w->Id() == kNewPlaylist || w->Id() == kViewOptions);
}

void MainWindow::UpdateSplitPart(POINT pt) {
    // Which half of the hovered split button the mouse is over; the plates repaint on change.
    Widget* w = m_pressed ? m_pressed : m_hover;
    if (!IsSplit(w)) return;
    const float split = w->Id() == kNewPlaylist ? m_newPlaylistSplit : kViewOptionsSplit;
    const int part = pt.x - w->Bounds().left >= PxI(split) ? 1 : 0;
    if (part == m_splitPart || m_pressed) return; // keep the pressed half while held
    m_splitPart = part;
    w->Invalidate();
}

void MainWindow::RunSplitMenu(int id, const std::function<void()>& menu) {
    // While the menu is up the arrow half looks pressed and the main half grey (WMP 12).
    Widget* w = WidgetById(id);
    m_splitMenu = id;
    if (w) {
        w->Invalidate();
        w->SetPressed(true);
    }
    menu();
    m_splitMenu = 0;
    if (w) {
        w->SetPressed(false);
        w->Invalidate();
    }
    SetHoverWidget(nullptr); // the menu loop swallowed the mouse leave
}
void MainWindow::OnActivate(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (!active) SetVolumeFlyout(false);
    if (!active) HideSearchPopup();
    if (m_glass) m_glass->SetActive(active);
    if (m_pane) m_pane->SetActive(active);
    for (auto& w : m_widgets)
        if (IsCaptionWidget(w->Id())) w->SetInactive(!active);
    if (m_title) gfx::CompositionHost::Get().AnimateOpacity(m_title, active ? 1.0f : 0.85f, 200);
    m_shadow.SetActive(active);
    PaintSideHighlights();
}

void MainWindow::PaintSideHighlights() {
    const SIZE size = m_sideHighlightSize;
    if (!m_aeroArt || size.cx <= 0 || size.cy <= 0 || !m_sideHighlight[0]) return;
    auto& host = gfx::CompositionHost::Get();
    const D2D1_SIZE_F dip{size.cx / m_scale, size.cy / m_scale};
    for (auto& v : m_sideHighlight)
        host.Repaint(v, size.cx, size.cy, m_scale,
                     [&](ID2D1DeviceContext* dc) { aero::atlas::SideHighlight(dc, m_active, dip); });
}

// ---- Window procedure ----------------------------------------------------------------

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = m_hwnd;
    if (m_taskbar && m_taskbar->OnMessage(msg, wp, lp)) return 0;
    switch (msg) {
    case WM_CREATE: {
        // user_interface::init precedes initquit::on_init. Initialize this UI's queue
        // service before any widget/startup callback queries or changes its mode.
        media::PlaybackScheduler::Start();
        m_scheduleListener =
            media::PlaybackScheduler::Get().AddListener([this, alive = std::weak_ptr<int>(m_alive)] {
                if (alive.expired()) return;
                if (auto* scheduler = media::PlaybackScheduler::Existing())
                    UpdateShuffleRepeat(scheduler->QueueShuffleEnabled(),
                                        static_cast<Repeat>(scheduler->QueueRepeatMode()));
                SchedulePaneQueueRefresh();
            });
        m_scale = GetDpiForWindow(hwnd) / 96.0f;
        InitComposition();
        CreateWidgets();
        RepaintScaleDependent();
        m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] { OnDeviceReplaced(); });
        m_shadow.Create(hwnd);
        media::EnsureSpecialPlaylists(false); // queue, Favorites (protected), smart playlists
        if (m_nav) m_nav->Rebuild();
        fb2k::inMainThread([this] { // once start-up is done
            // Keep lang/strings-en.txt current for scripts/translate-deepseek.ps1: our strings
            // and foobar2000's main menu plus a song's context menu.
            metadb_handle_list sample;
            if (!m_library.Tracks().empty()) sample.add_item(m_library.Tracks().front().handle);
            ExportStrings(sample);
        });
        m_taskbar = std::make_unique<Taskbar>(hwnd);
        m_taskbar->onFavorite = [this] {
            metadb_handle_ptr now;
            if (playback_control::get()->get_now_playing(now)) {
                auto* provider = stream::ProviderOf(now);
                const bool next = !media::IsFavorite(now);
                if (!provider || provider->CanSetFavorite(next)) media::ToggleFavorite(now);
            }
            UpdateFavoriteState();
        };
        Taskbar::SetupJumpList();
        m_library.Reload();
        m_playCallback = std::make_unique<PlayCallback>(*this);
        m_playlistCallback = std::make_unique<PlaylistCallback>(*this);
        m_libraryCallback = std::make_unique<LibraryCallback>(*this);
        m_artNotify = now_playing_album_art_notify_manager::get()->add(
            [this, alive = std::weak_ptr<int>(m_alive)](album_art_data::ptr) {
                fb2k::inMainThread([this, alive] {
                    if (!alive.expired()) RefreshNowPlayingArt();
                });
            });
        m_dropTarget = MakeFileDropTarget([this](IDataObject* data) { AddDroppedFiles(data); });
        RegisterDragDrop(hwnd, m_dropTarget.get());

        // Start on the page that was open when the window closed; otherwise where
        // foobar2000 is (the active playlist, else the music library).
        View start;
        size_t startTop = SIZE_MAX;
        if (!LoadLastView(start, startTop)) {
            const t_size active = playlist_manager::get()->get_active_playlist();
            if (active != SIZE_MAX) {
                start.library = View::Library::All;
                start.kind = View::Kind::Playlist;
                start.playlist = active;
            }
        } else if (startTop != SIZE_MAX) {
            m_scrollPositions[ViewKey(start)] = {start.IsRemote() ? start.provider : stream::Id{}, startTop};
        }
        Navigate(start, true);

        OnPlaybackStateChanged();
        OnTrackChanged();
        OnVolumeChanged();
        m_shuffle = cfg_shuffle.get() != 0;
        m_repeat = static_cast<Repeat>(std::clamp<int>(static_cast<int>(cfg_repeat.get()), 0, 2));
        SetShuffleRepeat(m_shuffle, m_repeat);
        for (auto* provider : stream::Providers()) // the navigation lists and saved items
            if (provider->AccountVerified()) provider->RefreshLibrary();
        try { // output device changed elsewhere (preferences)
            m_outputCallback = output_manager_v2::get()->addCallback([this] {
                RefreshDeviceName();
                RepaintStreamInfo();
            });
        } catch (...) {
        }
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }

    case settings::kChangedMessage: // Preferences > Display > Glasswave applied something
        ApplyLanguage(CurrentLanguageSetting());
        ApplyTint(true);
        if (m_pane) m_pane->RestyleLyrics(); // sizes, fonts, where translations stand
        if (std::wstring signature = LyricsSettingsSignature(); signature != m_lyricsSettingsSignature) {
            m_lyricsSettingsSignature = std::move(signature);
            if (m_lyricsTrack.is_valid()) RequestLyrics(m_lyricsTrack, false);
        }
        if (m_view.kind == View::Kind::AllRecentPlayed || m_view.kind == View::Kind::AllMostPlayed)
            Navigate(m_view, false);
        else if (m_list)
            m_list->Refresh(); // rows drawn with the settings (song tags before the artist)
        RefreshPaneQueue();
        return 0;

    case kFrameMessage:
        if (m_frameClock) {
            m_frameClock->Ack();
            if (!PlaybackPaintAllowed()) {
                m_playbackPresentationSuspended = true;
                m_frameClock->Stop();
                return 0;
            }
            // A pane lent to the player runs on the player's frames.
            bool more = !m_paneLent && m_pane && m_pane->Tick();
            more |= TickNowPlayingLyric();
            more |= TickSeek();
            if (!more) m_frameClock->Stop();
        }
        return 0;

    case WM_TIMER:
        if (wp == kWindowOpenTimer) {
            KillTimer(hwnd, kWindowOpenTimer);
            m_shadow.SetSuppressed(false);
            m_shadow.Sync();
            return 0;
        }
        if (wp == kWindowCloseTimer) {
            KillTimer(hwnd, kWindowCloseTimer);
            standard_commands::main_exit();
            return 0;
        }
        if (wp == kArtHoldTimer) { // the waiting song shows now, its cover still to come
            ReleaseHeldTrack(true);
            return 0;
        }
        if (wp == kPaneRevealTimer) {
            StepPaneReveal();
            return 0;
        }
        if (wp == kLibraryTimer) {
            KillTimer(hwnd, kLibraryTimer);
            m_library.Reload();
            // Local pages show the new snapshot; provider pages do not depend on it.
            if (m_view.kind != View::Kind::Playlist && !m_view.IsRemote()) Navigate(m_view, false);
            return 0;
        }
        if (wp == kVolumeTipTimer) {
            KillTimer(hwnd, kVolumeTipTimer);
            if (m_volumeHover) ShowVolumeTip(VolumeThumbX());
            return 0;
        }
        if (wp == kDetailTipTimer) {
            KillTimer(hwnd, kDetailTipTimer);
            // Below the pointer, clear of the cursor, as Windows places its tooltips.
            // In the control bar, above the text instead (the window's bottom edge is near).
            if (!m_tipText.empty())
                ShowTip(m_tipText, static_cast<float>(m_detailTipAt.x),
                        static_cast<float>(m_tipAbove ? m_tipRect.top - PxI(2) : m_detailTipAt.y + PxI(20)),
                        m_tipAbove);
            return 0;
        }
        if (wp == kSuggestTimer) {
            KillTimer(hwnd, kSuggestTimer);
            FetchSuggestions();
            return 0;
        }
        if (wp == kScrollAnimTimer) {
            bool active = m_list && m_list->AnimateScrollBar();
            active |= AnimateDetailScrollBar();
            active |= m_crumbBar->Animate();
            active |= AnimateTabs();
            if (m_search) active |= m_search->Animate();
            if (!active) {
                KillTimer(hwnd, kScrollAnimTimer);
                m_scrollAnimActive = false;
            }
            return 0;
        }
        if (wp == kScrollRepeatTimer) {
            if (GetTickCount64() - m_scrollRepeatSince < 350) return 0;
            bool active = m_list && m_list->OnScrollRepeat();
            active |= OnDetailBriefRepeat();
            if (!active) {
                KillTimer(hwnd, kScrollRepeatTimer);
                m_scrollRepeatActive = false;
            }
            return 0;
        }
        break;

    case WM_NCCALCSIZE:
        if (wp) {
            // The whole window is client area. A maximized window is pushed out by the
            // frame thickness on every side, so pull the client back onto the monitor.
            if (IsWindowMaximized()) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const UINT dpi = GetDpiForWindow(hwnd);
                const int fx =
                    GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy =
                    GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                InflateRect(&p->rgrc[0], -fx, -fy);
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        return HitTest(pt);
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (m_search && m_search->HitTest(pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            if (m_list && m_list->HitTest(pt))
                if (LPCWSTR cursor = m_list->CursorAt(pt)) { // column edges, album links
                    SetCursor(LoadCursorW(nullptr, cursor));
                    return TRUE;
                }
            if (NowPlayingPartAt(pt)) { // now-playing links, output device
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_NCACTIVATE:
        OnActivate(wp != FALSE);
        return DefWindowProcW(hwnd, msg, wp, -1); // -1: do not repaint the (non-existent) frame

    case WM_SHOWWINDOW: {
        const LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
        // The library shown from elsewhere (tray, another instance): the player gives way.
        if (wp && m_player && m_player->Shown()) m_player->Close();
        UpdatePlaybackPresentation();
        return result;
    }

    case WM_SIZE: {
        HideSearchPopup();
        if (wp == SIZE_MINIMIZED) {
            m_playbackPresentationSuspended = true;
            if (m_frameClock) m_frameClock->Stop();
        }
        if (m_list) m_list->SetMotion(wp != SIZE_MINIMIZED); // motion covers rest while minimized
        const bool wasMax = m_layout.border == 0;
        UpdateLayout();
        if (wp != SIZE_MINIMIZED) UpdatePlaybackPresentation();
        if (wasMax != (m_layout.border == 0))
            if (auto* w = WidgetById(kCapMax)) w->Invalidate();
        return 0;
    }

    case WM_MOVE:
        HideSearchPopup();
        if (m_glass) m_glass->UpdateReflectionAnchor(hwnd);
        return 0;

    case WM_WINDOWPOSCHANGING: {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (m_glass && !(pos->flags & SWP_NOMOVE) && !IsIconic(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const LONG w = (pos->flags & SWP_NOSIZE) ? wr.right - wr.left : pos->cx;
            const LONG h = (pos->flags & SWP_NOSIZE) ? wr.bottom - wr.top : pos->cy;
            m_glass->UpdateReflectionAnchor(hwnd, RECT{pos->x, pos->y, pos->x + w, pos->y + h});
        }
        break;
    }

    case WM_WINDOWPOSCHANGED: {
        const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp); // generates WM_SIZE / WM_MOVE
        UpdatePlaybackPresentation();
        m_shadow.Sync();
        return r;
    }

    case WM_DPICHANGED: {
        m_scale = HIWORD(wp) / 96.0f;
        const auto* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RepaintScaleDependent();
        UpdateLayout();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        // Narrow, but with nothing overlapping: the transport's narrowest arrangement (song text
        // 110, time 110, the capsule without shuffle, repeat, stop and slider 212, the lyrics
        // toggle, 12 DIP gaps) needs a 516 DIP client; the command bar and the navigation pane
        // give way before (UpdateLayout).
        mmi->ptMinTrackSize = {PxI(540 + (m_paneReveal >= 0 ? m_paneReveal / m_scale
                                                          : m_lyricsOpen ? kLyricsPaneWidth : 0.0f)),
                               PxI(420)};
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (m_glass) m_glass->RefreshTransparencyPolicy();
        break;

    case WM_DEVICECHANGE:
        // A disc came or went: the navigation pane shows the drives' CDs (cd/discs.h), and a
        // disc taken out leaves the queue (cd/playback.h).
        if ((wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) && lp &&
            reinterpret_cast<const DEV_BROADCAST_HDR*>(lp)->dbch_devicetype == DBT_DEVTYP_VOLUME &&
            (reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp)->dbcv_flags & DBTF_MEDIA)) {
            cd::OnMediaChanged(reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp)->dbcv_unitmask,
                               wp == DBT_DEVICEARRIVAL);
            if (wp == DBT_DEVICEREMOVECOMPLETE)
                cd::DropEjectedDiscs([] {
                    auto* window = Instance();
                    if (!window) return;
                    window->OnCoreQueueChanged(); // history rows
                    // The disc's songs leave Recently Played and Most Played (media::PlayStats).
                    if (window->m_view.kind == View::Kind::AllRecentPlayed ||
                        window->m_view.kind == View::Kind::AllMostPlayed)
                        window->Navigate(window->m_view, false);
                });
        }
        break;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;

    // -- Caption buttons (non-client messages) --
    case WM_NCMOUSEMOVE: {
        Widget* w = nullptr;
        if (wp == HTMINBUTTON)
            w = WidgetById(kCapMin);
        else if (wp == HTMAXBUTTON)
            w = WidgetById(kCapMax);
        else if (wp == HTCLOSE)
            w = WidgetById(kCapClose);
        if (!m_pressed) SetHoverWidget(w);
        if (!m_trackingNc) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            m_trackingNc = TrackMouseEvent(&tme) != FALSE;
        }
        break;
    }
    case WM_NCMOUSELEAVE:
        m_trackingNc = false;
        if (m_hover && IsCaptionWidget(m_hover->Id()) && !m_pressed) SetHoverWidget(nullptr);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            const int id = wp == HTMINBUTTON ? kCapMin : wp == HTMAXBUTTON ? kCapMax : kCapClose;
            m_ncDownId = id;
            m_pressed = WidgetById(id);
            m_pressed->SetPressed(true);
            m_capture = Capture::Widget;
            SetCapture(hwnd);
            return 0; // keep DefWindowProc from drawing classic buttons
        }
        break;
    case WM_NCLBUTTONUP:
        // Windows 11 may take the capture away over the maximize button (Snap Layouts);
        // the button-up then arrives here. Down and up on the same button is a click.
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            const int id = wp == HTMINBUTTON ? kCapMin : wp == HTMAXBUTTON ? kCapMax : kCapClose;
            const bool same = (id == m_ncDownId);
            m_ncDownId = 0;
            if (m_pressed) {
                m_pressed->SetPressed(false);
                m_pressed = nullptr;
                m_capture = Capture::None;
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            if (Widget* w = WidgetById(id); same && w && w->onClick) w->onClick();
            return 0;
        }
        break;

    // -- Client-area mouse --
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (m_capture == Capture::Lyrics) {
            m_pane->OnMouseMove(pt);
            return 0;
        }
        if (m_pane) {
            const bool over = m_capture == Capture::None && m_pane->HitTest(pt);
            if (over != m_lyricsHover) {
                m_lyricsHover = over;
                if (!over) m_pane->OnMouseLeave();
            }
            if (over) m_pane->OnMouseMove(pt);
        }
        if (m_capture == Capture::DetailScroll) {
            OnDetailBriefBarMove(pt);
            return 0;
        }
        if (m_capture == Capture::Search) {
            m_search->OnMouseDrag(pt);
            return 0;
        }
        if (m_capture == Capture::Nav) {
            m_nav->OnMouseMove(pt);
            return 0;
        }
        if (m_capture == Capture::List) {
            m_list->OnMouseMove(pt);
        } else if (m_pressed) {
            if (m_pressed->Id() == kVolume) {
                SetVolumeFromX(pt.x);
            } else if (m_pressed->Id() == kSeek) {
                m_seekPreview = SeekFractionAt(pt.x);
                m_pressed->Invalidate();
            } else {
                m_pressed->SetPressed(m_pressed->HitTest(pt));
            }
        } else {
            SetHoverWidget(WidgetAt(pt));
            UpdateSplitPart(pt);
            m_list->OnMouseMove(pt);
            m_nav->OnMouseMove(pt);
            m_crumbBar->OnMouseMove(pt);
            UpdateTabHover(pt);
            m_search->SetClearHot(m_search->HitClear(pt));
            if (m_search->Animating()) StartScrollAnimation();
            UpdateNowPlayingHover(pt);
            UpdateDetailBriefHover(pt);
            UpdateDetailTip(pt);
            const bool overVolume = PtInRect(&m_layout.volume, pt) != FALSE;
            if (overVolume) {
                if (!m_volumeHover) {
                    m_volumeHover = true;
                    SetTimer(hwnd, kVolumeTipTimer, static_cast<UINT>(settings::VolumeTipDelayMs()), nullptr);
                }
            } else if (m_volumeHover) {
                m_volumeHover = false;
                HideVolumeTip();
            }
        }
        if (!m_trackingClient) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            m_trackingClient = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_trackingClient = false;
        if (m_lyricsHover && m_capture != Capture::Lyrics) {
            m_lyricsHover = false;
            m_pane->OnMouseLeave();
        }
        if (m_detailBriefHot) {
            m_detailBriefHot = 0;
            StartScrollAnimation();
        }
        if (m_hover && !IsCaptionWidget(m_hover->Id()) && !m_pressed) SetHoverWidget(nullptr);
        if (m_capture == Capture::None) {
            m_list->OnMouseLeave();
            m_nav->OnMouseLeave();
            m_crumbBar->OnMouseLeave();
            UpdateTabHover({-1, -1});
            m_search->SetClearHot(false);
            if (m_search->Animating()) StartScrollAnimation();
            UpdateNowPlayingHover({-1, -1});
            m_volumeHover = false;
            HideVolumeTip();
            UpdateDetailTip({-1, -1});
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool dbl = msg == WM_LBUTTONDBLCLK;
        // A click anywhere but on the volume flyout or its arrow closes the flyout.
        if (m_volFlyoutOpen && !PtInRect(&m_layout.volFlyout, pt) && !PtInRect(&m_layout.volArrow, pt))
            SetVolumeFlyout(false);
        if (m_pane && m_pane->HitTest(pt)) {
            m_pane->OnLButtonDown(pt);
            m_capture = Capture::Lyrics;
            SetCapture(hwnd);
            return 0;
        }
        if (m_detailBriefScrollable && PtInRect(&m_layout.detailBriefBar, pt)) {
            OnDetailBriefBarDown(pt);
            m_capture = Capture::DetailScroll;
            SetCapture(hwnd);
            return 0;
        }
        if (PtInRect(&m_layout.breadcrumb, pt)) {
            m_crumbBar->OnClick(m_hwnd, pt);
            return 0;
        }
        if (PtInRect(&m_layout.tabs, pt)) {
            OnTabClick(pt);
            return 0;
        }
        if (const int part = NowPlayingPartAt(pt)) {
            OnNowPlayingClick(part);
            return 0;
        }
        if (Widget* w = WidgetAt(pt)) {
            m_pressed = w;
            w->SetPressed(true);
            UpdateSplitPlate();
            m_capture = Capture::Widget;
            SetCapture(hwnd);
            if (w->Id() == kVolume) SetVolumeFromX(pt.x);
            if (w->Id() == kSeek && m_length > 0) {
                m_seeking = true;
                m_seekPreview = SeekFractionAt(pt.x);
                w->Invalidate();
            }
            return 0;
        }
        if (m_list->HitTest(pt)) {
            SetSearchFocus(false);
            m_list->OnLButtonDown(pt, KeyDown(VK_CONTROL), KeyDown(VK_SHIFT));
            if (dbl) m_list->OnDoubleClick(pt);
            if (m_list->Capturing()) {
                m_capture = Capture::List;
                SetCapture(hwnd);
            }
            return 0;
        }
        if (m_nav->HitTest(pt)) {
            FocusNav();
            if (m_nav->OnLButtonDown(pt)) {
                m_capture = Capture::Nav;
                SetCapture(hwnd);
            }
            return 0;
        }
        if (m_search->HitClear(pt)) { // held: the pressed plate; it clears when released on it
            m_search->SetClearPressed(true);
            StartScrollAnimation();
            m_capture = Capture::SearchClear;
            SetCapture(hwnd);
            return 0;
        }
        if (m_search->HitTest(pt)) {
            if (!m_search->Focused()) SetSearchFocus(true);
            if (dbl) {
                m_search->OnDoubleClick(pt);
            } else {
                m_search->OnMouseDown(pt, KeyDown(VK_SHIFT));
                m_capture = Capture::Search;
                SetCapture(hwnd);
            }
            return 0;
        }
        SetSearchFocus(false);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        m_ncDownId = 0;
        if (m_capture == Capture::Lyrics) {
            m_capture = Capture::None;
            ReleaseCapture();
            m_pane->OnLButtonUp(pt);
            return 0;
        }
        if (m_capture == Capture::DetailScroll) {
            m_capture = Capture::None;
            ReleaseCapture();
            OnDetailBriefBarUp();
            return 0;
        }
        if (m_capture == Capture::Search) {
            m_capture = Capture::None;
            ReleaseCapture();
            return 0;
        }
        if (m_capture == Capture::SearchClear) {
            m_capture = Capture::None;
            ReleaseCapture();
            m_search->SetClearPressed(false);
            StartScrollAnimation();
            if (m_search->HitClear(pt)) OnSearchClear();
            return 0;
        }
        if (m_capture == Capture::Nav) {
            m_capture = Capture::None;
            ReleaseCapture();
            if (const NavPane::Item* item = m_nav->OnLButtonUp(pt)) {
                const NavPane::Item copy = *item;
                OnNavItem(copy);
            }
            return 0;
        }
        if (m_capture == Capture::List) {
            m_capture = Capture::None;
            ReleaseCapture();
            m_list->OnLButtonUp(pt);
            return 0;
        }
        if (Widget* w = m_pressed) {
            // ReleaseCapture() synchronously sends WM_CAPTURECHANGED, which resets the drag
            // state; take what we need first.
            const bool seek = w->Id() == kSeek && m_seeking;
            const float preview = m_seekPreview;
            m_pressed = nullptr;
            m_capture = Capture::None;
            ReleaseCapture();
            w->SetPressed(false);
            UpdateSplitPlate();
            if (w->Id() == kVolume) {
                m_volumeHover = PtInRect(&m_layout.volume, pt) != FALSE;
                if (m_volumeHover)
                    ShowVolumeTip(VolumeThumbX());
                else
                    HideVolumeTip();
            }
            if (w->Id() == kSeek) {
                if (seek && m_length > 0) playback_control::get()->playback_seek(preview * m_length);
                m_seeking = false;
                w->Invalidate();
                return 0;
            }
            const bool inside = w->HitTest(pt);
            if (!inside || IsCaptionWidget(w->Id())) SetHoverWidget(nullptr);
            if (inside && w->onClick && w->IsEnabled()) w->onClick();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != hwnd) {
            m_volumeHover = false;
            HideVolumeTip();
            if (m_pressed) {
                m_pressed->SetPressed(false);
                m_pressed = nullptr;
                UpdateSplitPlate();
            }
            if (m_capture == Capture::List) m_list->CancelDrag();
            if (m_capture == Capture::Nav) m_nav->CancelDrag();
            if (m_capture == Capture::Lyrics && m_pane) m_pane->OnCaptureLost();
            if (m_capture == Capture::DetailScroll) {
                m_detailBriefPressed = 0;
                m_detailBriefTrackDir = 0;
                StartScrollAnimation();
            }
            m_capture = Capture::None;
            m_seeking = false;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (m_pane && m_pane->HitTest(pt)) {
            m_pane->OnWheel(delta);
            return 0;
        }
        if (m_detailBriefScrollable &&
            (PtInRect(&m_layout.detailBriefArea, pt) || PtInRect(&m_layout.detailBriefBar, pt))) {
            ScrollDetailBrief(-delta / static_cast<float>(WHEEL_DELTA) * 54.0f);
            return 0;
        }
        if (m_list->HitTest(pt)) {
            m_list->OnWheel(delta, (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
            m_list->OnMouseMove(pt);
            return 0;
        }
        if (m_nav->HitTest(pt)) {
            m_nav->OnWheel(delta);
            return 0;
        }
        if (PtInRect(&m_layout.volume, pt)) { // the volume slider itself, not the whole band
            auto pc = playback_control::get();
            if (delta > 0)
                pc->volume_up();
            else
                pc->volume_down();
            return 0;
        }
        break;
    }
    case WM_MOUSEHWHEEL: { // tilt wheel / touchpad sideways: the list scrolls horizontally
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (m_list->HitTest(pt)) {
            m_list->OnWheel(-GET_WHEEL_DELTA_WPARAM(wp), true);
            return TRUE;
        }
        break;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        // The playing song's cover and title: the song's own menu, as on its row in a list.
        if (PtInRect(&m_npArtRect, pt) || PtInRect(&m_npTitleRect, pt)) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            ShowNowPlayingSongMenu(screen);
            return 0;
        }
        // The song's lines and the lyric line under them (the menu turns a hidden line back on).
        const RECT lyricRect{m_layout.nowPlaying.left + PxI(m_npTextX), m_layout.nowPlaying.top,
                             m_layout.nowPlaying.right, m_layout.nowPlaying.bottom};
        if (PtInRect(&lyricRect, pt)) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            ShowLyricsMenu(screen);
            return 0;
        }
        if (m_pane && m_pane->HitTest(pt)) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            m_pane->OnRButtonUp(pt, screen);
            return 0;
        }
        if (m_list->HitTest(pt)) {
            m_list->OnRButtonUp(hwnd, pt);
            return 0;
        }
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (m_nav->HitTest(pt)) {
            const NavPane::Item* item = m_nav->ItemAt(pt);
            if (item && (item->target == NavPane::Target::LibraryLocal ||
                         item->target == NavPane::Target::Category)) {
                ShowNavCustomizeMenu(screen);
                return 0;
            }
            if (item) {
                switch (item->target) {
                case NavPane::Target::LibraryRemote:
                case NavPane::Target::RemoteSection:
                case NavPane::Target::RemotePlaylist:
                    ShowRemoteNavMenu(*item, screen);
                    return 0;
                case NavPane::Target::LibraryAll:
                case NavPane::Target::AllFavorites:
                case NavPane::Target::AllRecentPlayed:
                case NavPane::Target::AllMostPlayed:
                case NavPane::Target::AllDownloads:
                case NavPane::Target::Home:
                    // Page actions live on the list's own empty area; nothing applies to the row.
                    return 0;
                case NavPane::Target::AudioCd:
                    ShowDiscMenu(static_cast<wchar_t>(item->playlist), screen);
                    return 0;
                case NavPane::Target::DiscsHeader:
                    return 0;
                default:
                    break;
                }
            }
            const bool playlistMenu =
                item && (item->target == NavPane::Target::Playlist || item->target == NavPane::Target::Queue);
            ShowPlaylistMenu(playlistMenu ? item->playlist : SIZE_MAX, screen);
            return 0;
        }
        if (PtInRect(&m_layout.detailHeader, pt) && m_detail.visible) {
            ShowPageContextMenu(screen);
            return 0;
        }
        if (pt.y >= m_layout.client.top) {
            ShowMainMenu(hwnd, screen);
            return 0;
        }
        break;
    }

    // -- Keyboard --
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT);
        if (msg == WM_KEYDOWN) {
            if ((ctrl && wp == 'E') || wp == VK_F3) {
                SetSearchFocus(true);
                return 0;
            }
            if (m_search->Focused()) {
                if (OnSearchKey(wp)) return 0;
                if (m_search->OnKeyDown(static_cast<UINT>(wp), ctrl, hwnd)) return 0;
                if (wp == VK_DOWN || wp == VK_TAB) {
                    SetSearchFocus(false);
                    m_list->OnKeyDown(VK_HOME, false, false);
                    return 0;
                }
            }
            if (wp == VK_F11) {
                ShowPlayer(true);
                return 0;
            }
            if (ctrl && shift && wp == 'M') { // WMP 12: switch to the mini player
                ShowPlayer(false);
                return 0;
            }
            if (wp == VK_SPACE && !m_search->Focused()) { // WMP's play / pause
                media::transport::PlayPause();
                return 0;
            }
            if (wp == VK_BACK && !m_search->Focused()) {
                GoBack();
                return 0;
            }
            if (wp == VK_F5 && !m_search->Focused()) {
                RefreshCurrentPage();
                return 0;
            }
            if (m_list->OnKeyDown(static_cast<UINT>(wp), ctrl, shift)) return 0;
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_LEFT) {
            GoBack();
            return 0;
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_RIGHT) {
            GoForward();
            return 0;
        }
        if (keyboard_shortcut_manager::get()->on_keydown_auto(wp)) return 0;
        break;
    }
    case WM_CHAR:
        if (m_search->Focused() && m_search->OnChar(static_cast<wchar_t>(wp), hwnd)) return 0;
        break;
    // The search field draws the IME composition itself (the IME's own composition window
    // floated over the box) and keeps the candidate list just under the box.
    case WM_IME_SETCONTEXT:
        if (wp && m_search->Focused()) lp &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
        break;
    case WM_IME_STARTCOMPOSITION:
        if (m_search->Focused()) {
            PlaceImeWindows();
            return 0;
        }
        break;
    case WM_IME_COMPOSITION:
        if (m_search->Focused()) {
            if (HIMC imc = ImmGetContext(hwnd)) {
                auto read = [imc](DWORD kind) {
                    const LONG bytes = ImmGetCompositionStringW(imc, kind, nullptr, 0);
                    std::wstring text(bytes > 0 ? static_cast<size_t>(bytes) / sizeof(wchar_t) : 0, L'\0');
                    if (!text.empty())
                        ImmGetCompositionStringW(imc, kind, text.data(), static_cast<DWORD>(bytes));
                    return text;
                };
                if (lp & GCS_RESULTSTR) m_search->InsertText(read(GCS_RESULTSTR));
                if (lp & GCS_COMPSTR) {
                    const LONG cursor =
                        (lp & GCS_CURSORPOS) ? ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0) : -1;
                    std::wstring text = read(GCS_COMPSTR);
                    const size_t at = cursor >= 0 ? static_cast<size_t>(cursor) : text.size();
                    m_search->SetComposition(std::move(text), at);
                } else if (lp & GCS_RESULTSTR) {
                    m_search->SetComposition({}, 0);
                }
                ImmReleaseContext(hwnd, imc);
            }
            PlaceImeWindows();
            return 0;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        if (m_search->Focused()) {
            m_search->SetComposition({}, 0);
            return 0;
        }
        break;
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1)
            GoBack();
        else if (GET_XBUTTON_WPARAM(wp) == XBUTTON2)
            GoForward();
        return TRUE;
    case WM_SYSCOMMAND:
        // Alt or F10 alone opens the foobar2000 main menu (WMP 12 also hides its menu bar).
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) {
            POINT pt{m_layout.client.left, m_layout.client.top};
            ClientToScreen(hwnd, &pt);
            ShowMainMenu(hwnd, pt);
            return 0;
        }
        break;

    case WM_CLOSE:
        // Closing the main window exits foobar2000; the core then calls user_interface::shutdown().
        // Visible, it first closes the way Windows 7 closed windows (kWindowCloseTimer).
        if (!m_closing && IsWindowVisible(hwnd) && !IsIconic(hwnd) && WindowAnimations()) {
            m_closing = true;
            AnimateWindow(false);
            return 0;
        }
        standard_commands::main_exit();
        return 0;

    case WM_DESTROY: {
        if (auto* scheduler = media::PlaybackScheduler::Existing())
            scheduler->RemoveListener(std::exchange(m_scheduleListener, 0));
        CloseLyricsSearch();
        WINDOWPLACEMENT wpl{sizeof(wpl)};
        if (GetWindowPlacement(hwnd, &wpl)) cfg_placement.set(wpl);
        SaveLastView();
        KillTimer(hwnd, kLibraryTimer);
        RevokeDragDrop(hwnd);
        m_dropTarget = nullptr;
        if (m_artNotify) {
            now_playing_album_art_notify_manager::get()->remove(m_artNotify);
            m_artNotify = nullptr;
        }
        m_shadow.Destroy();
        m_libraryCallback.reset();
        m_playlistCallback.reset();
        m_playCallback.reset();
        if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_detailArtToken);
        m_detailArtToken = 0;
        media::radio::Unsubscribe(m_radioToken);
        m_radioToken = 0;
        SetContextActionProvider({});
        m_providerListener.reset();
        cd::RemoveDiscsListener(m_discsToken);
        m_discsToken = 0;
        m_alive.reset(); // provider callbacks that outlive the window do nothing
        m_page.reset();
        m_hover = m_pressed = nullptr;
        m_list.reset();
        m_nav.reset();
        media::SetPlayQueueListener({});
        m_searchPopup.reset();
        m_search.reset();
        m_frameClock.reset();
        m_paneLent = false;
        m_playerTaskbar.reset();
        m_player.reset();
        m_pane.reset();
        m_nowPlayingPaint.Clear();
        m_seekPaint.Clear();
        m_nowPlayingBacking.Clear();
        m_timeBacking.Clear();
        m_npLyric = nullptr;
        m_npLyricSurface = nullptr;
        m_nowPlaying = nullptr;
        m_time = nullptr;
        ++m_lyricsRequestGeneration;
        lyrics::Service::Get().Cancel();
        m_source.reset();
        m_widgets.clear();
        m_glass.reset();
        m_outputCallback.release();
        m_root = nullptr;
        m_windowRoot = nullptr;
        m_target = nullptr;
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- PageHost ------------------------------------------------------------------------------

void MainWindow::SetPageName(const std::wstring& name) {
    if (name.empty() || name == m_view.name) return;
    if (m_historyPos < m_history.size() && SameView(m_history[m_historyPos], m_view))
        m_history[m_historyPos].name = name;
    m_view.name = name;
    RepaintBreadcrumb();
}

void MainWindow::ShowSource(std::unique_ptr<media::ListSource> source) {
    if (!m_list || !source) return;
    // One linear pass per snapshot preserves exact duplicate occurrences. Repeated
    // FindRow calls both collapse equal tracks and become quadratic for Select All.
    const bool hadRows = m_source && m_source->Count();
    const size_t topRow = hadRows ? m_list->TopRow() : SIZE_MAX;
    const float topOffset = topRow != SIZE_MAX ? m_list->RowOffset(topRow) : 0.0f;
    // At the top, the new layout starts at its top too: its headers (an artist's band, a
    // group's caption) differ in height, so the first row's offset would leave a sliver.
    const bool atTop = hadRows && m_list->AtTop();
    const size_t oldFocus = hadRows ? m_source->Focus() : SIZE_MAX;
    const size_t oldSort = m_source ? m_source->SortColumn() : SIZE_MAX;
    const bool descending = m_source && m_source->SortDescending();
    std::unordered_set<std::wstring> selected;
    std::wstring anchor, focused;
    const auto identity = [](media::ListSource& list, size_t row,
                             std::unordered_map<std::wstring, size_t>& occurrences) {
        std::wstring base = list.RowKey(row);
        if (base.empty()) base = L"row:" + std::to_wstring(row);
        const size_t ordinal = occurrences[base]++;
        return std::to_wstring(base.size()) + L":" + base + L":" + std::to_wstring(ordinal);
    };
    if (hadRows) {
        std::unordered_map<std::wstring, size_t> occurrences;
        for (size_t row = 0; row < m_source->Count(); ++row) {
            const std::wstring key = identity(*m_source, row, occurrences);
            if (m_source->IsSelected(row)) selected.insert(key);
            if (row == topRow) anchor = key;
            if (row == oldFocus) focused = key;
        }
    }
    m_source = std::move(source);
    if (auto* style = ViewStyleSetting(m_view);
        style && m_source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
        m_source->SetViewStyle(static_cast<media::ViewStyle>(style->get()));
    if (oldSort != SIZE_MAX && m_source->CanSort(oldSort)) m_source->Sort(oldSort, descending);
    m_source->SetFilter(m_filter);
    m_list->SetSource(m_source.get(), false);
    std::vector<size_t> selection;
    size_t anchorRow = SIZE_MAX, focusRow = SIZE_MAX;
    std::unordered_map<std::wstring, size_t> occurrences;
    for (size_t row = 0; row < m_source->Count(); ++row) {
        const std::wstring key = identity(*m_source, row, occurrences);
        if (selected.contains(key)) selection.push_back(row);
        if (key == anchor) anchorRow = row;
        if (key == focused) focusRow = row;
    }
    m_source->SetSelection(selection);
    if (focusRow != SIZE_MAX) m_source->SetFocus(focusRow);
    if (anchorRow == SIZE_MAX && topRow != SIZE_MAX && m_source->Count())
        anchorRow = std::min(topRow, m_source->Count() - 1);
    if (atTop)
        m_list->ScrollToTop();
    else if (anchorRow != SIZE_MAX)
        m_list->ScrollToRow(anchorRow, topOffset);
    m_list->Refresh();
    if (m_page) m_page->OnPresentationChanged();
    if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
    SelectPending();
}
void MainWindow::BeforeListChange() {
    m_pageAnchor.clear();
    m_pageAnchorOrdinal = 0;
    m_pageAnchorRow = m_list ? m_list->TopRow() : SIZE_MAX;
    if (!m_source || m_pageAnchorRow >= m_source->Count()) return;
    m_pageAnchorOffset = m_list->RowOffset(m_pageAnchorRow);
    m_pageAnchor = m_source->RowKey(m_pageAnchorRow);
    if (m_pageAnchor.empty()) return;
    for (size_t row = 0; row < m_pageAnchorRow; ++row)
        if (m_source->RowKey(row) == m_pageAnchor) ++m_pageAnchorOrdinal;
}

void MainWindow::ListChanged() {
    if (m_list) m_list->Refresh();
    if (m_list && m_source && m_pageAnchorRow != SIZE_MAX) {
        size_t ordinal = 0, target = SIZE_MAX;
        for (size_t row = 0; row < m_source->Count(); ++row)
            if (!m_pageAnchor.empty() && m_source->RowKey(row) == m_pageAnchor &&
                ordinal++ == m_pageAnchorOrdinal) {
                target = row;
                break;
            }
        if (target == SIZE_MAX && m_source->Count())
            target = std::min(m_pageAnchorRow, m_source->Count() - 1);
        // Back at the same distance from the top, not at the top: an album's header (or the
        // band) above the row stays in view (owner, 2026-11-14: every load hid the first header).
        if (target != SIZE_MAX) m_list->ScrollToRow(target, m_pageAnchorOffset);
    }
    m_pageAnchor.clear();
    m_pageAnchorRow = SIZE_MAX;
    SelectPending();
}

void MainWindow::ChromeChanged() {
    RefreshProviderSections();
    RepaintBreadcrumb(); // re-applies the layout: the band's height follows its text
    RepaintDetailHeader();
}

media::ViewStyle MainWindow::SavedStyle(media::ViewStyle fallback) const {
    auto* style = ViewStyleSetting(m_view);
    return style ? static_cast<media::ViewStyle>(style->get()) : fallback;
}

} // namespace glasswave::ui
