#include "pch.h"
#include "ui/remote_pages_parts.h"

// A record's menu (ui/item_menu.h), playing a source's playlist or album, and the playlist
// commands (new, rename, delete).
namespace glasswave::ui {

using namespace remotepages;

// ---- A record's menu (ui/item_menu.h) ----------------------------------------------------------

namespace {
MenuAction SearchFor(PageHost& host, const wchar_t* what, const std::wstring& name) {
    return {Tr(what) + L" \u201C" + name + L"\u201D", [&host, name] { host.Search(name); }};
}
} // namespace

std::vector<MenuAction> RecordActions(PageHost& host, stream::Provider& provider, const MenuRecord& record,
                                      const RecordMenuContext& context) {
    std::vector<MenuAction> actions;
    const auto group = [&actions] { AppendSeparator(actions); };
    const bool account = provider.AccountVerified();
    if (const auto* album = std::get_if<stream::Album>(&record)) {
        const stream::Album a = *album;
        if (!context.onItsPage)
            actions.push_back({L"View", [&host, &provider, a, section = context.sectionId] {
                                   host.Open(AlbumView(provider, a.id, a.name, section));
                               }});
        group();
        AddBulkActions(actions, FetchAlbumSongs(provider, a.id, &host), &provider, {}, SIZE_MAX,
                       [&provider, id = a.id] { PlayRemoteAlbum(provider, id); });
        if (account && provider.Can(stream::Capability::SaveAlbums)) {
            group();
            actions.push_back(SaveAlbumAction(provider, a.id, [removed = context.removed, id = a.id](bool saved) {
                if (!saved && removed) removed(id);
            }));
        }
        group();
        AddGoToArtist(actions, host, provider, a.artists, context.sectionId);
        group();
        actions.push_back(SearchFor(host, L"Search for album", a.name));
    } else if (const auto* artist = std::get_if<stream::Artist>(&record)) {
        const stream::Artist a = *artist;
        if (!context.onItsPage)
            actions.push_back({L"View", [&host, &provider, a, section = context.sectionId] {
                                   host.Open(ArtistView(provider, a.id, a.name, section));
                               }});
        group();
        // The artist's songs: what its page lists first (top songs, or an uploaded artist's).
        const stream::ListQuery songs = IdQuery(stream::ListKind::ArtistTopTracks, a.id);
        AddBulkActions(actions, FetchListSongs(provider, songs, &host), &provider, {}, SIZE_MAX,
                       [&provider, songs] { PlayRemoteList(provider, songs, false); });
        if (account && SupportsArtistPreference(provider)) {
            group();
            actions.push_back(FollowAction(provider, a.id, [removed = context.removed, id = a.id](bool followed) {
                if (!followed && removed) removed(id);
            }));
        }
        group();
        actions.push_back(SearchFor(host, L"Search for artist", a.name));
    } else if (const auto* playlist = std::get_if<stream::Playlist>(&record)) {
        const stream::Id id = playlist->id;
        const std::wstring name = playlist->name;
        const auto shelf = context.shelf;
        if (!context.onItsPage)
            actions.push_back({L"View", [&host, &provider, id, name, shelf, section = context.sectionId] {
                                   host.Open(PlaylistView(provider, id, name, shelf, section));
                               }});
        group();
        AddBulkActions(actions, FetchPlaylistSongs(provider, id, &host), &provider, id, SIZE_MAX,
                       [&provider, id] { PlayRemotePlaylist(provider, id); });
        const auto info = provider.FindPlaylist(id);
        const PlaylistFlags flags = ResolvePlaylistFlags(provider, id, shelf, info ? &*info : nullptr);
        group();
        for (auto& action : PlaylistAccountActions(
                 host, provider, id, name, context.sectionId, flags,
                 [&provider, id, saved = flags.saved, removed = context.removed] {
                     provider.SetPlaylistSaved(id, !saved, [&provider, id, saved, removed](std::wstring error) {
                         if (!error.empty()) return ShowProviderError(provider, error);
                         if (saved && removed) removed(id);
                     });
                 }))
            actions.push_back(std::move(action));
        group();
        actions.push_back(SearchFor(host, L"Search for playlist", name));
    }
    if (!context.page.empty()) {
        group();
        for (const auto& action : context.page) actions.push_back(action);
    }
    while (!actions.empty() && actions.back().label.empty() && !actions.back().run) actions.pop_back();
    return actions;
}

void ShowRecordMenu(PageHost& host, stream::Provider& provider, const MenuRecord& record, POINT screen,
                    const RecordMenuContext& context) {
    ShowActionMenu(host.Owner(), screen, RecordActions(host, provider, record, context));
}

void PlayRemotePlaylist(stream::Provider& provider, const stream::Id& playlist) {
    PlayRemoteList(provider, IdQuery(stream::ListKind::PlaylistTracks, playlist), true);
}

void PlayRemoteAlbum(stream::Provider& provider, const stream::Id& album) {
    PlayRemoteList(provider, IdQuery(stream::ListKind::AlbumTracks, album), false);
}

// ---- playlist commands --------------------------------------------------------------------------

void ShowRemotePlaylistMenu(PageHost& host, stream::Provider& provider, const stream::Id& id,
                            const std::wstring& name, std::optional<Shelf> shelf, const stream::Id& sectionId,
                            POINT screen, std::function<void(const stream::Id&)> onUnsaved) {
    stream::Playlist playlist;
    playlist.id = id;
    playlist.name = name;
    RecordMenuContext context;
    context.sectionId = sectionId;
    context.shelf = shelf;
    context.removed = std::move(onUnsaved);
    if (shelf != Shelf::Recommended) context.page.push_back(RefreshAction(host));
    ShowRecordMenu(host, provider, playlist, screen, context);
}

MenuAction NewPlaylistAction(PageHost& host, stream::Provider& provider) {
    return {provider.NamedText(L"New {0} playlist"), [&host, &provider] { CreateRemotePlaylist(host, provider); }};
}

void RefreshProviderLibrary(stream::Provider& provider) {
    ArtCache::Get().Invalidate(std::wstring(provider.Key()) + L":");
    provider.RefreshLibrary();
}

void CreateRemotePlaylist(PageHost& host, stream::Provider& provider) {
    if (!provider.Can(stream::Capability::CreatePlaylists)) return;
    if (!provider.AccountVerified()) {
        if (!provider.SessionRestoring()) provider.SignIn(host.Owner());
        return;
    }
    const std::wstring title = provider.NamedText(L"New {0} playlist");
    const stream::NavigationSection section = CreatedPlaylistSection(provider);
    PromptText(host.Owner(), title, ProviderPlaylistText(provider, L"Playlist name"), title,
               [&host, &provider, section](std::wstring name) {
                   if (name.empty()) return;
                   provider.CreatePlaylist(name, [&host, &provider, name, section, alive = host.Lifetime()](
                                                     stream::Id id, std::wstring error) {
                       if (!error.empty()) return ShowProviderError(provider, error);
                       if (!alive.expired())
                           host.Open(PlaylistView(provider, id, name, Shelf::Created, section.id));
                   });
               });
}

void RenameRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id,
                          std::wstring name) {
    if (!provider.Can(stream::Capability::RenamePlaylists)) return;
    if (!provider.AccountVerified()) return;
    // The provider reports the new name as a library change; pages update from there.
    PromptText(host.Owner(), ProviderPlaylistText(provider, L"Rename playlist"),
               ProviderPlaylistText(provider, L"Playlist name"), name,
               [&provider, id, name](std::wstring newName) {
                   if (newName.empty() || newName == name) return;
                   provider.RenamePlaylist(id, newName, [&provider](std::wstring error) {
                       if (!error.empty()) ShowProviderError(provider, error);
                   });
               });
}

void DeleteRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id, std::wstring name,
                          const stream::Id& sectionId) {
    if (!provider.Can(stream::Capability::DeletePlaylists)) return;
    if (!provider.AccountVerified()) return;
    const std::wstring deleteText = ProviderPlaylistText(provider, L"Delete playlist");
    const std::wstring message = deleteText + L" \"" + name + L"\"?\n" + Tr(L"This cannot be undone.");
    if (!Confirm(host.Owner(), deleteText, message, true)) return;
    stream::NavigationSection section = CreatedPlaylistSection(provider);
    if (const auto current = FindSection(provider, sectionId);
        current && current->kind == stream::SectionKind::PlaylistShelf)
        section = *current;
    provider.DeletePlaylist(id, [&host, &provider, id, section, alive = host.Lifetime()](std::wstring error) {
        if (!error.empty()) return ShowProviderError(provider, error);
        if (alive.expired()) return;
        const View& current = host.CurrentView();
        if (current.kind == View::Kind::RemotePlaylist && current.provider == provider.Key() &&
            current.remoteId == id)
            host.Replace(ShelfView(provider, section.shelf, section.id, section.label));
    });
}

} // namespace glasswave::ui
