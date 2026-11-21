#include "pch.h"
#include "netease/convert.h"

#include <algorithm>
#include <ctime>

namespace glasswave::netease {

namespace {

bool ReleaseDay(int64_t publishTimeMs, std::tm& day) {
    if (publishTimeMs <= 0) return false;
    const time_t seconds = static_cast<time_t>(publishTimeMs / 1000 + 8 * 3600);
    return gmtime_s(&day, &seconds) == 0;
}

int Year(int64_t publishTimeMs) {
    std::tm day{};
    return ReleaseDay(publishTimeMs, day) ? day.tm_year + 1900 : 0;
}

// The service's album type as a UI key ("专辑" -> Album); an unknown one is shown as written.
std::wstring AlbumType(const Album& album) {
    if (album.subType.find(L"现场") != std::wstring::npos) return L"Live album";
    if (album.type == L"专辑") return L"Album";
    if (album.type == L"EP/Single") return L"EP or single";
    if (album.type == L"EP") return L"EP";
    if (album.type == L"Single") return L"Single";
    if (album.type == L"精选集" || album.type == L"合辑") return L"Compilation";
    return album.type;
}

std::vector<stream::ArtistRef> Refs(const std::vector<ArtistRef>& refs) {
    std::vector<stream::ArtistRef> out;
    out.reserve(refs.size());
    for (const auto& ref : refs)
        out.push_back({IdText(ref.id), ref.name});
    return out;
}

} // namespace

std::wstring Date(int64_t publishTimeMs) {
    std::tm day{};
    if (!ReleaseDay(publishTimeMs, day)) return {};
    wchar_t text[16]{};
    swprintf_s(text, L"%04d-%02d-%02d", day.tm_year + 1900, day.tm_mon + 1, day.tm_mday);
    return text;
}

stream::Track ToTrack(const Song& song) {
    stream::Track track;
    track.id = IdText(song.id);
    track.title = song.title;
    track.note = song.note;
    track.album = song.album;
    track.albumId = IdText(song.albumId);
    track.artists = Refs(song.artistRefs);
    if (track.artists.empty()) // a summary that names its artists without ids
        for (const auto& name : song.artists)
            track.artists.push_back({{}, name});
    track.durationMs = song.durationMs;
    track.popularity = song.popularity;
    if (song.original) track.badges.push_back(L"Original");
    return track;
}

SongRecord ToSong(const Song& song) {
    return {ToTrack(song), song.coverUrl, {song.codec, song.bitrate, song.sampleRate, song.bitsPerSample}};
}

std::vector<SongRecord> ToSongs(const std::vector<Song>& songs) {
    std::vector<SongRecord> out;
    out.reserve(songs.size());
    for (const auto& song : songs)
        out.push_back(ToSong(song));
    return out;
}

std::vector<stream::Track> TracksOf(const std::vector<SongRecord>& songs) {
    std::vector<stream::Track> out;
    out.reserve(songs.size());
    for (const auto& song : songs)
        out.push_back(song.track);
    return out;
}

std::vector<stream::Track> ToTracks(const std::vector<Song>& songs) {
    std::vector<stream::Track> out;
    out.reserve(songs.size());
    for (const auto& song : songs)
        out.push_back(ToTrack(song));
    return out;
}

stream::Playlist ToPlaylist(const Playlist& playlist, int64_t userId, PlaylistScope scope) {
    stream::Playlist out;
    out.id = IdText(playlist.id);
    out.name = playlist.name;
    out.coverUrl = playlist.coverUrl;
    out.creator = playlist.creatorName;
    out.description = playlist.description;
    out.trackCount = playlist.trackCount;
    out.created = Date(playlist.createTime);
    out.updated = Date(playlist.updateTime);
    out.updates = playlist.updateFrequency;
    out.tags = playlist.tags;
    if (!playlist.englishTitle.empty() && playlist.englishTitle != playlist.name)
        out.aliases.push_back(playlist.englishTitle);
    if (playlist.chart) out.type = L"Chart";
    const bool madeByAccount = playlist.creatorId ? playlist.creatorId == userId : !playlist.subscribed;
    out.own = scope == PlaylistScope::Created || madeByAccount;
    out.saved = !out.own && (playlist.subscribed || scope == PlaylistScope::Collected);
    return out;
}

stream::Album ToAlbum(const Album& album) {
    stream::Album out;
    out.id = IdText(album.id);
    out.name = album.name;
    out.coverUrl = album.coverUrl;
    out.artists = Refs(album.artistRefs);
    out.description = album.description;
    out.trackCount = album.trackCount;
    out.year = Year(album.publishTime);
    out.released = Date(album.publishTime);
    out.label = album.company;
    out.type = AlbumType(album);
    out.edition = album.subType;
    out.aliases = album.aliases;
    // Tags are one string of genre words ("华语 流行"), separated by spaces or commas.
    std::wstring tag;
    for (const wchar_t c : album.tags + L" ") {
        if (c == L' ' || c == L',' || c == L'，' || c == L'、' || c == L'/') {
            if (!tag.empty()) out.genres.push_back(std::move(tag));
            tag.clear();
        } else {
            tag += c;
        }
    }
    return out;
}

std::vector<stream::Album> ToAlbums(const std::vector<Album>& albums) {
    std::vector<stream::Album> out;
    out.reserve(albums.size());
    for (const auto& album : albums)
        out.push_back(ToAlbum(album));
    return out;
}

stream::Artist ToArtist(const Artist& artist) {
    stream::Artist out;
    out.id = IdText(artist.id);
    out.name = artist.name;
    out.avatarUrl = artist.avatarUrl;
    out.bio = artist.brief;
    out.albumCount = artist.albumCount;
    out.trackCount = artist.songCount;
    out.followKnown = artist.followKnown;
    out.followed = artist.followed;
    out.profile = artist.profile;
    out.aliases = artist.aliases;
    out.roles = artist.roles;
    out.videoCount = artist.videoCount < 0 ? stream::kUnknownCount : artist.videoCount;
    // The wiki's codes, as the service's artist lists name them. Untranslated UI keys.
    switch (artist.area) {
    case 7: out.area = L"Chinese"; break;
    case 96: out.area = L"Western"; break;
    case 8: out.area = L"Japanese"; break;
    case 16: out.area = L"Korean"; break;
    default: break;
    }
    out.kind = artist.type == 1   ? stream::ArtistKind::Male
               : artist.type == 2 ? stream::ArtistKind::Female
               : artist.type == 3 ? stream::ArtistKind::Group
                                  : stream::ArtistKind::Unknown;
    return out;
}

void MarkFollowed(stream::Artist& artist, const std::vector<stream::Artist>& followed) {
    for (const auto& saved : followed)
        if (saved.id == artist.id) {
            artist.followKnown = artist.followed = true;
            return;
        }
}

} // namespace glasswave::netease
