#include "pch.h"
#include "ui/art_cache.h"
#include "cache/art_absence.h"
#include "app/settings.h"
#include "stream/provider.h"
#include "ui/shell_glue.h"

namespace glasswave::ui {

namespace {

std::unique_ptr<ArtCache> g_cache;
uint64_t g_nextGeneration = 0;

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_wide_from_utf8(text.data(), text.size()).get_ptr();
}

std::wstring SourceKeyFor(const std::wstring& key, const metadb_handle_ptr& track) {
    if (!key.empty()) return key;
    std::wstring sourceKey = pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr();
    sourceKey += L'|' + std::to_wstring(track->get_subsong_index());
    return sourceKey;
}

std::string ProviderFor(std::wstring_view sourceKey) {
    const size_t colon = sourceKey.find(L':');
    if (colon == std::wstring_view::npos || colon == 0) return {};
    const std::wstring name(sourceKey.substr(0, colon));
    return stream::FindProvider(name) ? Utf8(name) : std::string();
}

uint64_t AccountRevisionFor(const std::string& provider) {
    return provider.empty() ? 0 : cache::AccountRevision(provider);
}

std::wstring MemoryKey(std::wstring_view sourceKey, uint64_t accountRevision, int sizePx) {
    return std::to_wstring(sourceKey.size()) + L':' + std::wstring(sourceKey) + L':' +
           std::to_wstring(accountRevision) + L':' + std::to_wstring(sizePx);
}

bool HasPrefix(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
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
        m_threads.emplace_back([this] { Worker(); });
}

ArtCache::~ArtCache() {
    if (auto* images = cache::Images::Existing()) images->Unsubscribe(m_imageToken);
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_queue.clear();
        m_done.clear();
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

    const std::wstring sourceKey = SourceKeyFor(key, track);
    const std::string provider = ProviderFor(sourceKey);
    const uint64_t accountRevision = AccountRevisionFor(provider);
    bool accountChanged = false;
    if (!provider.empty()) {
        auto [revision, inserted] = m_sourceRevisions.try_emplace(sourceKey, accountRevision);
        if (!inserted && revision->second != accountRevision) {
            revision->second = accountRevision;
            accountChanged = true;
        }
    }
    if (accountChanged) m_replaceCallerCopies.insert(sourceKey);
    const bool resetCachedBitmap = m_replaceCallerCopies.count(sourceKey) != 0;
    if (!provider.empty()) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->second.sourceKey == sourceKey && it->second.accountRevision != accountRevision)
                it = m_entries.erase(it);
            else
                ++it;
        }
    }

    const std::wstring cacheKey = MemoryKey(sourceKey, accountRevision, sizePx);

    // The same picture is requested at several sizes (grid tile, detail header, list
    // column). Reuse a finished larger thumbnail instead of decoding it again; the list
    // scales it into its smaller box. A stale larger thumbnail is still shown at once
    // while this size reloads.
    if (m_entries.find(cacheKey) == m_entries.end()) {
        auto pick = [&](bool allowStale) -> Entry* {
            Entry* found = nullptr;
            for (auto& [candidateKey, candidate] : m_entries) {
                (void)candidateKey;
                if (candidate.sourceKey != sourceKey || candidate.accountRevision != accountRevision ||
                    candidate.state != State::Ready || !candidate.bitmap || candidate.placeholder)
                    continue;
                if (candidate.stale && !allowStale) continue;
                if (candidate.sizePx < sizePx) continue;
                if (!found || candidate.sizePx < found->sizePx) found = &candidate;
            }
            return found;
        };
        Entry* best = pick(false);
        if (!best) best = pick(true);
        if (best) {
            Entry& alias = m_entries[cacheKey];
            alias.state = State::Ready;
            alias.bitmap = best->bitmap;
            alias.used = ++m_clock;
            alias.content = best->content;
            alias.sourceKey = sourceKey;
            alias.accountRevision = accountRevision;
            alias.track = track;
            alias.sizePx = sizePx;
            if (best->stale) {
                alias.stale = true;
                alias.epoch = ++m_epoch;
                alias.reloading = true;
                {
                    std::lock_guard lock(m_mutex);
                    m_queue.push_front({sourceKey, track, sizePx, alias.epoch, provider, accountRevision});
                }
                m_wake.notify_one();
            }
            *out = alias.bitmap;
            return alias.bitmap ? State::Ready : State::Pending;
        }
    }

    auto [it, inserted] = m_entries.try_emplace(cacheKey);
    Entry& entry = it->second;
    entry.sourceKey = sourceKey;
    entry.accountRevision = accountRevision;
    entry.track = track;
    entry.sizePx = sizePx;
    entry.used = ++m_clock;
    if (inserted) {
        entry.epoch = ++m_epoch;
        if (cache::art_absence::Fresh(Utf8(sourceKey))) {
            entry.state = State::Missing;
            ++entry.content;
            return State::Missing;
        }
        if (resetCachedBitmap && m_resetArt) {
            entry.bitmap = m_resetArt;
            entry.state = State::Ready;
            entry.placeholder = true;
            entry.stale = true;
            entry.reloading = true;
        }
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_front({sourceKey, track, sizePx, entry.epoch, provider, accountRevision});
        }
        m_wake.notify_one();

        if (const size_t limit = static_cast<size_t>(settings::ThumbnailCacheEntries());
            m_entries.size() > limit) {
            std::vector<std::pair<uint64_t, std::wstring>> old;
            for (const auto& [entryKey, candidate] : m_entries)
                if (candidate.state != State::Pending && !candidate.reloading)
                    old.emplace_back(candidate.used, entryKey);
            std::sort(old.begin(), old.end());
            for (size_t i = 0; i < old.size() && m_entries.size() > limit * 3 / 4; ++i)
                m_entries.erase(old[i].second);
        }
        if (entry.placeholder) {
            *out = entry.bitmap;
            return State::Ready;
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
                m_queue.push_front({sourceKey, track, sizePx, entry.epoch, provider, accountRevision});
            }
            m_wake.notify_one();
        }
        *out = entry.bitmap;
        return entry.bitmap ? State::Ready : State::Pending;
    }
    if (!provider.empty() && !entry.reloading && cache::ImageRefreshDue(Utf8(sourceKey)) &&
        (entry.state == State::Ready || entry.state == State::Missing)) {
        entry.stale = true;
        entry.epoch = ++m_epoch;
        entry.reloading = true;
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_front({sourceKey, track, sizePx, entry.epoch, provider, accountRevision});
        }
        m_wake.notify_one();
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

