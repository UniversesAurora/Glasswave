#include "pch.h"
#include "core/text.h"
#include "core/file.h"

#include "lyrics/lyrics_service.h"
#include "core/paths.h"

#include "stream/provider.h"
#include "lyrics/cache_format.h"
#include "lyrics/scripts.h"
#include "cache/store.h"

#include <filesystem>
#include <fstream>

namespace glasswave::lyrics {

namespace {

constexpr int kSearchTimeoutMs = 20000;            // searchers still running then are abandoned
// A "nothing found" answer is kept this long. Sources report a failed request the same way
// as no lyrics, so the answer is not kept for long; "Search again" always asks at once.
constexpr long long kNotFoundRetrySeconds = 3600;
std::mutex g_cacheMutex;

std::vector<std::shared_ptr<Searcher>>& Registry() {
    static std::vector<std::shared_ptr<Searcher>> searchers;
    return searchers;
}

using core::Utf8;
using core::Wide;

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    return core::ReadFileLimited(path, 4 * 1024 * 1024);
}

bool WriteFile(const std::filesystem::path& path, std::string_view bytes) {
    try {
        core::WriteFileAtomic(path, bytes);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::wstring EmbeddedLyrics(const metadb_handle_ptr& track) {
    std::wstring unsynced;
    if (auto info = track->get_info_ref(); info.is_valid()) {
        for (const char* field : {"SYNCEDLYRICS", "LYRICS", "UNSYNCEDLYRICS", "UNSYNCED LYRICS"}) {
            const char* value = info->info().meta_get(field, 0);
            if (!value || !*value) continue;
            std::wstring text = Wide(value);
            if (ParseLrc(text).synced) return text;
            if (unsynced.empty()) unsynced = std::move(text);
        }
    }
    return unsynced;
}

Query MakeQuery(const metadb_handle_ptr& track) {
    Query q;
    q.path = Wide(track->get_path());
    q.subsong = track->get_subsong_index();
    if (auto* provider = stream::ProviderOf(track)) {
        q.provider = provider->Key();
        q.providerId = provider->TrackId(track);
    }
    if (auto info = track->get_info_ref(); info.is_valid()) {
        const file_info& fi = info->info();
        q.title = Wide(fi.meta_get("title", 0));
        q.artist = Wide(fi.meta_get("artist", 0));
        q.album = Wide(fi.meta_get("album", 0));
        q.duration = fi.get_length();
    }
    if (q.title.empty()) {
        // No tags: the file name.
        std::wstring name = q.path;
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        if (const size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos && dot > 0)
            name.resize(dot);
        q.title = name;
    }
    if (pfc::string8 native; filesystem::g_get_native_path(track->get_path(), native))
        q.nativePath = Wide(native.get_ptr());
    q.embeddedLyrics = EmbeddedLyrics(track);
    return q;
}

std::filesystem::path CacheFile(const Query& q, bool identityOnly = false) {
    // FNV-1a over path and subsong: stable across sessions.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::wstring_view s) {
        for (wchar_t c : s) {
            h ^= static_cast<uint16_t>(c);
            h *= 1099511628211ull;
        }
    };
    mix(q.path);
    mix(L"|");
    mix(std::to_wstring(q.subsong));
    if (!identityOnly) {
        for (const auto& text : {q.title, q.artist, q.album, q.provider, std::to_wstring(q.duration)}) {
            mix(L"|");
            mix(text);
        }
    }
    wchar_t name[32];
    swprintf_s(name, L"%016llx.lrc", static_cast<unsigned long long>(h));
    return std::filesystem::path(Service::CacheFolder()) / name;
}

struct CacheEntry {
    bool notFound = false;
    std::shared_ptr<Lyrics> lyrics;
    std::optional<Candidate> candidate;
    bool none = false; // the owner's selection: no lyrics
};

bool CacheScopeCurrent(std::wstring_view provider, std::wstring_view scope) {
    if (provider.empty() || scope.empty()) return false;
    try {
        return cache::AccountScope(Utf8(provider)) == Utf8(scope);
    } catch (...) {
        return false;
    }
}

struct AccountBinding {
    std::wstring provider;
    std::wstring scope;
    uint64_t revision = 0;
};

std::optional<AccountBinding> BindingFor(const Candidate& candidate) {
    if (candidate.cacheProvider.empty()) return std::nullopt;
    return AccountBinding{candidate.cacheProvider, candidate.cacheScope, candidate.accountRevision};
}

bool AccountBindingCurrent(const AccountBinding& binding) {
    try {
        return cache::AccountRevision(Utf8(binding.provider)) == binding.revision &&
               CacheScopeCurrent(binding.provider, binding.scope);
    } catch (...) {
        return false;
    }
}

bool CandidateCurrent(const Candidate& candidate) {
    const auto binding = BindingFor(candidate);
    return !binding || AccountBindingCurrent(*binding);
}

std::wstring CachedText(const Candidate& candidate) {
    return cache_format::Automatic(candidate.source, candidate.sourceKey, candidate.cacheProvider,
                                   candidate.cacheScope, candidate.lrc);
}

std::wstring ManualSelectionText(const Candidate& candidate) {
    return cache_format::Manual(candidate.source, candidate.lrc, candidate.sourceKey, candidate.file,
                                candidate.codePage);
}

std::filesystem::path SelectedFile(const Query& q) {
    auto path = CacheFile(q, true);
    path.replace_extension(L".selected.lrc");
    return path;
}

std::optional<CacheEntry> ReadCache(const Query& q, bool selected = false) {
    auto bytes = ReadFile(selected ? SelectedFile(q) : CacheFile(q));
    if (!bytes) return std::nullopt;
    std::wstring text = DecodeText(*bytes);
    const cache_format::Headers headers = cache_format::Parse(text);
    if (!selected) {
        if (headers.provider.empty() != headers.scope.empty()) return std::nullopt;
        if (!headers.provider.empty() && !CacheScopeCurrent(headers.provider, headers.scope))
            return std::nullopt;
    }
    if (selected && headers.none) return CacheEntry{true, nullptr, std::nullopt, true};
    if (headers.notFoundAt) {
        const long long when = *headers.notFoundAt;
        if (_time64(nullptr) - when < kNotFoundRetrySeconds) return CacheEntry{true, nullptr, std::nullopt};
        return std::nullopt;
    }
    const std::wstring body = text.substr(headers.body);
    auto lyrics = std::make_shared<Lyrics>(ParseLrc(body));
    if (lyrics->lines.empty()) return std::nullopt;
    lyrics->source = headers.source;
    lyrics->sourceKey = headers.sourceKey;
    lyrics->document = body;
    lyrics->file = headers.file;
    lyrics->codePage = headers.codePage;
    Candidate candidate;
    candidate.title = lyrics->title;
    candidate.artist = lyrics->artist;
    candidate.album = lyrics->album;
    candidate.source = headers.source;
    candidate.sourceKey = headers.sourceKey;
    candidate.lrc = body;
    candidate.file = headers.file;
    candidate.codePage = headers.codePage;
    if (!headers.provider.empty()) {
        candidate.cacheProvider = headers.provider;
        candidate.cacheScope = headers.scope;
        candidate.accountRevision = cache::AccountRevision(Utf8(headers.provider));
    }
    return CacheEntry{false, std::move(lyrics), std::move(candidate)};
}

// ---- Matching --------------------------------------------------------------------------

std::wstring Normalize(std::wstring_view s) {
    std::wstring out;
    int depth = 0;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'[' || c == L'（' || c == L'【' || c == L'{') {
            ++depth;
            continue;
        }
        if (c == L')' || c == L']' || c == L'）' || c == L'】' || c == L'}') {
            depth = std::max(0, depth - 1);
            continue;
        }
        if (depth > 0) continue;
        if (iswspace(c) || iswpunct(c) || c == L'・' || c == L'·' || c == L'、' || c == L'，' || c == L'。' ||
            c == L'！' || c == L'？' || c == L'～' || c == L'“' || c == L'”' || c == L'‘' || c == L'’')
            continue;
        out.push_back(static_cast<wchar_t>(towlower(c)));
    }
    // A title that is all brackets ("(Intro)") keeps its text.
    if (out.empty())
        for (wchar_t c : s)
            if (!iswspace(c) && !iswpunct(c)) out.push_back(static_cast<wchar_t>(towlower(c)));
    return out;
}

