#include "pch.h"
#include "ui/art_cache.h"
#include "core/text.h"
#include "cache/art_absence.h"
#include "cache/images.h"
#include "app/settings.h"
#include "stream/provider.h"
#include "ui/shell_glue.h"

namespace glasswave::ui {

namespace {

std::unique_ptr<ArtCache> g_cache;
uint64_t g_nextGeneration = 0;

// A provider picture that failed to load is asked for again only after a growing pause, and
// one the service confirmed absent after the longest one. Requests come from painting, so
// nothing is retried while the picture is not on screen.
constexpr std::chrono::seconds kRetryBase{30};
constexpr std::chrono::seconds kRetryMax{600};
// Fetches of uncached provider pictures run beside the local lane. The image cache coalesces
// one download per picture, so this bounds waiting threads, not requests per picture.
constexpr int kNetworkThreads = 3;

using core::Utf8;
using core::Wide;

std::string ProviderFor(std::wstring_view sourceKey) {
    const size_t colon = sourceKey.find(L':');
    if (colon == std::wstring_view::npos || colon == 0) return {};
    const std::wstring name(sourceKey.substr(0, colon));
    return stream::FindProvider(name) ? Utf8(name) : std::string();
}

// The key selects whole segments: "apple:album:12" is that album, not "apple:album:123".
bool HasPrefix(std::wstring_view value, std::wstring_view prefix) {
    return cache::ArtKeyHasPrefix(value, prefix);
}

} // namespace

ArtCache& ArtCache::Get() {
    if (!g_cache) g_cache.reset(new ArtCache());
    return *g_cache;
}

ArtCache* ArtCache::Existing() {
    return g_cache.get();
}

void ArtCache::Shutdown() {
    g_cache.reset();
}

std::wstring ArtCache::SourceKey(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return {};
    if (auto* provider = stream::ProviderOf(track)) return provider->ArtKeyOf(track);
    std::wstring sourceKey = pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr();
    sourceKey += L'|' + std::to_wstring(track->get_subsong_index());
    return sourceKey;
}

ArtCache::ArtCache() {
    m_generation = ++g_nextGeneration;
    {
        winrt::com_ptr<IWICImagingFactory> wic;
        winrt::com_ptr<IWICBitmap> bitmap;
        std::array<BYTE, 4> clear{};
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(wic.put()))) &&
            SUCCEEDED(wic->CreateBitmapFromMemory(1, 1, GUID_WICPixelFormat32bppPBGRA, 4,
                                                  static_cast<UINT>(clear.size()), clear.data(),
                                                  bitmap.put())))
            m_resetArt = bitmap;
    }
    const uint64_t generation = m_generation;
    m_imageToken = cache::Images::Get().Subscribe([generation](cache::ImageEvent event) {
        fb2k::inMainThread([generation, event = std::move(event)] {
            if (auto* current = ArtCache::Existing(); current && current->m_generation == generation)
                current->ImageChanged(std::move(event));
        });
    });
    for (int i = 0, n = settings::CoverThreads(); i < n; ++i)
        m_threads.emplace_back([this] { Worker(false); });
    for (int i = 0; i < kNetworkThreads; ++i)
        m_threads.emplace_back([this] { Worker(true); });
}

ArtCache::~ArtCache() {
    if (auto* images = cache::Images::Existing()) images->Unsubscribe(m_imageToken);
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_queue.clear();
        m_network.clear();
        m_done.clear();
    }
    m_abort.abort();
    m_wake.notify_all();
    m_wakeNetwork.notify_all();
    for (auto& thread : m_threads)
        if (thread.joinable()) thread.join();
}

ArtCache::Source& ArtCache::SourceFor(const std::wstring& sourceKey) {
    auto [it, inserted] = m_sources.try_emplace(sourceKey);
    Source& source = it->second;
    if (inserted) {
        source.provider = ProviderFor(sourceKey);
        if (!source.provider.empty()) source.accountRevision = cache::AccountRevision(source.provider);
    } else if (!source.provider.empty()) {
        // Another account's picture is neither shown nor reused, not even while it reloads.
        if (const uint64_t current = cache::AccountRevision(source.provider);
            current != source.accountRevision) {
            source.accountRevision = current;
            m_entryCount -= source.sizes.size();
            source.sizes.clear();
            source.replaceCallerCopies = true;
        }
    }
    source.used = ++m_clock;
    return source;
}

void ArtCache::Queue(const std::wstring& sourceKey, const Source& source, int sizePx, Entry& entry) {
    entry.epoch = ++m_epoch;
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_front(
            {sourceKey, entry.track, sizePx, entry.epoch, source.provider, source.accountRevision});
    }
    m_wake.notify_one();
}