void ArtCache::Unsubscribe(int token) {
    m_listeners.erase(token);
}

void ArtCache::Invalidate(std::wstring_view prefix) {
    cache::Images::Get().Invalidate(Utf8(prefix));
    std::vector<Request> restart;
    for (auto& [cacheKey, entry] : m_entries) {
        if (!HasPrefix(entry.sourceKey, prefix)) continue;
        entry.stale = true;
        entry.reloading = false;
        entry.epoch = ++m_epoch;
        ++entry.content;
        if (entry.state == State::Pending) {
            entry.reloading = true;
            restart.push_back({entry.sourceKey, entry.track, entry.sizePx, entry.epoch,
                               ProviderFor(entry.sourceKey), entry.accountRevision});
        }
    }
    {
        std::lock_guard lock(m_mutex);
        auto matches = [prefix](const std::wstring& sourceKey) { return HasPrefix(sourceKey, prefix); };
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                     [&](const Request& request) { return matches(request.sourceKey); }),
                      m_queue.end());
        m_done.erase(std::remove_if(m_done.begin(), m_done.end(),
                                    [&](const Done& done) { return matches(done.sourceKey); }),
                     m_done.end());
        for (auto it = restart.rbegin(); it != restart.rend(); ++it)
            m_queue.push_front(std::move(*it));
    }
    m_wake.notify_all();

    auto listeners = m_listeners;
    for (auto& [token, callback] : listeners)
        callback();
}

