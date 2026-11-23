#pragma once

#include <string>
#include <vector>

// Playlists the user played, newest first, for Recently Played's Playlists tab: a provider's
// playlist (its key and id) or a foobar2000 playlist (no provider; its name, since playlist
// indices move). Written when a playlist's songs start playing from its page or its play button.
// Main thread.
namespace glasswave::media {

struct RecentList {
    std::wstring provider; // empty: a foobar2000 playlist
    std::wstring id;       // the provider's playlist id, or the foobar2000 playlist's name
    std::wstring name;
    int64_t played = 0; // Unix seconds
};

std::vector<RecentList> RecentLists();
void RecordRecentList(const std::wstring& provider, const std::wstring& id, const std::wstring& name);
void ForgetRecentList(const std::wstring& provider, const std::wstring& id);
void ClearRecentLists();

} // namespace glasswave::media
