#include "pch.h"
#include "ui/art_cache.h"
#include "ui/shell_glue.h"

namespace glasswave::ui {

namespace {
std::unique_ptr<ArtCache> g_cache;
constexpr size_t kMaxEntries = 600;  // thumbnails kept (about 90 KB each at 150 px)
constexpr int kWorkers = 3;          // covers of the visible albums load in parallel
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
    std::wstring cacheKey = key;
    if (cacheKey.empty()) {
        cacheKey = pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr();
        cacheKey += L'|' + std::to_wstring(track->get_subsong_index());
    }
    cacheKey += L'|' + std::to_wstring(sizePx);

    auto [it, inserted] = m_entries.try_emplace(cacheKey);
    it->second.used = ++m_clock;
    if (inserted) {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_front({cacheKey, track, sizePx});
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
    *out = it->second.bitmap;
    return it->second.state;
}

int ArtCache::Subscribe(std::function<void()> callback) {
    m_listeners[m_nextToken] = std::move(callback);
    return m_nextToken++;
}

void ArtCache::Unsubscribe(int token) { m_listeners.erase(token); }

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
            m_done.emplace_back(std::move(req.key), std::move(bitmap));
        }
        if (first) fb2k::inMainThread([] {
                if (g_cache) g_cache->Delivered();
            });
    }
    winrt::uninit_apartment();
}

void ArtCache::Delivered() {
    std::vector<std::pair<std::wstring, winrt::com_ptr<IWICBitmapSource>>> done;
    {
        std::lock_guard lock(m_mutex);
        done.swap(m_done);
    }
    for (auto& [key, bitmap] : done) {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) continue;  // evicted meanwhile
        it->second.state = bitmap ? State::Ready : State::Missing;
        it->second.bitmap = std::move(bitmap);
    }
    if (done.empty()) return;
    auto listeners = m_listeners;  // a callback may unsubscribe
    for (auto& [token, cb] : listeners) cb();
}

}  // namespace glasswave::ui