void ArtCache::Forget(std::wstring_view prefix) {
    cache::Images::Get().Invalidate(Utf8(prefix));
    for (const auto& [sourceKey, revision] : m_sourceRevisions) {
        (void)revision;
        if (HasPrefix(sourceKey, prefix)) m_replaceCallerCopies.insert(sourceKey);
    }
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (HasPrefix(it->second.sourceKey, prefix))
            it = m_entries.erase(it);
        else
            ++it;
    }
    {
        std::lock_guard lock(m_mutex);
        auto matches = [prefix](const std::wstring& sourceKey) { return HasPrefix(sourceKey, prefix); };
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                     [&](const Request& request) { return matches(request.sourceKey); }),
                      m_queue.end());
        m_done.erase(std::remove_if(m_done.begin(), m_done.end(),
                                    [&](const Done& done) { return matches(done.sourceKey); }),
                     m_done.end());
    }

    auto listeners = m_listeners;
    for (auto& [token, callback] : listeners)
        callback();
}

void ArtCache::InvalidateMissing(std::wstring_view prefix) {
    cache::Images::Get().Invalidate(Utf8(prefix));
    bool changed = false;
    for (auto& [cacheKey, entry] : m_entries) {
        (void)cacheKey;
        if (entry.state != State::Missing || entry.stale || entry.reloading) continue;
        if (!HasPrefix(entry.sourceKey, prefix)) continue;
        entry.stale = true;
        entry.epoch = ++m_epoch;
        ++entry.content;
        changed = true;
    }
    if (!changed) return;
    auto listeners = m_listeners;
    for (auto& [token, callback] : listeners)
        callback();
}

uint64_t ArtCache::Version(const std::wstring& key, const metadb_handle_ptr& track, int sizePx) const {
    if (!track.is_valid()) return 0;
    const std::wstring sourceKey = SourceKeyFor(key, track);
    const uint64_t accountRevision = AccountRevisionFor(ProviderFor(sourceKey));
    const auto it = m_entries.find(MemoryKey(sourceKey, accountRevision, sizePx));
    return it == m_entries.end() ? 0 : it->second.content;
}

void ArtCache::ImageChanged(cache::ImageEvent event) {
    const std::wstring artKey = Wide(event.artKey);
    const std::string provider = ProviderFor(artKey);
    if (provider.empty() || cache::AccountRevision(provider) != event.accountRevision) return;

    bool changed = false;
    for (auto& [cacheKey, entry] : m_entries) {
        (void)cacheKey;
        if (entry.sourceKey != artKey || entry.accountRevision != event.accountRevision) continue;
        if (entry.reloading) continue;
        entry.stale = true;
        entry.epoch = ++m_epoch;
        entry.reloading = true;
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_front(
                {entry.sourceKey, entry.track, entry.sizePx, entry.epoch, provider, event.accountRevision});
        }
        changed = true;
    }
    if (!changed) return;
    m_wake.notify_all();

    auto listeners = m_listeners;
    for (auto& [token, callback] : listeners)
        callback();
}

void ArtCache::Worker() {
    // WIC objects are created on this thread; the decoded bitmaps own their pixels.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
    for (;;) {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_quit || !m_queue.empty(); });
            if (m_quit) break;
            request = std::move(m_queue.front());
            m_queue.pop_front();
        }

        winrt::com_ptr<IWICBitmapSource> bitmap;
        try {
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
        if (!item.provider.empty() && cache::AccountRevision(item.provider) != item.accountRevision) continue;
        const auto it = m_entries.find(MemoryKey(item.sourceKey, item.accountRevision, item.sizePx));
        if (it == m_entries.end()) continue;
        Entry& entry = it->second;
        if (entry.epoch != item.epoch) continue;

        entry.reloading = false;
        entry.stale = false;
        changed = true;
        if (item.bitmap) {
            entry.bitmap = std::move(item.bitmap);
            entry.state = State::Ready;
            entry.placeholder = false;
            m_replaceCallerCopies.erase(item.sourceKey);
            ++entry.content;
        } else if (cache::art_absence::Fresh(Utf8(item.sourceKey)) || entry.placeholder) {
            entry.bitmap = nullptr;
            entry.placeholder = false;
            entry.state = State::Missing;
            ++entry.content;
        } else if (!entry.bitmap) {
            entry.state = State::Missing;
        }
        // A transient failure keeps any older bitmap in place.
    }
    if (!changed) return;

    auto listeners = m_listeners;
    for (auto& [token, callback] : listeners)
        callback();
}

} // namespace glasswave::ui