// The old bitmap stays in place while the replacement loads.
void ArtCache::Reload(const std::wstring& sourceKey, const Source& source, int sizePx, Entry& entry) {
    entry.stale = true;
    entry.reloading = true;
    Queue(sourceKey, source, sizePx, entry);
}

ArtCache::State ArtCache::Find(const std::wstring& key, const metadb_handle_ptr& track, int sizePx,
                               winrt::com_ptr<IWICBitmapSource>* out) {
    *out = nullptr;
    if (!track.is_valid()) return State::Missing;

    const std::wstring sourceKey = key.empty() ? SourceKey(track) : key;
    Source& source = SourceFor(sourceKey);

    auto found = source.sizes.find(sizePx);
    if (found == source.sizes.end()) {
        // The same picture is requested at several sizes (grid tile, detail header, list
        // column). Serve a smaller request from a finished larger thumbnail instead of
        // decoding it again; the view scales it into its box. A stale larger thumbnail is
        // still shown at once while this size reloads.
        Entry* best = nullptr;
        for (auto it = source.sizes.lower_bound(sizePx); it != source.sizes.end(); ++it) {
            Entry& candidate = it->second;
            if (candidate.state != State::Ready || !candidate.bitmap || candidate.placeholder) continue;
            if (!best || (best->stale && !candidate.stale)) best = &candidate;
            if (!best->stale) break;
        }
        // Nothing as large yet (a player cover after the list's thumbnails): the largest smaller
        // one stands in, scaled up, while this size loads; then the sharp one replaces it.
        bool interim = false;
        if (!best)
            for (auto it = source.sizes.rbegin(); it != source.sizes.rend(); ++it) {
                Entry& candidate = it->second;
                if (it->first >= sizePx || candidate.state != State::Ready || !candidate.bitmap ||
                    candidate.placeholder)
                    continue;
                best = &candidate;
                interim = true;
                break;
            }
        const bool bestStale = best && (best->stale || interim);
        const auto bestBitmap = best ? best->bitmap : nullptr;
        const uint64_t bestContent = best ? best->content : 0;

        Entry& entry = source.sizes[sizePx];
        ++m_entryCount;
        entry.track = track;
        if (bestBitmap) {
            entry.state = State::Ready;
            entry.bitmap = bestBitmap;
            entry.content = bestContent;
            *out = entry.bitmap;
            if (!bestStale) return State::Ready;
            // A stand-in: still Pending, so callers keep waiting for the sharp picture.
            Reload(sourceKey, source, sizePx, entry);
            return State::Pending;
        }
        if (!source.provider.empty() && cache::art_absence::Fresh(Utf8(sourceKey))) {
            entry.state = State::Missing;
            entry.retryAt = Clock::now() + kRetryMax;
            ++entry.content;
            return State::Missing;
        }
        if (source.replaceCallerCopies && m_resetArt) {
            entry.bitmap = m_resetArt;
            entry.state = State::Ready;
            entry.placeholder = true;
            entry.stale = true;
            entry.reloading = true;
        }
        Queue(sourceKey, source, sizePx, entry);
        *out = entry.bitmap; // the blank placeholder after an account change, if any
        Evict(); // may not drop this source: it was used last and its entry is loading
        return State::Pending;
    }

    Entry& entry = found->second;
    entry.track = track;
    if (entry.stale && !entry.reloading) {
        Reload(sourceKey, source, sizePx, entry);
    } else if (entry.state == State::Missing && !entry.reloading && !source.provider.empty() &&
               Clock::now() >= entry.retryAt) {
        Reload(sourceKey, source, sizePx, entry);
    }
    *out = entry.bitmap;
    // A bitmap shown while this size (re)loads is a stand-in: Pending until the load ends.
    if (entry.reloading || entry.state == State::Pending) return State::Pending;
    if (entry.bitmap) return State::Ready;
    return entry.state == State::Missing ? State::Missing : State::Pending;
}

void ArtCache::Evict() {
    const size_t limit = static_cast<size_t>(settings::ThumbnailCacheEntries());
    if (m_entryCount <= limit) return;
    std::vector<std::pair<uint64_t, const std::wstring*>> old;
    old.reserve(m_sources.size());
    for (const auto& [sourceKey, source] : m_sources) {
        const bool busy = std::any_of(source.sizes.begin(), source.sizes.end(), [](const auto& item) {
            return item.second.state == State::Pending || item.second.reloading;
        });
        if (!busy) old.emplace_back(source.used, &sourceKey);
    }
    std::sort(old.begin(), old.end());
    for (const auto& [used, sourceKey] : old) {
        (void)used;
        if (m_entryCount <= limit * 3 / 4) break;
        const auto it = m_sources.find(*sourceKey);
        m_entryCount -= it->second.sizes.size();
        m_sources.erase(it);
    }
}