double Similarity(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) return 0;
    if (a == b) return 1;
    if (a.find(b) != std::wstring_view::npos || b.find(a) != std::wstring_view::npos)
        return 0.75 + 0.25 * static_cast<double>(std::min(a.size(), b.size())) / std::max(a.size(), b.size());
    // Levenshtein ratio (titles are short).
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return 1.0 - static_cast<double>(prev[b.size()]) / std::max(a.size(), b.size());
}

std::vector<std::wstring> SplitArtists(std::wstring_view s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        if (auto n = Normalize(cur); !n.empty()) out.push_back(std::move(n));
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'/' || c == L',' || c == L'&' || c == L'、' || c == L';' || c == L'，' || c == L'×') {
            flush();
            continue;
        }
        // " feat. " / " ft. " / " x "
        if (c == L' ') {
            const std::wstring_view rest = s.substr(i + 1);
            if (rest.rfind(L"feat.", 0) == 0 || rest.rfind(L"ft.", 0) == 0 || rest.rfind(L"x ", 0) == 0 ||
                rest.rfind(L"feat ", 0) == 0) {
                flush();
                i += rest.find(L' ') == std::wstring_view::npos ? rest.size() : rest.find(L' ');
                continue;
            }
        }
        cur.push_back(c);
    }
    flush();
    return out;
}

