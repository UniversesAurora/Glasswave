#include "pch.h"
#include "ui/downloads_page.h"
#include "core/text.h"
#include "ui/song_actions.h"
#include "media/favorites.h"
#include "media/special_playlists.h"
#include "ui/i18n.h"
#include "ui/text.h"

#include <set>

namespace glasswave::ui {

using core::Wide;


std::vector<LocalPlaylistTarget> LocalPlaylistTargets(size_t exclude) {
    auto pm = playlist_manager::get();
    std::vector<LocalPlaylistTarget> out;
    const t_size favorites = media::FavoritesPlaylist(false);
    for (t_size p = 0; p < pm->get_playlist_count(); ++p) {
        // Favorites has its own command (it understands local vs. provider songs).
        if (p == exclude || p == favorites || media::IsQueue(p) || media::IsAutoPlaylist(p)) continue;
        pfc::string8 name;
        pm->playlist_get_name(p, name);
        out.push_back({p, Wide(name.get_ptr())});
    }
    return out;
}

void AddToLocalPlaylist(const LocalPlaylistTarget& target, const metadb_handle_list& items) {
    if (!items.get_count()) return;
    auto pm = playlist_manager::get();
    t_size p = target.index;
    pfc::string8 name;
    if (p >= pm->get_playlist_count() || !pm->playlist_get_name(p, name) ||
        Wide(name.get_ptr()) != target.name)
        p = pm->find_playlist(pfc::stringcvt::string_utf8_from_wide(target.name.c_str()).get_ptr());
    if (p != SIZE_MAX) pm->playlist_add_items(p, items, pfc::bit_array_false());
}

SongsFetch Ready(metadb_handle_list items) {
    return [items = std::move(items)](std::function<void(metadb_handle_list)> ready) { ready(items); };
}

std::vector<stream::Id> TrackIds(stream::Provider& provider, const metadb_handle_list& items) {
    std::vector<stream::Id> ids;
    for (size_t i = 0; i < items.get_count(); ++i)
        if (auto id = provider.TrackId(items[i]); !id.empty()) ids.push_back(std::move(id));
    return ids;
}

void ShowProviderError(const stream::Provider& provider, const std::wstring& error) {
    const auto text = pfc::stringcvt::string_utf8_from_wide(Tr(error).c_str());
    const auto title = pfc::stringcvt::string_utf8_from_wide(Tr(provider.Name()).c_str());
    popup_message::g_show(text.get_ptr(), title.get_ptr());
}

void AddRemotePlaylistMenu(std::vector<MenuAction>& actions, stream::Provider& provider, SongsFetch songs,
                           const stream::Id& exclude) {
    if (!provider.Can(stream::Capability::AppendPlaylistTracks) || !provider.AccountVerified()) return;
    std::vector<MenuAction> targets;
    std::set<stream::Id> seen;
    for (const auto& section : provider.Sections()) {
        if (section.kind != stream::SectionKind::PlaylistShelf || section.shelf != stream::Shelf::Created)
            continue;
        for (const auto& playlist : provider.SectionPlaylists(section.id)) {
            if (playlist.id == exclude || !seen.insert(playlist.id).second) continue;
            targets.push_back(
                {playlist.name.empty() ? provider.Text(L"Unnamed playlist") : playlist.name,
                 [&provider, songs, id = playlist.id] {
                     songs([&provider, id](metadb_handle_list items) {
                         if (auto ids = TrackIds(provider, items); !ids.empty())
                             provider.AddToPlaylist(id, std::move(ids));
                     });
                 }});
        }
    }
    if (targets.empty()) targets.push_back({provider.Text(L"No created playlists"), nullptr});
    actions.push_back({Tr(L"Add to " + provider.Name() + L" playlist"), nullptr, std::move(targets)});
}

void AddBulkActions(std::vector<MenuAction>& actions, SongsFetch songs, stream::Provider* remote,
                    const stream::Id& excludeRemote, size_t excludeLocal, std::function<void()> play) {
    if (play)
        actions.push_back({L"Play", std::move(play)});
    else
        actions.push_back(
            {L"Play", [songs] { songs([](metadb_handle_list items) { media::PlayQueue(items, 0); }); }});
    actions.push_back(
        {L"Play next", [songs] { songs([](metadb_handle_list items) { media::PlayNext(items); }); }});
    actions.push_back(
        {L"Add to queue", [songs] { songs([](metadb_handle_list items) { media::AddToQueue(items); }); }});
    std::vector<MenuAction> local;
    for (auto& target : LocalPlaylistTargets(excludeLocal)) {
        const std::wstring label = target.name.empty() ? Tr(L"Unnamed playlist") : target.name;
        local.push_back({label, [songs, target = std::move(target)] {
                             songs([target](metadb_handle_list items) { AddToLocalPlaylist(target, items); });
                         }});
    }
    if (local.empty()) local.push_back({Tr(L"No playlists"), nullptr});
    actions.push_back({L"Add to playlist", nullptr, std::move(local)});
    if (remote && remote->Can(stream::Capability::Download))
        AddBulkDownloadActions(actions, songs, core_api::get_main_window());
    if (remote) AddRemotePlaylistMenu(actions, *remote, std::move(songs), excludeRemote);
}

} // namespace glasswave::ui