int ArtCache::Subscribe(std::function<void()> callback) {
    m_listeners[m_nextToken] = std::move(callback);
    return m_nextToken++;
}

void ArtCache::Unsubscribe(int token) {
    m_listeners.erase(token);
}

void ArtCache::Notify() {
    auto listeners = m_listeners;
    for (auto& [token, callback] : listeners) {
        (void)token;
        callback();
    }
}

void ArtCache::DropQueued(std::wstring_view prefix) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_queue, [&](const Request& request) { return HasPrefix(request.sourceKey, prefix); });
    std::erase_if(m_network, [&](const Request& request) { return HasPrefix(request.sourceKey, prefix); });
    std::erase_if(m_done, [&](const Done& done) { return HasPrefix(done.sourceKey, prefix); });
}

void ArtCache::Invalidate(std::wstring_view prefix) {
    cache::Images::Get().Invalidate(Utf8(prefix));
    DropQueued(prefix);
    for (auto& [sourceKey, source] : m_sources) {
        if (!HasPrefix(sourceKey, prefix)) continue;
        for (auto& [sizePx, entry] : source.sizes) {
            entry.failures = 0;
            entry.retryAt = {};
            ++entry.content;
            if (entry.state == State::Pending) {
                // Its load was dropped from the queue (or will be ignored): start it again.
                Reload(sourceKey, source, sizePx, entry);
            } else {
                // Reloaded by the next paint that still wants it.
                entry.stale = true;
                entry.reloading = false;
                entry.epoch = ++m_epoch;
            }
        }
    }
    Notify();
}

void ArtCache::Forget(std::wstring_view prefix) {
    cache::Images::Get().Invalidate(Utf8(prefix));
    DropQueued(prefix);
    for (auto& [sourceKey, source] : m_sources) {
        if (!HasPrefix(sourceKey, prefix)) continue;
        m_entryCount -= source.sizes.size();
        source.sizes.clear();
        source.replaceCallerCopies = true;
    }
    Notify();
}

void ArtCache::InvalidateMissing(std::wstring_view prefix) {
    std::vector<std::wstring> invalidatedKeys;
    for (auto& [sourceKey, source] : m_sources) {
        if (!HasPrefix(sourceKey, prefix)) continue;
        bool sourceChanged = false;
        for (auto& [sizePx, entry] : source.sizes) {
            (void)sizePx;
            if (entry.state != State::Missing || entry.stale || entry.reloading) continue;
            entry.stale = true;
            entry.failures = 0;
            entry.epoch = ++m_epoch;
            ++entry.content;
            sourceChanged = true;
        }
        if (sourceChanged) invalidatedKeys.push_back(sourceKey);
    }
    if (invalidatedKeys.empty()) return;
    // Invalidate only the specific missing keys on disk so valid cached covers under `prefix`
    // stay fresh instead of being forced into conditional HTTP revalidation.
    for (const auto& key : invalidatedKeys)
        cache::Images::Get().Invalidate(Utf8(key));
    Notify();
}

uint64_t ArtCache::Version(const std::wstring& key, const metadb_handle_ptr& track, int sizePx) const {
    if (!track.is_valid()) return 0;
    const auto source = m_sources.find(key.empty() ? SourceKey(track) : key);
    if (source == m_sources.end()) return 0;
    const auto entry = source->second.sizes.find(sizePx);
    return entry == source->second.sizes.end() ? 0 : entry->second.content;
}

// The persistent image cache replaced this picture's bytes: decode the sizes on screen again.
void ArtCache::ImageChanged(cache::ImageEvent event) {
    const std::wstring artKey = Wide(event.artKey);
    const auto found = m_sources.find(artKey);
    if (found == m_sources.end()) return;
    Source& source = found->second;
    if (source.provider.empty() || source.accountRevision != event.accountRevision ||
        cache::AccountRevision(source.provider) != event.accountRevision)
        return;

    bool changed = false;
    for (auto& [sizePx, entry] : source.sizes) {
        if (entry.reloading || entry.state == State::Pending) continue;
        Reload(artKey, source, sizePx, entry);
        changed = true;
    }
    if (changed) Notify();
}