double ArtistSimilarity(std::wstring_view query, std::wstring_view found) {
    const auto a = SplitArtists(query), b = SplitArtists(found);
    if (a.empty() || b.empty()) return 0.5; // unknown: neither helps nor hurts much
    double best = 0;
    for (const auto& x : a)
        for (const auto& y : b)
            best = std::max(best, Similarity(x, y));
    return best;
}

} // namespace

const Candidate* BestCandidate(const Query& query, const std::vector<Candidate>& candidates) {
    const std::wstring title = Normalize(query.title);
    const Candidate* best = nullptr;
    double bestScore = 0;
    for (const auto& c : candidates) {
        const Lyrics parsed = ParseLrc(c.lrc);
        if (parsed.lines.empty()) continue;
        const double titleSim = Similarity(title, Normalize(c.title.empty() ? parsed.title : c.title));
        const double artistSim = ArtistSimilarity(query.artist, c.artist.empty() ? parsed.artist : c.artist);
        if (titleSim < 0.5 && c.confidence < 70) continue;
        double score = titleSim * 55 + artistSim * 30;
        if (parsed.synced) score += 5;
        if (parsed.karaoke) score += 6;
        if (std::any_of(parsed.lines.begin(), parsed.lines.end(),
                        [](const Line& l) { return !l.translation.empty(); }))
            score += 4;
        // Lyrics that run far past the song are for another version.
        if (query.duration > 0 && parsed.synced && parsed.lines.back().start / 1000.0 > query.duration + 10)
            score -= 30;
        if (c.confidence >= 0) score = score * 0.5 + c.confidence * 0.5;
        if (score >= 50 && score > bestScore) {
            bestScore = score;
            best = &c;
        }
    }
    return best;
}

void RegisterSearcher(std::shared_ptr<Searcher> searcher) {
    if (searcher) Registry().push_back(std::move(searcher));
}

void UnregisterSearcher(const Searcher* searcher) {
    auto& r = Registry();
    r.erase(std::remove_if(r.begin(), r.end(), [&](const auto& s) { return s.get() == searcher; }), r.end());
}

std::vector<std::shared_ptr<Searcher>> Searchers() {
    return Registry();
}

Service& Service::Get() {
    static Service service;
    return service;
}

std::wstring Service::CacheFolder() {
    return core::ProfileDir(L"lyrics").wstring();
}

Query Service::QueryFor(const metadb_handle_ptr& track) {
    return track.is_valid() ? MakeQuery(track) : Query{};
}

std::wstring Service::TrackIdentity(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return {};
    if (auto* provider = stream::ProviderOf(track)) {
        const auto id = provider->TrackId(track);
        if (!id.empty())
            return L"provider\037" + std::wstring(provider->Key()) + L"\037" + id + L"\037" +
                   std::to_wstring(track->get_subsong_index());
    }
    return L"path\037" + Wide(track->get_path()) + L"\037" + std::to_wstring(track->get_subsong_index());
}

