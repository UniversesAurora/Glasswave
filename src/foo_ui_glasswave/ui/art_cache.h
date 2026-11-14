#pragma once

#include "cache/images.h"

#include <chrono>
#include <unordered_map>

// Front-cover thumbnails for every view.
//
// Album art comes from foobar2000's album_art_manager_v2 (embedded pictures, folder.jpg,
// whatever the user configured, and the streaming providers' extractors), read and decoded
// on bounded background workers so painting never waits for disk or network I/O. Finished
// thumbnails are announced on the main thread; listeners repaint. Nothing runs while nothing
// is requested (idle = idle).
//
// A picture is identified by its source key and held once: every view of one cover shares
// the record, each requested size is decoded at most once, and a smaller request is served
// from a larger decode. Once a picture has been shown it is never replaced by a blank while
// it reloads; the old bitmap stays until the new one is ready.

namespace glasswave::ui {

class ArtCache {
  public:
    static ArtCache& Get();
    static ArtCache* Existing(); // null after Shutdown
    static void Shutdown();

    ~ArtCache();
    ArtCache(const ArtCache&) = delete;
    ArtCache& operator=(const ArtCache&) = delete;

    enum class State { Pending, Ready, Missing };
    // Thumbnail of `track`'s front cover fitting `sizePx`. Main thread only.
    //   Ready:   *out is the picture for this size, final until the picture changes.
    //   Pending: it loads; *out is null or a stand-in to show meanwhile (a smaller decode
    //            scaled up, the old picture while it reloads, a blank after an account
    //            change). Listeners are notified when the load ends: ask again then.
    //   Missing: there is none; *out is null.
    // `key` names the picture; rows of one album pass the album's key so they share one load.
    // Empty derives it from the track (see SourceKey).
    // Account changes replace old caller-held copies with a transparent in-memory placeholder.
    State Find(const std::wstring& key, const metadb_handle_ptr& track, int sizePx,
               winrt::com_ptr<IWICBitmapSource>* out);
    State Find(const metadb_handle_ptr& track, int sizePx, winrt::com_ptr<IWICBitmapSource>* out) {
        return Find(std::wstring(), track, sizePx, out);
    }

    // The key Find uses for a track without an explicit one: the provider's cover key for a
    // streamed track (its album's when known, so the playing track, the queue and the lists
    // share the album's picture), else the file path and subsong.
    static std::wstring SourceKey(const metadb_handle_ptr& track);

    // Called (coalesced, main thread) after new thumbnails arrive.
    int Subscribe(std::function<void()> callback);
    void Unsubscribe(int token);

    // Reloads the pictures whose key starts with `prefix` (empty: all of them): the persistent
    // image cache revalidates them and listeners request them again. Used when a cover URL
    // changed server-side or the user asks for a refresh. Main thread only.
    // The old bitmap stays available while the new one loads, so covers never blank out.
    void Invalidate(std::wstring_view prefix);

    // Account changes are different from ordinary refreshes: an old account's bitmap must
    // not remain visible. Forget drops the affected records and forces the next paint to
    // replace caller-held D2D copies before loading the new account. Main thread only.
    void Forget(std::wstring_view prefix);

    // Retries only failed pictures under `prefix`. Ready and pending ones are untouched.
    // Main thread only; the next visible request schedules the failed picture again.
    void InvalidateMissing(std::wstring_view prefix);

    // Bumps whenever a new thumbnail for this key has been decoded; callers that keep their
    // own D2D bitmap can compare it and replace the bitmap only when it really changed.
    uint64_t Version(const std::wstring& key, const metadb_handle_ptr& track, int sizePx) const;

  private:
    using Clock = std::chrono::steady_clock;

    ArtCache();
    // Two lanes. The local lane decodes what is already on this machine (files, the image
    // cache) and never waits on a service; a provider picture that is not cached moves to the
    // network lane. A slow or starting service therefore never holds up cached pictures.
    void Worker(bool network);
    void Delivered();
    void ImageChanged(cache::ImageEvent event);

    // One decoded size of a picture.
    struct Entry {
        State state = State::Pending;
        winrt::com_ptr<IWICBitmapSource> bitmap;
        uint64_t epoch = 0;   // a load started for an older epoch must not fill this entry
        uint64_t content = 0; // bumped when a new bitmap is decoded (see Version)
        bool stale = false;   // invalidated: keep showing the old bitmap while it reloads
        bool reloading = false;
        bool placeholder = false;
        unsigned failures = 0;
        Clock::time_point retryAt{}; // a failed provider picture is asked for again after this
        metadb_handle_ptr track;
    };
    // One picture, in every size requested so far.
    struct Source {
        std::string provider; // empty for local files
        uint64_t accountRevision = 0;
        uint64_t used = 0;
        bool replaceCallerCopies = false; // the account changed under a picture already shown
        std::map<int, Entry> sizes;
    };
    struct Request {
        std::wstring sourceKey;
        metadb_handle_ptr track;
        int sizePx = 0;
        uint64_t epoch = 0;
        std::string provider;
        uint64_t accountRevision = 0;
        bool network = false; // missed the cache: may fetch
    };
    struct Done {
        std::wstring sourceKey;
        winrt::com_ptr<IWICBitmapSource> bitmap;
        uint64_t epoch = 0;
        std::string provider;
        uint64_t accountRevision = 0;
        int sizePx = 0;
    };

    Source& SourceFor(const std::wstring& sourceKey);
    void Queue(const std::wstring& sourceKey, const Source& source, int sizePx, Entry& entry);
    void Reload(const std::wstring& sourceKey, const Source& source, int sizePx, Entry& entry);
    void DropQueued(std::wstring_view prefix);
    void Evict();
    void Notify();

    std::unordered_map<std::wstring, Source> m_sources; // main thread
    size_t m_entryCount = 0;
    winrt::com_ptr<IWICBitmapSource> m_resetArt;
    uint64_t m_clock = 0;
    uint64_t m_epoch = 0;
    uint64_t m_generation = 0;
    std::map<int, std::function<void()>> m_listeners;
    int m_nextToken = 1;
    int m_imageToken = 0;

    std::mutex m_mutex; // guards m_queue, m_network, m_done, m_quit
    std::condition_variable m_wake, m_wakeNetwork;
    std::deque<Request> m_queue;   // newest first: what was just scrolled into view loads first
    std::deque<Request> m_network; // cache misses of provider pictures, newest first
    std::vector<Done> m_done;
    bool m_quit = false;
    std::vector<std::thread> m_threads;
    abort_callback_impl m_abort;
};

} // namespace glasswave::ui
