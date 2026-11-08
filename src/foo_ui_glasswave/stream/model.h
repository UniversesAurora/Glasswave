#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Provider-neutral catalogue records shown by the UI. A streaming provider (NetEase Cloud
// Music today, others later) maps its own API objects to these; the UI never sees a
// provider's native types. Ids are opaque strings scoped to their provider.
namespace glasswave::stream {

using Id = std::wstring;

struct ArtistRef {
    Id id;
    std::wstring name;
};

struct Track {
    Id id;
    std::wstring title;
    std::wstring album;
    Id albumId;
    std::vector<ArtistRef> artists;  // performers, in credit order
    int durationMs = 0;
    int popularity = 0;  // provider's ranking score where it has one (orders hot songs by album)
};

struct Playlist {
    Id id;
    std::wstring name;
    std::wstring coverUrl;
    std::wstring creator;
    std::wstring description;
    int trackCount = 0;
    bool own = false;        // created by the signed-in user (editable, cannot be saved)
    bool saved = false;      // in the user's library ("collected")
};

struct Album {
    Id id;
    std::wstring name;
    std::wstring coverUrl;
    std::vector<ArtistRef> artists;
    std::wstring description;
    int trackCount = 0;
    int year = 0;  // 0 = unknown
};

struct Artist {
    Id id;
    std::wstring name;
    std::wstring avatarUrl;
    std::wstring bio;
    int albumCount = 0;
    int trackCount = 0;
    bool followKnown = false;  // the provider reported the follow state
    bool followed = false;
};

// The playlist sections of a provider's library.
enum class Shelf { Created, Saved, Recommended };

// What a cover belongs to; covers are shared by every place that shows the same object.
enum class ArtKind { Track, Album, Artist, Playlist };

std::wstring JoinArtists(const std::vector<ArtistRef>& artists);

}  // namespace glasswave::stream
