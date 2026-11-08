#pragma once

// Front-cover thumbnails for the library views.
//
// Album art comes from foobar2000's album_art_manager_v2 (embedded pictures, folder.jpg
// and whatever the user configured), read and decoded on one background thread so
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
    void Invalidate(std::wstring_view prefix);

private:
    ArtCache();
    void Worker();
    void Delivered();

    struct Entry {
        State state = State::Pending;
        winrt::com_ptr<IWICBitmapSource> bitmap;
        uint64_t used = 0;
        uint64_t epoch = 0;  // a load started for an older epoch must not fill this entry
    };
    struct Request {
        std::wstring key;
        metadb_handle_ptr track;
        int sizePx = 0;
        uint64_t epoch = 0;
    };
    struct Done {
        std::wstring key;
        winrt::com_ptr<IWICBitmapSource> bitmap;
        uint64_t epoch = 0;
    };

    std::map<std::wstring, Entry> m_entries;  // main thread
    uint64_t m_clock = 0;
    uint64_t m_epoch = 0;
    std::map<int, std::function<void()>> m_listeners;
    int m_nextToken = 1;

    std::mutex m_mutex;  // guards m_queue, m_done, m_quit
    std::condition_variable m_wake;
    std::deque<Request> m_queue;  // newest first: what was just scrolled into view loads first
    std::vector<Done> m_done;
    bool m_quit = false;
    std::vector<std::thread> m_threads;
    abort_callback_impl m_abort;
};

}  // namespace glasswave::ui
