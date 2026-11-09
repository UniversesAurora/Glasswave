#include "pch.h"

#include "lyrics/lyrics_service.h"

#include "stream/provider.h"

#include <filesystem>
#include <fstream>

namespace glasswave::lyrics {

namespace {

constexpr int kSearchTimeoutMs = 20000;           // searchers still running then are abandoned
constexpr long long kNotFoundRetrySeconds = 86400;  // a "nothing found" answer is kept a day
constexpr wchar_t kSourceTag[] = L"[glasswave-source:";
constexpr wchar_t kNotFoundTag[] = L"[glasswave-notfound:";

std::vector<std::shared_ptr<Searcher>>& Registry() {
    static std::vector<std::shared_ptr<Searcher>> searchers;
    return searchers;
}

std::wstring Wide(const char* s) { return s ? std::wstring(pfc::stringcvt::string_wide_from_utf8(s).get_ptr()) : L""; }

std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool WriteFile(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto part = std::filesystem::path(path).concat(L".part");
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::filesystem::rename(part, path, ec);
    return !ec;
}

Query MakeQuery(const metadb_handle_ptr& track) {
    Query q;
    q.path = Wide(track->get_path());
    q.subsong = track->get_subsong_index();
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
        if (const size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos && dot > 0) name.resize(dot);
        q.title = name;
    }
    if (pfc::string8 native; filesystem::g_get_native_path(track->get_path(), native))
        q.nativePath = Wide(native.get_ptr());
    return q;
}

std::filesystem::path CacheFile(const Query& q) {
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
    wchar_t name[32];
    swprintf_s(name, L"%016llx.lrc", static_cast<unsigned long long>(h));
    return std::filesystem::path(Service::CacheFolder()) / name;
}

// The track's own lyrics: tags first, then <file>.lrc.
std::optional<std::wstring> LocalLyrics(const metadb_handle_ptr& track, const Query& q) {
    if (auto info = track->get_info_ref(); info.is_valid()) {
        std::optional<std::wstring> unsynced;
        for (const char* field : {"SYNCEDLYRICS", "LYRICS", "UNSYNCEDLYRICS", "UNSYNCED LYRICS"}) {
            const char* v = info->info().meta_get(field, 0);
            if (!v || !*v) continue;
            std::wstring text = Wide(v);
            if (text.find(L'[') != std::wstring::npos) return text;
            if (!unsynced) unsynced = std::move(text);
        }
        // An unsynced tag only wins if there is no .lrc file below.
        if (unsynced && q.nativePath.empty()) return unsynced;
        if (!q.nativePath.empty()) {
            std::filesystem::path lrc(q.nativePath);
            lrc.replace_extension(L".lrc");
            if (auto bytes = ReadFile(lrc)) return DecodeText(*bytes);
        }
        return unsynced;
    }
    return std::nullopt;
}

struct CacheEntry {
    bool notFound = false;
    std::shared_ptr<Lyrics> lyrics;
};

std::optional<CacheEntry> ReadCache(const Query& q) {
    auto bytes = ReadFile(CacheFile(q));
    if (!bytes) return std::nullopt;
    std::wstring text = DecodeText(*bytes);
    if (text.rfind(kNotFoundTag, 0) == 0) {
        const long long when = _wtoi64(text.c_str() + wcslen(kNotFoundTag));
        if (_time64(nullptr) - when < kNotFoundRetrySeconds) return CacheEntry{true, nullptr};
        return std::nullopt;
    }
    std::wstring source;
    if (text.rfind(kSourceTag, 0) == 0) {
        const size_t close = text.find(L']');
        if (close != std::wstring::npos) source = text.substr(wcslen(kSourceTag), close - wcslen(kSourceTag));
    }
    auto lyrics = std::make_shared<Lyrics>(ParseLrc(text));
    if (lyrics->lines.empty()) return std::nullopt;
    lyrics->source = source;
    return CacheEntry{false, std::move(lyrics)};
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
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
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
    if (a.empty() || b.empty()) return 0.5;  // unknown: neither helps nor hurts much
    double best = 0;
    for (const auto& x : a)
        for (const auto& y : b) best = std::max(best, Similarity(x, y));
    return best;
}

}  // namespace

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
        if (std::any_of(parsed.lines.begin(), parsed.lines.end(), [](const Line& l) { return !l.translation.empty(); }))
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

std::vector<std::shared_ptr<Searcher>> Searchers() { return Registry(); }

Service& Service::Get() {
    static Service service;
    return service;
}

std::wstring Service::CacheFolder() {
    pfc::string8 native;
    if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return {};
    return (std::filesystem::path(Wide(native.get_ptr())) / L"glasswave" / L"lyrics").wstring();
}

void Service::Request(const metadb_handle_ptr& track, std::function<void(const Result&)> done) {
    Start(track, true, std::move(done));
}