void Service::Run(std::shared_ptr<abort_callback_impl> abort, std::function<void()> work) {
    std::erase_if(m_workers, [](Worker& worker) {
        if (!worker.finished->load()) return false;
        worker.thread.join();
        return true;
    });
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread thread([work = std::move(work), finished] {
        try {
            work();
        } catch (const std::exception& error) {
            console::printf("Glasswave lyrics: %s", error.what());
        }
        finished->store(true);
    });
    m_workers.push_back({std::move(thread), std::move(abort), std::move(finished)});
}

void Service::Request(const metadb_handle_ptr& track, std::function<void(const Result&)> done) {
    Start(track, true, std::move(done));
}

void Service::Research(const metadb_handle_ptr& track, std::function<void(const Result&)> done) {
    if (track.is_valid()) {
        std::lock_guard lock(g_cacheMutex);
        std::error_code ec;
        std::filesystem::remove(CacheFile(MakeQuery(track)), ec);
    }
    // The new answer changes what the track shows (copies such as downloaded files follow).
    Start(track, false, [this, track, done = std::move(done)](const Result& result) {
        done(result);
        if (result.status == Status::Found || result.status == Status::NotFound) NotifyChanged(track);
    });
}

uint64_t Service::SubscribeChanges(std::function<void(const metadb_handle_ptr&)> listener) {
    return m_changeListeners.Add(std::move(listener));
}

void Service::UnsubscribeChanges(uint64_t token) {
    m_changeListeners.Remove(token);
}

void Service::NotifyChanged(const metadb_handle_ptr& track) {
    m_changeListeners.Emit(track);
}

void Service::CancelSearch() {
    ++m_searchGeneration;
    if (m_searchAbort) m_searchAbort->abort();
    m_searchAbort.reset();
}

void Service::Cancel() {
    ++m_generation;
    m_activeDone.reset();
    m_activeIdentity.clear();
    if (m_abort) m_abort->abort();
    m_abort.reset();
    CancelSearch();
}

void Service::Shutdown() {
    Cancel();
    for (auto& worker : m_workers)
        worker.abort->abort();
    for (auto& worker : m_workers)
        if (worker.thread.joinable()) worker.thread.join();
    m_workers.clear();
}

