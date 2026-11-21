#pragma once

#include "netease/types.h"
#include "stream/model.h"

#include <string>
#include <vector>

// Where a NetEase object leaves the service's own shape: the HTTP client parses the JSON into
// netease:: types, and these functions turn them, once, into the stream:: records everything
// else (the session's lists, the catalog, the UI) works with.
namespace glasswave::netease {

// The decimal id of an object; empty for 0.
inline stream::Id IdText(int64_t id) {
    return id ? std::to_wstring(id) : stream::Id();
}
// The service's id of a stream::Id; 0 when it is not one.
inline int64_t Num(const stream::Id& id) {
    return id.empty() ? 0 : static_cast<int64_t>(_wcstoi64(id.c_str(), nullptr, 10));
}

// What NetEase keeps of a song beyond its stream::Track: the quality of the stream it knows.
struct SongTech {
    std::wstring codec;
    int bitrate = 0; // bits per second, as NetEase reports it (foobar2000 shows kbps)
    int sampleRate = 0;
    int bitsPerSample = 0;
    bool operator==(const SongTech&) const = default;
};

// A song as the catalog keeps it: the track, its own cover and its stream's quality.
struct SongRecord {
    stream::Track track;
    std::wstring cover;
    SongTech tech;
};

// Which of the account's lists a playlist is in.
enum class PlaylistScope {
    Unknown,
    Created,
    Collected,
    Recommended
};

// "YYYY-MM-DD" of a publish time, the calendar day the service means (it stores release days as
// midnight in China, UTC+8); empty when there is none.
std::wstring Date(int64_t publishTimeMs);

stream::Track ToTrack(const Song& song);
SongRecord ToSong(const Song& song);
std::vector<SongRecord> ToSongs(const std::vector<Song>& songs);
std::vector<stream::Track> TracksOf(const std::vector<SongRecord>& songs);
std::vector<stream::Track> ToTracks(const std::vector<Song>& songs);

// `userId` is the signed-in account (0: unknown); `scope` the list the playlist was found in.
// own: created by the account; saved: in its library without being its own.
stream::Playlist ToPlaylist(const Playlist& playlist, int64_t userId, PlaylistScope scope);
stream::Album ToAlbum(const Album& album);
std::vector<stream::Album> ToAlbums(const std::vector<Album>& albums);
stream::Artist ToArtist(const Artist& artist);
// The followed-artists list is authoritative even when a profile lags behind a follow.
void MarkFollowed(stream::Artist& artist, const std::vector<stream::Artist>& followed);

} // namespace glasswave::netease