void Service::Research(const metadb_handle_ptr& track, std::function<void(const Result&)> done) {
    if (track.is_valid()) {
        std::error_code ec;
        std::filesystem::remove(CacheFile(MakeQuery(track)), ec);
    }
    Start(track, false, std::move(done));
}

void Service::Cancel() {
    ++m_generation;
    if (m_abort) m_abort->abort();
    m_abort.reset();
}

void Service::Shutdown() { Cancel(); }

void Service::Start(const metadb_handle_ptr& track, bool useCache, std::function<void(const Result&)> done) {
    Cancel();
    if (!track.is_valid()) {
        done(Result{Status::NotFound, nullptr});
        return;
    }
    const unsigned generation = m_generation;
    auto abort = std::make_shared<abort_callback_impl>();
    m_abort = abort;
    auto deliver = [this, generation, done](Result result) {
        fb2k::inMainThread([this, generation, done, result = std::move(result)] {
            if (generation == m_generation) done(result);
        });
    };
    done(Result{Status::Searching, nullptr});

    auto searchers = Registry();
    std::thread([track, useCache, abort, deliver, searchers = std::move(searchers)] {
        try {
            const Query query = MakeQuery(track);
            if (useCache) {
                if (auto cached = ReadCache(query)) {
                    if (cached->notFound) {
                        // Local lyrics may have been added since.
                        if (auto local = LocalLyrics(track, query)) {
                            auto lyrics = std::make_shared<Lyrics>(ParseLrc(*local));
                            if (!lyrics->lines.empty()) {
                                lyrics->source = L"Local";
                                deliver({Status::Found, std::move(lyrics)});
                                return;
                            }
                        }
                        deliver({Status::NotFound, nullptr});
                    } else {
                        deliver({Status::Found, std::move(cached->lyrics)});
                    }
                    return;
                }
            }
            if (auto local = LocalLyrics(track, query)) {
                auto lyrics = std::make_shared<Lyrics>(ParseLrc(*local));
                if (!lyrics->lines.empty()) {
                    lyrics->source = L"Local";
                    deliver({Status::Found, std::move(lyrics)});
                    return;
                }
            }
            // A streaming provider's own lyrics for its track: an exact match, no search.
            if (stream::Provider* provider = stream::ProviderOf(track)) {
                const stream::Id id = provider->TrackId(track);
                const std::wstring lrc = id.empty() ? std::wstring() : provider->Lyrics(id, *abort);
                if (abort->is_aborting()) return;
                auto lyrics = std::make_shared<Lyrics>(ParseLrc(lrc));
                if (!lyrics->lines.empty()) {
                    lyrics->source = provider->Name();
                    WriteFile(CacheFile(query), Utf8(kSourceTag + lyrics->source + L"]\n" + lrc));
                    deliver({Status::Found, std::move(lyrics)});
                    return;
                }
            }
            if (searchers.empty()) {
                deliver({Status::NotFound, nullptr});
                return;
            }

            // Every searcher on its own thread; collect until all finish or the timeout.
            struct Shared {
                std::mutex mutex;
                std::condition_variable cv;
                std::vector<Candidate> candidates;
                size_t running = 0;
            };
            auto shared = std::make_shared<Shared>();
            shared->running = searchers.size();
            for (const auto& s : searchers) {
                std::thread([s, query, abort, shared] {
                    try {
                        s->Search(query, *abort, [&](Candidate c) {
                            if (c.source.empty()) c.source = s->Name();
                            std::lock_guard lock(shared->mutex);
                            shared->candidates.push_back(std::move(c));
                        });
                    } catch (...) {
                    }
                    std::lock_guard lock(shared->mutex);
                    --shared->running;
                    shared->cv.notify_all();
                }).detach();
            }
            std::vector<Candidate> candidates;
            {
                std::unique_lock lock(shared->mutex);
                shared->cv.wait_for(lock, std::chrono::milliseconds(kSearchTimeoutMs),
                                    [&] { return shared->running == 0 || abort->is_aborting(); });
                candidates = shared->candidates;
            }
            if (abort->is_aborting()) return;
            abort->abort();  // searchers still running after the timeout stop
            const Candidate* best = BestCandidate(query, candidates);
            if (!best) {
                WriteFile(CacheFile(query), Utf8(kNotFoundTag + std::to_wstring(_time64(nullptr)) + L"]\n"));
                deliver({Status::NotFound, nullptr});
                return;
            }
            auto lyrics = std::make_shared<Lyrics>(ParseLrc(best->lrc));
            lyrics->source = best->source;
            WriteFile(CacheFile(query), Utf8(kSourceTag + best->source + L"]\n" + best->lrc));
            deliver({Status::Found, std::move(lyrics)});
        } catch (...) {
            deliver({Status::NotFound, nullptr});
        }
    }).detach();
}

}  // namespace glasswave::lyrics