namespace {
Result FromCandidate(const Candidate& candidate) {
    auto lyrics = std::make_shared<Lyrics>(ParseLrc(candidate.lrc));
    if (lyrics->lines.empty()) return {Status::NotFound, nullptr};
    lyrics->source = candidate.source;
    lyrics->sourceKey = candidate.sourceKey;
    lyrics->document = candidate.lrc;
    lyrics->file = candidate.file;
    lyrics->codePage = candidate.codePage;
    return {Status::Found, std::move(lyrics)};
}

// Path and subsong: whose candidates Service keeps.
std::wstring CandidatesKey(const Query& query) {
    return query.path + L"\037" + std::to_wstring(query.subsong);
}

// Search concurrently, but automatic selection respects source order. A slow fallback
// cannot delay a successful preferred source. All children are joined before returning.
// Returns whether every source answered (false: stopped early, cancelled or timed out).
bool SearchAll(const Query& query, const std::vector<std::shared_ptr<Searcher>>& searchers,
               std::optional<Candidate> cached, abort_callback& abort, bool automatic,
               const std::function<void(std::vector<Candidate>, bool)>& update) {
    bool complete = false;
    struct Slot {
        bool finished = false;
        std::vector<Candidate> candidates;
    };
    std::vector<Slot> slots(searchers.size());
    if (cached) {
        for (size_t i = 0; i < searchers.size(); ++i)
            if (searchers[i]->Cacheable() && cached->sourceKey == searchers[i]->Key())
                slots[i].candidates.push_back(*cached);
    }
    std::mutex mutex;
    std::condition_variable changed;
    abort_callback_impl childrenAbort;
    std::vector<std::thread> children;
    for (size_t i = 0; i < searchers.size(); ++i) {
        children.emplace_back([&, i, cached] {
            std::vector<Candidate> candidates;
            std::mutex candidateMutex;
            try {
                searchers[i]->Search(query, childrenAbort, [&](Candidate candidate) {
                    if (childrenAbort.is_aborting()) return;
                    if (candidate.source.empty()) candidate.source = searchers[i]->Name();
                    if (candidate.sourceKey.empty()) candidate.sourceKey = searchers[i]->Key();
                    if (CandidateCurrent(candidate) && !ParseLrc(candidate.lrc).lines.empty()) {
                        std::lock_guard candidateLock(candidateMutex);
                        candidates.push_back(std::move(candidate));
                    }
                });
            } catch (const std::exception& error) {
                console::printf("Glasswave lyrics: %s", error.what());
            }
            std::lock_guard lock(mutex);
            if (!candidates.empty() || slots[i].candidates.empty())
                slots[i].candidates = std::move(candidates);
            slots[i].finished = true;
            changed.notify_all();
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSearchTimeoutMs);
    std::unique_lock lock(mutex);
    size_t reported = SIZE_MAX;
    while (!abort.is_aborting()) {
        const size_t finished = static_cast<size_t>(
            std::count_if(slots.begin(), slots.end(), [](const auto& slot) { return slot.finished; }));
        std::vector<Candidate> all;
        for (const auto& slot : slots)
            all.insert(all.end(), slot.candidates.begin(), slot.candidates.end());
        // Automatic: the chosen candidate first, then the others found so far (Select lyrics).
        const auto withBest = [&all](const Candidate& best) {
            std::vector<Candidate> out{best};
            for (const auto& c : all)
                if (c.lrc != best.lrc || c.sourceKey != best.sourceKey) out.push_back(c);
            return out;
        };
        if (automatic) {
            for (const auto& slot : slots) {
                if (!slot.finished) break;
                if (const Candidate* best = BestCandidate(query, slot.candidates)) {
                    update(withBest(*best), true);
                    childrenAbort.abort();
                    break;
                }
            }
            if (childrenAbort.is_aborting()) break;
        } else if (finished != reported) {
            update(all, finished == slots.size());
            reported = finished;
        }
        if (finished == slots.size() || std::chrono::steady_clock::now() >= deadline) {
            complete = finished == slots.size();
            if (automatic) {
                // At timeout, prefer the earliest source that has produced a valid match.
                const Candidate* best = nullptr;
                for (const auto& slot : slots)
                    if ((best = BestCandidate(query, slot.candidates))) break;
                update(best ? withBest(*best) : std::vector<Candidate>{}, true);
            } else if (finished != slots.size())
                update(std::move(all), true);
            break;
        }
        changed.wait_for(lock, std::chrono::milliseconds(30));
    }
    childrenAbort.abort();
    lock.unlock();
    for (auto& child : children)
        child.join();
    return complete;
}
} // namespace

void Service::Start(const metadb_handle_ptr& track, bool useAutomaticCache,
                    std::function<void(const Result&)> done) {
    if (!track.is_valid()) {
        Cancel();
        done({Status::NotFound, nullptr});
        return;
    }
    // A CD track has no name to search by ("CD Track 01") until its disc is tagged
    // (cd/musicbrainz.h); the tag update asks again (on_playback_edited). Nothing is cached
    // for it meanwhile.
    if (_strnicmp(track->get_path(), "cdda://", 7) == 0) {
        const auto info = track->get_info_ref();
        if (!info.is_valid() || !info->info().meta_get("ALBUM", 0)) {
            Cancel();
            done({Status::NotFound, nullptr});
            return;
        }
    }
    const Query query = MakeQuery(track);
    const std::wstring identity = TrackIdentity(track);

    // The same track asked for again while its search runs (a metadata update repeats the
    // track-changed event) joins that search instead of cancelling it and asking every
    // source once more.
    if (useAutomaticCache && m_activeDone && identity == m_activeIdentity) {
        *m_activeDone = done;
        done({Status::Searching, nullptr});
        return;
    }
    Cancel();

    // What is already known is answered at once, without a search and without passing
    // through "searching": the owner's own selection, the lyrics found before, or the
    // recent answer that no source has any. Searching again is the owner's explicit command.
    {
        std::optional<CacheEntry> selected, automatic;
        {
            std::lock_guard lock(g_cacheMutex);
            if (cache_format::ManualSelectionApplies(useAutomaticCache)) selected = ReadCache(query, true);
            if (useAutomaticCache && !(selected && !selected->notFound)) automatic = ReadCache(query);
        }
        if (selected && selected->none) {
            done({Status::NotFound, nullptr});
            return;
        }
        if (selected && !selected->notFound) {
            done({Status::Found, selected->lyrics});
            return;
        }
        if (automatic && automatic->notFound) {
            done({Status::NotFound, nullptr});
            return;
        }
        if (automatic && automatic->candidate && CandidateCurrent(*automatic->candidate)) {
            Remember(query, {*automatic->candidate});
            done({Status::Found, automatic->lyrics});
            return;
        }
    }

    const auto searchers = scripts::Ordered(query.provider);
    const unsigned generation = m_generation;
    auto abort = std::make_shared<abort_callback_impl>();
    m_abort = abort;
    auto slot = std::make_shared<std::function<void(const Result&)>>(done);
    m_activeDone = slot;
    m_activeIdentity = identity;
    auto deliver = [this, generation, slot](Result result, std::optional<AccountBinding> binding) {
        fb2k::inMainThread(
            [this, generation, slot, result = std::move(result), binding = std::move(binding)] {
                if (generation != m_generation) return;
                if (m_activeDone == slot) m_activeDone.reset();
                if (binding && !AccountBindingCurrent(*binding)) return;
                (*slot)(result);
            });
    };
    auto deliverNeutral = [deliver](Result result) { deliver(std::move(result), std::nullopt); };
    auto remember = [this, generation, query](std::vector<Candidate> candidates) {
        fb2k::inMainThread([this, generation, query, candidates = std::move(candidates)] {
            if (generation == m_generation) Remember(query, candidates);
        });
    };
    done({Status::Searching, nullptr});
    Run(abort, [=] {
        try {
            bool nothing = false;
            const bool complete = SearchAll(
                query, searchers, std::nullopt, *abort, true, [&](std::vector<Candidate> candidates, bool) {
                          if (abort->is_aborting()) return;
                          if (candidates.empty()) {
                              nothing = true;
                              deliverNeutral({Status::NotFound, nullptr});
                              return;
                          }
                          remember(candidates);
                          const auto& best = candidates.front();
                          if (!CandidateCurrent(best)) return;
                          const auto source =
                              std::find_if(searchers.begin(), searchers.end(), [&](const auto& searcher) {
                                  return searcher->Key() == best.sourceKey && searcher->Cacheable();
                              });
                          {
                              std::lock_guard lock(g_cacheMutex);
                              if (abort->is_aborting() || !CandidateCurrent(best)) return;
                              if (source != searchers.end())
                                  WriteFile(CacheFile(query), Utf8(CachedText(best)));
                          }
                          if (!CandidateCurrent(best)) return;
                          deliver(FromCandidate(best), BindingFor(best));
                      });
            // Every source answered and none has lyrics: remember that for a while, so the
            // track does not set off the same fruitless searches each time it plays. A search
            // that timed out is not remembered.
            if (complete && nothing && !abort->is_aborting()) {
                std::lock_guard lock(g_cacheMutex);
                WriteFile(CacheFile(query), Utf8(cache_format::kNotFoundTag + std::to_wstring(_time64(nullptr)) + L"]\n"));
            }
        } catch (const exception_aborted&) {
        } catch (const std::exception& error) {
            console::printf("Glasswave lyrics: %s", error.what());
            if (!abort->is_aborting()) deliverNeutral({Status::NotFound, nullptr});
        }
    });
}

void Service::Lookup(const metadb_handle_ptr& track, std::function<void(const Result&)> done) {
    if (!track.is_valid()) {
        done({Status::NotFound, nullptr});
        return;
    }
    const Query query = MakeQuery(track);
    {
        std::optional<CacheEntry> selected, automatic;
        {
            std::lock_guard lock(g_cacheMutex);
            selected = ReadCache(query, true);
            if (!(selected && !selected->notFound)) automatic = ReadCache(query);
        }
        if (selected && selected->none) return done({Status::NotFound, nullptr});
        if (selected && !selected->notFound) return done({Status::Found, selected->lyrics});
        if (automatic && automatic->notFound) return done({Status::NotFound, nullptr});
        if (automatic && automatic->candidate && CandidateCurrent(*automatic->candidate))
            return done({Status::Found, automatic->lyrics});
    }
    // Nothing known: the automatic search, as Start runs it, answered once at its end.
    const auto searchers = scripts::Ordered(query.provider);
    auto abort = std::make_shared<abort_callback_impl>();
    Run(abort, [query, searchers, abort, done = std::move(done)] {
        Result best{Status::NotFound, nullptr};
        try {
            bool nothing = false;
            const bool complete = SearchAll(
                query, searchers, std::nullopt, *abort, true, [&](std::vector<Candidate> candidates, bool) {
                    if (candidates.empty()) {
                        nothing = true;
                        return;
                    }
                    const auto& top = candidates.front();
                    if (!CandidateCurrent(top)) return;
                    const auto source = std::find_if(searchers.begin(), searchers.end(), [&](const auto& searcher) {
                        return searcher->Key() == top.sourceKey && searcher->Cacheable();
                    });
                    if (source != searchers.end()) {
                        std::lock_guard lock(g_cacheMutex);
                        WriteFile(CacheFile(query), Utf8(CachedText(top)));
                    }
                    best = FromCandidate(top);
                });
            if (complete && nothing && best.status != Status::Found) {
                std::lock_guard lock(g_cacheMutex);
                WriteFile(CacheFile(query),
                          Utf8(cache_format::kNotFoundTag + std::to_wstring(_time64(nullptr)) + L"]\n"));
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& error) {
            console::printf("Glasswave lyrics: %s", error.what());
        }
        fb2k::inMainThread([done, best] { done(best); });
    });
}

void Service::FindCandidates(Query query, std::wstring source,
                             std::function<void(std::vector<Candidate>, bool)> done) {
    CancelSearch();
    const unsigned generation = m_searchGeneration;
    auto abort = std::make_shared<abort_callback_impl>();
    m_searchAbort = abort;
    auto searchers = scripts::Ordered(query.provider);
    if (!source.empty())
        std::erase_if(searchers, [&](const auto& searcher) { return searcher->Key() != source; });
    Run(abort, [this, query = std::move(query), searchers = std::move(searchers), abort, done, generation] {
        SearchAll(query, searchers, std::nullopt, *abort, false,
                  [&](std::vector<Candidate> candidates, bool finished) {
                      fb2k::inMainThread(
                          [this, generation, done, query, candidates = std::move(candidates), finished] {
                              if (generation != m_searchGeneration) return;
                              Remember(query, candidates);
                              done(candidates, finished);
                          });
                  });
    });
}

void Service::Remember(const Query& query, const std::vector<Candidate>& candidates) {
    const auto key = CandidatesKey(query);
    if (key != m_candidatesFor) {
        m_candidatesFor = key;
        m_candidates.clear();
    }
    for (const auto& c : candidates) {
        const bool known = std::any_of(m_candidates.begin(), m_candidates.end(), [&](const Candidate& k) {
            return k.lrc == c.lrc && k.sourceKey == c.sourceKey;
        });
        if (!known) m_candidates.push_back(c);
    }
}

std::vector<Candidate> Service::Candidates(const metadb_handle_ptr& track) const {
    if (!track.is_valid()) return {};
    Query key;
    key.path = Wide(track->get_path());
    key.subsong = track->get_subsong_index();
    return CandidatesKey(key) == m_candidatesFor ? m_candidates : std::vector<Candidate>{};
}

namespace {
// An offset belongs to one lyric of one track: another lyric for the same song (another source,
// another version) is timed differently (owner, 2026-11-14). The lyric is named by its lines as
// parsed - times, words and references, whatever file or cache they were read from - and the
// file name needs a fixed-size identity of that text, hence FNV-1a.
std::filesystem::path OffsetFile(const metadb_handle_ptr& track, const Lyrics& lyrics) {
    Query q;
    q.path = Wide(track->get_path());
    q.subsong = track->get_subsong_index();
    const std::wstring trackName = CacheFile(q, true).stem().wstring();
    const std::wstring text = WriteLrc(lyrics, WriteOptions{false, true, false, true, false});
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : text) {
        h ^= static_cast<uint16_t>(c);
        h *= 1099511628211ull;
    }
    wchar_t name[64];
    swprintf_s(name, L"%s-%016llx.offset", trackName.c_str(), static_cast<unsigned long long>(h));
    return std::filesystem::path(Service::CacheFolder()) / name;
}
}  // namespace

int Service::Offset(const metadb_handle_ptr& track, const Lyrics& lyrics) const {
    if (!track.is_valid() || !lyrics.synced) return 0;
    const auto bytes = ReadFile(OffsetFile(track, lyrics));
    return bytes ? std::atoi(bytes->c_str()) : 0;
}

void Service::SetOffset(const metadb_handle_ptr& track, const Lyrics& lyrics, int offsetMs) {
    if (!track.is_valid() || !lyrics.synced) return;
    std::error_code ec;
    if (offsetMs == 0)
        std::filesystem::remove(OffsetFile(track, lyrics), ec);
    else if (!WriteFile(OffsetFile(track, lyrics), std::to_string(offsetMs)))
        console::print("Glasswave lyrics: unable to save the lyrics offset");
    NotifyChanged(track);
}

std::wstring Service::ProfileLyricsFolder() {
    pfc::string8 native;
    if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return {};
    return (std::filesystem::path(Wide(native.get_ptr())) / L"lyrics").wstring();
}

Result Service::Select(const metadb_handle_ptr& track, const Candidate& candidate) {
    if (!track.is_valid()) return {Status::NotFound, nullptr};
    const auto result = FromCandidate(candidate);
    if (result.status != Status::Found) return result;
    Cancel();
    {
        std::lock_guard lock(g_cacheMutex);
        if (!WriteFile(SelectedFile(MakeQuery(track)), Utf8(ManualSelectionText(candidate))))
            console::print("Glasswave lyrics: unable to save lyric selection");
    }
    NotifyChanged(track);
    return result;
}

Result Service::LoadFile(const metadb_handle_ptr& track, const std::wstring& path, unsigned codePage) {
    auto bytes = ReadFile(path);
    if (!bytes || bytes->size() > 4 * 1024 * 1024) return {Status::NotFound, nullptr};
    Candidate candidate;
    candidate.source = L"Local file";
    candidate.lrc = DecodeText(*bytes, codePage);
    candidate.file = path;
    candidate.codePage = codePage;
    return Select(track, candidate);
}

Result Service::Reencode(const metadb_handle_ptr& track, const Lyrics& shown, unsigned codePage) {
    if (shown.file.empty()) return {Status::NotFound, nullptr};
    auto bytes = ReadFile(shown.file);
    if (!bytes) return {Status::NotFound, nullptr};
    Candidate candidate;
    candidate.source = shown.source.empty() ? L"Local file" : shown.source;
    candidate.sourceKey = shown.sourceKey;
    candidate.lrc = DecodeText(*bytes, codePage);
    candidate.file = shown.file;
    candidate.codePage = codePage;
    return Select(track, candidate);
}

Result Service::SelectNone(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return {Status::NotFound, nullptr};
    Cancel();
    {
        std::lock_guard lock(g_cacheMutex);
        if (!WriteFile(SelectedFile(MakeQuery(track)), Utf8(cache_format::ManualNone())))
            console::print("Glasswave lyrics: unable to save lyric selection");
    }
    NotifyChanged(track);
    return {Status::NotFound, nullptr};
}

void Service::UseAutomatic(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return;
    Cancel();
    {
        std::lock_guard lock(g_cacheMutex);
        std::error_code ec;
        std::filesystem::remove(SelectedFile(MakeQuery(track)), ec);
    }
    NotifyChanged(track);
}

void Service::ClearCache() {
    Cancel();
    std::lock_guard lock(g_cacheMutex);
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(CacheFolder(), ec)) {
        const auto name = file.path().filename().wstring();
        if (file.is_regular_file(ec) && name.ends_with(L".lrc") && !name.ends_with(L".selected.lrc"))
            std::filesystem::remove(file.path(), ec);
    }
}

} // namespace glasswave::lyrics
