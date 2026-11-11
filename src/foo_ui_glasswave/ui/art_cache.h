#pragma once

#include "cache/images.h"

#include <chrono>

// Front-cover thumbnails for the library views.
//
// Album art comes from foobar2000's album_art_manager_v2 (embedded pictures, folder.jpg
// and whatever the user configured), read and decoded on bounded background workers so
// painting never waits for disk I/O. Finished thumbnails are announced on the main
// thread; listeners repaint. Nothing runs while nothing is requested (idle = idle).

namespace glasswave::ui {

class ArtCache {
public:
    static ArtCache& Get();
    static ArtCache* Existing();  // null after Shutdown
    static void Shutdown();

    ~ArtCache();
    ArtCache(const ArtCache&) = delete;
    ArtCache& operator=(const ArtCache&) = delete;

    enum class State { Pending, Ready, Missing };
    // Thumbnail of `track`'s front cover fitting `sizePx`. While it loads (Pending) or when
    // there is none (Missing), *out is null. Main thread only.
    // `key` lets several rows of one album share a single load; empty derives it from the track.
    // Account changes replace old caller-held copies with a transparent in-memory placeholder.
    State Find(const std::wstring& key, const metadb_handle_ptr& track, int sizePx,
               winrt::com_ptr<IWICBitmapSource>* out);
    State Find(const metadb_handle_ptr& track, int sizePx, winrt::com_ptr<IWICBitmapSource>* out) {
        return Find(std::wstring(), track, sizePx, out);
    }

    // Called (coalesced, main thread) after new thumbnails arrive.
    int Subscribe(std::function<void()> callback);
    void Unsubscribe(int token);

    // Drops the cached thumbnails whose key starts with `prefix` (empty: all of them) and
    // the queued loads, then tells listeners to request them again. Used when a NetEase
    // cover URL changed server-side, e.g. after a playlist edit. Main thread only.
    // The old bitmap stays available while the new one loads, so covers never blank out.
    void Invalidate(std::wstring_view prefix);

    // Account changes are different from ordinary refreshes: an old account's bitmap must
    // not remain visible. Forget drops the affected memory entries and forces the next paint
    // to replace caller-held D2D copies before loading the new account. Main thread only.
    void Forget(std::wstring_view prefix);

    // Retries only failed entries under `prefix`. Ready and pending images are untouched.
    // Main thread only; the next visible request schedules the failed entry again.
    void InvalidateMissing(std::wstring_view prefix);

    // Bumps whenever a new thumbnail for this key has been decoded; callers that keep their
    // own D2D bitmap can compare it and replace the bitmap only when it really changed.
    uint64_t Version(const std::wstring& key, const metadb_handle_ptr& track, int sizePx) const;

    // Records an authoritative local-file result with no front cover. The absence is shared
    // by every requested size and expires, so a later cover publication is picked up without
    // making "not found" permanent. Provider failures are deliberately not recorded here:
    // their extractor currently maps transport and account errors to the same not-found value.
    void RecordMissing(const metadb_handle_ptr& track);

private:
    ArtCache();
    void Worker();
    void Delivered();
    void ImageChanged(cache::ImageEvent event);
    void RecordMissingSource(std::wstring_view sourceKey, uint64_t accountRevision);

    struct Entry {
        State state = State::Pending;
        winrt::com_ptr<IWICBitmapSource> bitmap;
        uint64_t used = 0;
        uint64_t epoch = 0;   // a load started for an older epoch must not fill this entry
        uint64_t content = 0; // bumped when a new bitmap is decoded (see Version)
        bool stale = false;   // invalidated: keep showing the old bitmap while it reloads
        bool reloading = false;
        bool placeholder = false;
        std::wstring sourceKey;
        uint64_t accountRevision = 0;
        metadb_handle_ptr track;
        int sizePx = 0;
    };
    struct Request {
        std::wstring sourceKey;
        metadb_handle_ptr track;
        int sizePx = 0;
        uint64_t epoch = 0;
        std::string provider;
        uint64_t accountRevision = 0;
    };
    struct Done {
        std::wstring sourceKey;
        winrt::com_ptr<IWICBitmapSource> bitmap;
        uint64_t epoch = 0;
        std::string provider;
        uint64_t accountRevision = 0;
        int sizePx = 0;
        bool definitiveMissing = false;
    };
    struct MissingRecord {
        uint64_t accountRevision = 0;
        std::chrono::steady_clock::time_point expiresAt{};
    };

    std::map<std::wstring, Entry> m_entries;  // main thread
    std::map<std::wstring, uint64_t> m_sourceRevisions;
    std::map<std::wstring, MissingRecord> m_missing;
    std::map<std::wstring, std::chrono::steady_clock::time_point> m_retryAfter;
    std::set<std::wstring> m_replaceCallerCopies;
    winrt::com_ptr<IWICBitmapSource> m_resetArt;
    uint64_t m_clock = 0;
    uint64_t m_epoch = 0;
    uint64_t m_generation = 0;
    std::map<int, std::function<void()>> m_listeners;
    int m_nextToken = 1;
    int m_imageToken = 0;

    std::mutex m_mutex;  // guards m_queue, m_done, m_quit
    std::condition_variable m_wake;
    std::deque<Request> m_queue;  // newest first: what was just scrolled into view loads first
    std::vector<Done> m_done;
    bool m_quit = false;
    std::vector<std::thread> m_threads;
    abort_callback_impl m_abort;
};

}  // namespace glasswave::ui
