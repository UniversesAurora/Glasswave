#pragma once

#include "stream/provider.h"
#include "ui/main_menu.h"

#include <functional>
#include <vector>

// Menu commands that act on a set of songs, shared by every page: play, queue, copy to a
// foobar2000 playlist or to a provider's own playlist.
namespace glasswave::ui {

// A foobar2000 playlist chosen in a menu. The index can change while a remote list is
// being fetched, so the name is checked again before the songs are added.
struct LocalPlaylistTarget {
    size_t index = SIZE_MAX;
    std::wstring name;
};
// Playlists that accept copied songs: not the queue, Favorites or auto playlists.
std::vector<LocalPlaylistTarget> LocalPlaylistTargets(size_t exclude = SIZE_MAX);
void AddToLocalPlaylist(const LocalPlaylistTarget& target, const metadb_handle_list& items);

// Songs that are only known after a fetch (a whole remote playlist or album).
using SongsFetch = std::function<void(std::function<void(metadb_handle_list)> ready)>;
SongsFetch Ready(metadb_handle_list items);

// Play / Play next / Add to queue / Add to playlist, plus Add to <provider> playlist when
// `remote` is given (its own playlists, without `excludeRemote`).
void AddBulkActions(std::vector<MenuAction>& actions, SongsFetch songs, stream::Provider* remote = nullptr,
                    const stream::Id& excludeRemote = {}, size_t excludeLocal = SIZE_MAX);

// "Add to <provider> playlist" for songs of that provider; nothing when it has no
// editable playlists.
void AddRemotePlaylistMenu(std::vector<MenuAction>& actions, stream::Provider& provider, SongsFetch songs,
                           const stream::Id& exclude = {});

// A provider error shown to the user.
void ShowProviderError(const stream::Provider& provider, const std::wstring& error);

// The provider ids of the items that belong to it.
std::vector<stream::Id> TrackIds(stream::Provider& provider, const metadb_handle_list& items);

}  // namespace glasswave::ui
