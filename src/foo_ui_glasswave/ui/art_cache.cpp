#include "pch.h"
#include "ui/art_cache.h"
#include "ui/shell_glue.h"

namespace glasswave::ui {

namespace {
std::unique_ptr<ArtCache> g_cache;
constexpr size_t kMaxEntries = 600;  // thumbnails kept (about 90 KB each at 150 px)
constexpr int kWorkers = 3;          // covers of the visible albums load in parallel

std::wstring CacheKeyFor(const std::wstring& key, const metadb_handle_ptr& track, int sizePx) {
    std::wstring cacheKey = key;
    if (cacheKey.empty()) {
        cacheKey = pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr();
        cacheKey += L'|' + std::to_wstring(track->get_subsong_index());
    }
    cacheKey += L'|' + std::to_wstring(sizePx);
    return cacheKey;
}
}  // namespace

ArtCache& ArtCache::Get() {
    if (!g_cache) g_cache.reset(new ArtCache());
    return *g_cache;
}

ArtCache* ArtCache::Existing() { return g_cache.get(); }

void ArtCache::Shutdown() { g_cache.reset(); }

ArtCache::ArtCache() {
    for (int i = 0; i < kWorkers; ++i) m_threads.emplace_back([this] { Worker(); });
}

ArtCache::~ArtCache() {
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_queue.clear();
    }
    m_abort.abort();
    m_wake.notify_all();
    for (auto& thread : m_threads)
        if (thread.joinable()) thread.join();
}

ArtCache::State ArtCache::Find(const std::wstring& key, const metadb_handle_ptr& track, int sizePx,
                               winrt::com_ptr<IWICBitmapSource>* out) {
    *out = nullptr;
    if (!track.is_valid()) return State::Missing;
    const std::wstring cacheKey = CacheKeyFor(key, track, sizePx);

    // The same picture is requested at several sizes (grid tile, detail header, list
    // column). Reuse a finished larger thumbnail instead of decoding it again; the list
    // scales it into its smaller box.
    if (m_entries.find(cacheKey) == m_entries.end()) {
        const size_t bar = cacheKey.rfind(L'|');
        if (bar != std::wstring::npos) {
            const std::wstring prefix = cacheKey.substr(0, bar + 1);
            const Entry* best = nullptr;
            int bestSize = 0;
            for (const auto& [candidate, candidateEntry] : m_entries) {
                if (candidateEntry.state != State::Ready || candidateEntry.stale || !candidateEntry.bitmap)
                    continue;
                if (candidate.size() <= prefix.size() || candidate.compare(0, prefix.size(), prefix) != 0)
                    continue;
                const int candidateSize = _wtoi(candidate.c_str() + prefix.size());
                if (candidateSize < sizePx || (best && candidateSize >= bestSize)) continue;
                best = &candidateEntry;
                bestSize = candidateSize;
            }
            if (best) {
                Entry& alias = m_entries[cacheKey];
                alias.state = State::Ready;
                alias.bitmap = best->bitmap;
                alias.used = ++m_clock;
                alias.content = best->content;
                *out = alias.bitmap;
                return State::Ready;
            }
        }
    }

    auto [it, inserted] = m_entries.try_emplace(cacheKey);
    Entry& entry = it->second;
    entry.used = ++m_clock;
    if (inserted) {
        entry.epoch = ++m_epoch;
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_front({cacheKey, track, sizePx, entry.epoch});
        }
        m_wake.notify_one();
        // Keep memory bounded: drop the least recently used finished thumbnails.
        if (m_entries.size() > kMaxEntries) {
            std::vector<std::pair<uint64_t, std::wstring>> old;
            for (auto& [k, e] : m_entries)
                if (e.state != State::Pending) old.emplace_back(e.used, k);
            std::sort(old.begin(), old.end());
            for (size_t i = 0; i < old.size() && m_entries.size() > kMaxEntries * 3 / 4; ++i)
                m_entries.erase(old[i].second);
        }
        return State::Pending;
    }
    if (entry.stale) {
        // Keep showing the old bitmap until the replacement is ready; enqueue the reload
        // once. A cover that did not change simply decodes the same image again.
        if (!entry.reloading) {
            entry.epoch = ++m_epoch;
            entry.reloading = true;
            {
                std::lock_guard lock(m_mutex);
                m_queue.push_front({cacheKey, track, sizePx, entry.epoch});
            }
            m_wake.notify_one();
        }
        *out = entry.bitmap;
        return entry.bitmap ? State::Ready : State::Pending;
    }
    *out = entry.bitmap;
    return entry.state;
}

