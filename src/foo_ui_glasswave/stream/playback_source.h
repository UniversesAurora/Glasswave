#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

class abort_callback;

namespace glasswave::stream {

// Unavailable is an expected service answer (no accessible stream). Failed means the
// request or response could not be trusted and must retain normal error reporting.
enum class SourceStatus { Playable, Unavailable, Failed };

inline SourceStatus CheckSourceUrl(std::wstring_view url) {
    if (url.find_first_not_of(L" \t\r\n") == std::wstring_view::npos) return SourceStatus::Unavailable;
    const auto schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring_view::npos) return SourceStatus::Failed;
    std::wstring scheme(url.substr(0, schemeEnd));
    for (auto& c : scheme)
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    if (scheme != L"http" && scheme != L"https") return SourceStatus::Failed;
    const auto authority = url.substr(schemeEnd + 3);
    const auto host = authority.substr(0, authority.find_first_of(L"/?#"));
    if (host.empty() || host == L":" || url.find_first_of(L" \t\r\n") != std::wstring_view::npos)
        return SourceStatus::Failed;
    return SourceStatus::Playable;
}

// A failed quality request must not turn into an availability verdict just because
// another quality returned no URL. A playable fallback still wins.
inline SourceStatus CombineSourceStatus(SourceStatus before, SourceStatus after) {
    if (before == SourceStatus::Playable || after == SourceStatus::Playable) return SourceStatus::Playable;
    if (before == SourceStatus::Failed || after == SourceStatus::Failed) return SourceStatus::Failed;
    return SourceStatus::Unavailable;
}

// Only remembers unavailable items during the current playback attempt. A new user
// playback command, account/quality change or stop starts a fresh attempt. Nothing is
// persisted, and an old worker cannot mark an item in a newer attempt unavailable.
class SourceAvailability {
public:
    uint64_t Epoch() const {
        std::lock_guard lock(m_mutex);
        return m_epoch;
    }
    void Reset() {
        std::lock_guard lock(m_mutex);
        ++m_epoch;
        m_unavailable.clear();
    }
    void MarkUnavailable(uint64_t epoch, std::string_view path) {
        std::lock_guard lock(m_mutex);
        if (epoch == m_epoch) m_unavailable.emplace(path);
    }
    bool CanPlay(std::string_view path) const {
        std::lock_guard lock(m_mutex);
        return m_unavailable.find(path) == m_unavailable.end();
    }

private:
    mutable std::mutex m_mutex;
    uint64_t m_epoch = 0;
    std::set<std::string, std::less<>> m_unavailable;
};

uint64_t SourcePlaybackEpoch();
void ResetSourceAvailability();
bool CanPlaySource(std::string_view path);
// Called by provider inputs before opening a CDN URL. True signals an expected skip
// during realtime playback. Conversion/scanning still reports the unavailable source.
bool SkipUnavailableSource(uint64_t epoch, std::string_view path, std::wstring_view reason,
                           unsigned flags, abort_callback& abort);

// Provider inputs call this (any thread) as playback starts opening one of their tracks,
// before the stream is resolved, so the UI can show the song it is about to play. The
// listener runs on the main thread with the track's path; a gapless pre-open of the next
// song reports it too, so the listener decides whether a switch is under way.
void AnnouncePlaybackOpening(std::string_view path, unsigned flags);
void SetPlaybackOpeningListener(std::function<void(const std::string& path)> listener);

}  // namespace glasswave::stream