void ArtCache::Worker(bool network) {
    // WIC objects are created on this thread; the decoded bitmaps own their pixels.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
    auto& queue = network ? m_network : m_queue;
    auto& wake = network ? m_wakeNetwork : m_wake;
    for (;;) {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            wake.wait(lock, [this, &queue] { return m_quit || !queue.empty(); });
            if (m_quit) break;
            request = std::move(queue.front());
            queue.pop_front();
        }

        winrt::com_ptr<IWICBitmapSource> bitmap;
        bool missed = false;
        try {
            std::optional<cache::CacheOnlyScope> cacheOnly;
            if (!request.network) cacheOnly.emplace();
            const cache::SizeHintScope sizeHint(request.sizePx);
            auto api = album_art_manager_v2::get();
            auto extractor = api->open(pfc::list_single_ref_t<metadb_handle_ptr>(request.track),
                                       pfc::list_single_ref_t<GUID>(album_art_ids::cover_front), m_abort);
            album_art_data::ptr data = extractor->query(album_art_ids::cover_front, m_abort);
            if (wic) bitmap = DecodeArt(wic.get(), data, request.sizePx);
        } catch (const exception_aborted&) {
            break;
        } catch (...) {
            // Not found or unreadable: remembered as missing.
        }
        if (!request.network && !bitmap && cache::TakeCacheMiss()) missed = true;
        if (missed) {
            {
                std::lock_guard lock(m_mutex);
                if (m_quit) break;
                request.network = true;
                m_network.push_front(std::move(request));
            }
            m_wakeNetwork.notify_one();
            continue;
        }

        if (!request.provider.empty() && cache::AccountRevision(request.provider) != request.accountRevision)
            bitmap = nullptr;

        bool first = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_quit) break;
            first = m_done.empty();
            m_done.push_back({request.sourceKey, std::move(bitmap), request.epoch, request.provider,
                              request.accountRevision, request.sizePx});
        }
        if (first)
            fb2k::inMainThread([] {
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

    bool changed = false;
    for (auto& item : done) {
        const auto found = m_sources.find(item.sourceKey);
        if (found == m_sources.end()) continue;
        Source& source = found->second;
        if (source.accountRevision != item.accountRevision) continue;
        if (!item.provider.empty() && cache::AccountRevision(item.provider) != item.accountRevision) continue;
        const auto size = source.sizes.find(item.sizePx);
        if (size == source.sizes.end()) continue;
        Entry& entry = size->second;
        if (entry.epoch != item.epoch) continue;

        entry.reloading = false;
        entry.stale = false;
        changed = true;
        if (item.bitmap) {
            entry.bitmap = std::move(item.bitmap);
            entry.state = State::Ready;
            entry.placeholder = false;
            entry.failures = 0;
            source.replaceCallerCopies = false;
            ++entry.content;
            // The same picture's other sizes that have none (failed, or still waiting for a
            // retry) take it at once: the mini player's cover arrived while the library
            // window's smaller one stayed empty (owner, 2026-11-14). A larger size shows it
            // scaled up and still loads its own.
            for (auto& [otherSize, other] : source.sizes) {
                if (&other == &entry || other.bitmap || other.reloading) continue;
                other.bitmap = entry.bitmap;
                other.state = State::Ready;
                other.failures = 0;
                other.stale = otherSize > item.sizePx;
                ++other.content;
            }
            continue;
        }
        const bool absent = !item.provider.empty() && cache::art_absence::Fresh(Utf8(item.sourceKey));
        if (absent || entry.placeholder) {
            // Confirmed to have no cover, or another account's picture: show none.
            entry.bitmap = nullptr;
            entry.placeholder = false;
            entry.state = State::Missing;
            ++entry.content;
        } else if (!entry.bitmap) {
            entry.state = State::Missing;
        }
        // A transient failure keeps any older bitmap in place.
        if (!entry.bitmap) {
            entry.failures = std::min(entry.failures + 1, 8u);
            const auto pause = absent ? kRetryMax : std::min(kRetryMax, kRetryBase * (1u << (entry.failures - 1)));
            entry.retryAt = Clock::now() + pause;
            if (!absent) RemindAt(entry.retryAt);
        }
    }
    if (changed) Notify();
}

// A failed picture is tried again only when a view asks for it after its pause; a view that
// asked once (the now-playing cover) does not ask again by itself, and the cover stayed away
// until a restart (owner, 2026-11-14). So the views are told when the pause ends.
void ArtCache::RemindAt(Clock::time_point when) {
    if (m_reminderArmed && when >= m_reminderAt) return;
    m_reminderArmed = true;
    m_reminderAt = when;
    const auto delay = std::chrono::duration<double>(when - Clock::now()).count() + 0.05;
    fb2k::callLater(std::max(0.05, delay), [] {
        auto* cache = ArtCache::Existing();
        if (!cache) return;
        cache->m_reminderArmed = false;
        cache->Notify();
    });
}

} // namespace glasswave::ui