int ArtCache::Subscribe(std::function<void()> callback) {
    m_listeners[m_nextToken] = std::move(callback);
    return m_nextToken++;
}

void ArtCache::Unsubscribe(int token) { m_listeners.erase(token); }

void ArtCache::Invalidate(std::wstring_view prefix) {
    for (auto& [key, entry] : m_entries) {
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        // Keep the old bitmap: callers draw it while the new one loads. The content bump
        // makes caches that hold their own D2D bitmap re-query this entry.
        entry.stale = true;
        entry.reloading = false;
        entry.epoch = ++m_epoch;
        ++entry.content;
    }
    {
        std::lock_guard lock(m_mutex);
        auto matches = [prefix](const std::wstring& key) { return key.compare(0, prefix.size(), prefix) == 0; };
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                     [&](const Request& request) { return matches(request.key); }),
                      m_queue.end());
        m_done.erase(std::remove_if(m_done.begin(), m_done.end(),
                                    [&](const Done& done) { return matches(done.key); }),
                     m_done.end());
    }
    auto listeners = m_listeners;  // a callback may unsubscribe
    for (auto& [token, cb] : listeners) cb();
}

uint64_t ArtCache::Version(const std::wstring& key, const metadb_handle_ptr& track, int sizePx) const {
    if (!track.is_valid()) return 0;
    const auto it = m_entries.find(CacheKeyFor(key, track, sizePx));
    return it == m_entries.end() ? 0 : it->second.content;
}

void ArtCache::Worker() {
    // WIC objects are created on this thread; the decoded bitmaps own their pixels.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
    for (;;) {
        Request req;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_quit || !m_queue.empty(); });
            if (m_quit) break;
            req = std::move(m_queue.front());
            m_queue.pop_front();
        }
        winrt::com_ptr<IWICBitmapSource> bitmap;
        try {
            auto api = album_art_manager_v2::get();
            auto extractor = api->open(pfc::list_single_ref_t<metadb_handle_ptr>(req.track),
                                       pfc::list_single_ref_t<GUID>(album_art_ids::cover_front), m_abort);
            album_art_data::ptr data = extractor->query(album_art_ids::cover_front, m_abort);
            if (wic) bitmap = DecodeArt(wic.get(), data, req.sizePx);
        } catch (const exception_aborted&) {
            break;
        } catch (...) {
            // Not found or unreadable: remembered as missing.
        }
        bool first = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_quit) break;
            first = m_done.empty();
            m_done.push_back({std::move(req.key), std::move(bitmap), req.epoch});
        }
        if (first) fb2k::inMainThread([] {
                if (g_cache) g_cache->Delivered();
            });
    }
    winrt::uninit_apartment();
}

void ArtCache::Delivered() {
    std::vector<Done> done;
    {
        std::lock_guard lock(m_mutex);
        done.swap(m_done);
    }
    for (auto& [key, bitmap, epoch] : done) {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) continue;  // evicted meanwhile
        Entry& entry = it->second;
        if (entry.epoch != epoch) continue;  // a newer load for this key is in flight
        entry.reloading = false;
        entry.stale = false;
        if (bitmap) {
            entry.bitmap = std::move(bitmap);
            entry.state = State::Ready;
            ++entry.content;  // a newer image: caches holding the old bitmap re-query
        } else if (!entry.bitmap) {
            entry.state = State::Missing;
        }
        // No new image but an old one is still here (a transient failure): keep showing it.
    }
    if (done.empty()) return;
    auto listeners = m_listeners;  // a callback may unsubscribe
    for (auto& [token, cb] : listeners) cb();
}

}  // namespace glasswave::ui
