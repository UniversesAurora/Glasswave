#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace glasswave::netease {

// A NetEase Cloud Music song, reduced to what the UI and file_info need.
struct Song {
    int64_t id = 0;
    std::wstring title;
    std::wstring album;
    std::wstring albumArtist;
    std::vector<std::wstring> artists;
    int64_t albumId = 0;
    std::wstring coverUrl;
    int durationMs = 0;
    int fee = 0;
    std::wstring codec;
    int bitrate = 0;
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
    bool subscribed = false;
};

struct Account {
    bool loggedIn = false;
    int64_t userId = 0;
    std::wstring nickname;
};

}  // namespace glasswave::netease
