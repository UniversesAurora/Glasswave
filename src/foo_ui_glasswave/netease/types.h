#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace glasswave::netease {

struct ArtistRef {
    int64_t id = 0;
    std::wstring name;
};

// A NetEase Cloud Music song, reduced to what the UI and file_info need.
struct Song {
    int64_t id = 0;
    std::wstring title;
    std::wstring album;
    std::wstring albumArtist;
    std::vector<std::wstring> artists;
    std::vector<ArtistRef> artistRefs;  // ids for artist pages; may be empty in old caches
    int64_t albumId = 0;
    std::wstring coverUrl;
    int durationMs = 0;
    int fee = 0;
    int popularity = 0;  // NetEase pop score when the endpoint reports one
    std::wstring codec;
    int bitrate = 0;  // bits per second, as NetEase reports it (foobar2000 shows kbps)
    int sampleRate = 0;
    int bitsPerSample = 0;
};

// A NetEase playlist (created, subscribed or the liked "我喜欢的音乐" list).
struct Playlist {
    int64_t id = 0;
    std::wstring name;
    std::wstring coverUrl;
    int trackCount = 0;
    int64_t creatorId = 0;
    std::wstring creatorName;
    std::wstring description;
    bool subscribed = false;
};

// A NetEase Cloud Music artist page.
struct Artist {
    int64_t id = 0;
    std::wstring name;
    std::wstring avatarUrl;
    std::wstring brief;
    int albumCount = 0;
    int songCount = 0;
    bool followed = false;
    bool followKnown = false;  // the API reported the follow state
};

// A NetEase Cloud Music album as shown on an artist page.
struct Album {
    int64_t id = 0;
    std::wstring name;
    std::wstring coverUrl;
    int trackCount = 0;
    int64_t publishTime = 0;  // epoch milliseconds
    std::vector<ArtistRef> artistRefs;
    std::wstring description;
    std::wstring company;
};

struct Account {
    bool loggedIn = false;
    int64_t userId = 0;
    std::wstring nickname;
};

}  // namespace glasswave::netease
