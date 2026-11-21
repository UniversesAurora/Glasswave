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
    std::wstring note; // alia, else tns: shown in grey after the title (stream::Track::note)
    std::wstring album;
    std::vector<std::wstring> artists;
    std::vector<ArtistRef> artistRefs; // ids for artist pages
    int64_t albumId = 0;
    std::wstring coverUrl;
    int durationMs = 0;
    int fee = 0;
    int popularity = 0;  // NetEase pop score when the endpoint reports one
    bool original = false; // originCoverType 1: the original singer's (NetEase's 原唱 tag)
    std::wstring codec;
    int bitrate = 0;  // bits per second, as NetEase reports it (foobar2000 shows kbps)
    int sampleRate = 0;
    int bitsPerSample = 0;
};

// One song's details for the song page: /song/detail and the song wiki (音乐百科).
struct SongInfo {
    Song song;
    int disc = 0, number = 0;
    int64_t publishTime = 0; // epoch milliseconds
    std::vector<std::wstring> aliases; // alia and tns
    int originCoverType = 0;           // 1 original, 2 cover
    bool lossless = false, hiRes = false;
    std::vector<std::wstring> styles, tags; // 曲风, 推荐标签
    std::wstring language;
    int bpm = 0;
    std::vector<int64_t> similar; // the wiki's similar songs
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
    int64_t createTime = 0, updateTime = 0; // epoch ms; updateTime = last song added/removed
    std::vector<std::wstring> tags;
    std::wstring updateFrequency; // "每日更新"
    std::wstring englishTitle;
    bool chart = false; // ToplistType set: one of the service's charts
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
    // The profile (artist detail and artist wiki); empty / negative: unknown.
    bool profile = false;
    std::vector<std::wstring> aliases; // alias and transNames
    std::vector<std::wstring> roles;   // identify.imageDesc, split at '、', badges left out
    int videoCount = -1;               // mvSize
    int area = -1;                     // 7 Chinese, 96 Western, 8 Japanese, 16 Korean, 0 other
    int type = 0;                      // 1 male, 2 female, 3 group
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
    std::wstring type;    // "专辑", "EP/Single", "Single", ... as the service writes it
    std::wstring subType; // "录音室版", "现场版", ...
    std::wstring tags;    // the service's genre tags, as one string
    std::vector<std::wstring> aliases; // alias and transNames
};

// One of the cards at the top of the PC client's home page (/api/pc/daily/rcmd/block).
struct HomeCard {
    std::wstring label;    // coverText: "11-15" for the daily songs, 私人漫游, 私人雷达, ...
    std::wstring title;    // "从「…」听起"
    std::wstring subtitle; // simplifiedTitle
    std::wstring coverUrl;
    std::wstring resourceType, subResourceType, moduleType, resourceId;
    std::wstring targetUrl; // the client's action ("orpheus://songrcmd?tab=style&categoryId=...")
};

// One row of the PC client's home page: a block of playlists or of songs.
struct HomeBlock {
    std::wstring code;  // HOMEPAGE_BLOCK_PLAYLIST_RCMD, CUSTOMIZE_PLAYLIST_MGC, ...
    std::wstring title; // as the client heads it
    std::vector<Playlist> playlists;
    std::vector<Song> songs;
};

struct Account {
    bool loggedIn = false;
    int64_t userId = 0;
    std::wstring nickname;
};

}  // namespace glasswave::netease
