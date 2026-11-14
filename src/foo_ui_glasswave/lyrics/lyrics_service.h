#pragma once

#include "lyrics/lyrics.h"
#include <atomic>
#include <functional>
#include <memory>
#include <cstdint>
#include <string_view>

// Finding lyrics for a track. Explicit selections override automatic lookup. Automatic
// lookup uses the track's tags / adjacent LRC, its streaming provider, then cached and
// searched fallback sources in the owner's configured order.
//
// Searchers are the extension point: the ESLyric script host (docs/plan/lyrics-script-host.md)
// registers one per script. They run on worker threads; everything else here is main thread.

namespace glasswave::lyrics {

// What searchers get to know about the track. A copy, safe on any thread.
struct Query {
    std::wstring title, artist, album; // display values (first artist, no feat. cleanup)
    std::wstring path;                 // foobar2000 path: file://..., ncm://...
    std::wstring nativePath;           // file system path; empty for streams
    uint32_t subsong = 0;
    double duration = 0; // seconds, 0 if unknown
    std::wstring provider = L"local";
    std::wstring providerId;     // exact native provider item id, when applicable
    std::wstring embeddedLyrics; // first synced/most useful embedded lyric field
};

// One result of a searcher, already converted to LRC: enhanced LRC (<mm:ss.xx> word times)
// for word-timed lyrics, translations as reference lines with the same timestamp.
struct Candidate {
    std::wstring title, artist, album; // what the source matched
    std::wstring source;               // shown to the user, normally the searcher's name
    std::wstring lrc;
    int confidence = -1; // 0..100 if the searcher knows, else -1
    // Source-specific durable cache binding. Empty means this source does not expose
    // account-scoped data. accountRevision is process-local and used only to reject lateness.
    std::wstring cacheProvider;
    std::wstring cacheScope;
    std::uint64_t accountRevision = 0;
    std::wstring sourceKey; // stable registry key, distinct from display name
    // A candidate read from a file (a same-name .lrc, a file the owner loaded): its path and
    // the code page it was read in (0: detected), so it can be read again in another.
    std::wstring file;
    unsigned codePage = 0;
};

class Searcher {
  public:
    virtual ~Searcher() = default;
    virtual std::wstring Name() const = 0;
    virtual std::wstring Key() const { return Name(); }
    // Public stable token used in the shared user-visible order. Provider-native adapters use
    // the generic `source` token; plugin searchers normally use Key().
    virtual std::wstring OrderKey(std::wstring_view provider) const {
        (void)provider;
        return Key();
    }
    // Automatic and source-specific searches only instantiate applicable providers. Script
    // searchers default to universal metadata search.
    virtual bool AppliesTo(std::wstring_view provider) const {
        (void)provider;
        return true;
    }
    // Cheap local checks always rerun so changed or removed tags/sidecars cannot be shadowed
    // by an old automatic lyric.
    virtual bool Cacheable() const { return true; }
    // Blocking, on a worker thread. Calls `found` (any thread, any number of times) for each
    // candidate; returns when done. Must poll `abort` and return early when it is set.
    virtual void Search(const Query& query, abort_callback& abort,
                        const std::function<void(Candidate)>& found) = 0;
};

// Adds / removes a searcher. Main thread.
void RegisterSearcher(std::shared_ptr<Searcher> searcher);
void UnregisterSearcher(const Searcher* searcher);
std::vector<std::shared_ptr<Searcher>> Searchers();

enum class Status {
    None,
    Searching,
    Found,
    NotFound
};

struct Result {
    Status status = Status::None;
    std::shared_ptr<const Lyrics> lyrics; // set when Found
};

inline bool IsCurrentLyricsRequest(std::uint64_t request, std::uint64_t currentRequest,
                                   std::wstring_view track, std::wstring_view currentTrack) {
    return request == currentRequest && track == currentTrack;
}

// The view may keep an equal parsed document through a same-track refresh. Source labels and
// metadata do not affect the rendered lines; timing, text and word structure do.
inline bool SameLyricsDocument(const Lyrics& a, const Lyrics& b) {
    if (a.synced != b.synced || a.karaoke != b.karaoke || a.lines.size() != b.lines.size()) return false;
    for (size_t i = 0; i < a.lines.size(); ++i) {
        const auto& x = a.lines[i];
        const auto& y = b.lines[i];
        if (x.start != y.start || x.end != y.end || x.text != y.text || x.translation != y.translation ||
            x.romanization != y.romanization || x.words.size() != y.words.size())
            return false;
        for (size_t j = 0; j < x.words.size(); ++j)
            if (x.words[j].start != y.words[j].start || x.words[j].end != y.words[j].end ||
                x.words[j].text != y.words[j].text)
                return false;
    }
    return true;
}

// The same lines, words and references, whatever their times (an offset changed).
inline bool SameLyricsText(const Lyrics& a, const Lyrics& b) {
    if (a.synced != b.synced || a.karaoke != b.karaoke || a.lines.size() != b.lines.size()) return false;
    for (size_t i = 0; i < a.lines.size(); ++i) {
        const auto& x = a.lines[i];
        const auto& y = b.lines[i];
        if (x.text != y.text || x.translation != y.translation || x.romanization != y.romanization ||
            x.words.size() != y.words.size())
            return false;
    }
    return true;
}

class Service {
  public:
    static Service& Get();

    // Looks up lyrics for `track` and calls `done` on the main thread: first with Searching
    // (unless a cached answer is ready at once), then with Found or NotFound. A new request
    // cancels the previous one, whose callback is never called again.
    void Request(const metadb_handle_ptr& track, std::function<void(const Result&)> done);
    // Drops only the automatic result. An explicit owner selection remains in force.
    void Research(const metadb_handle_ptr& track, std::function<void(const Result&)> done);
    void Cancel();
    void Shutdown();
    static Query QueryFor(const metadb_handle_ptr& track);
    // Stable identity for request/callback ownership. Provider tracks use the provider's
    // opaque id; local files use foobar2000's path and subsong.
    static std::wstring TrackIdentity(const metadb_handle_ptr& track);
    // Manual searches do not replace the displayed lyric until Select is called.
    void FindCandidates(Query query, std::wstring source,
                        std::function<void(std::vector<Candidate>, bool finished)> done);
    void CancelSearch();
    Result Select(const metadb_handle_ptr& track, const Candidate& candidate);
    Result LoadFile(const metadb_handle_ptr& track, const std::wstring& path, unsigned codePage = 0);
    // The shown lyrics' file read again in another code page (0: detected) and selected.
    // NotFound when they did not come from a readable file.
    Result Reencode(const metadb_handle_ptr& track, const Lyrics& shown, unsigned codePage);
    // Explicitly removes the owner's per-track selection.
    void UseAutomatic(const metadb_handle_ptr& track);
    void ClearCache();

    // Every candidate the searches for `track` have turned up so far in this session (the
    // automatic lookup's and the search dialog's), for Select lyrics. Main thread.
    std::vector<Candidate> Candidates(const metadb_handle_ptr& track) const;

    // The owner's timing correction for one lyric of a track (as found, before any offset), in
    // ms, kept across sessions: positive shows it earlier, as LRC's [offset:]. Each lyric of a
    // song keeps its own. Main thread.
    int Offset(const metadb_handle_ptr& track, const Lyrics& lyrics) const;
    void SetOffset(const metadb_handle_ptr& track, const Lyrics& lyrics, int offsetMs);

    // Folder of cached results: <profile>\glasswave\lyrics.
    static std::wstring CacheFolder();
    // Folder of lyrics saved to the profile: <profile>\lyrics.
    static std::wstring ProfileLyricsFolder();

  private:
    Service() = default;
    void Start(const metadb_handle_ptr& track, bool useCache, std::function<void(const Result&)> done);
    void Remember(const Query& query, const std::vector<Candidate>& candidates);
    std::wstring m_candidatesFor; // the track the candidates are for (path and subsong)
    std::vector<Candidate> m_candidates;
    void Run(std::shared_ptr<abort_callback_impl> abort, std::function<void()> work);

    struct Worker {
        std::thread thread;
        std::shared_ptr<abort_callback_impl> abort;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Worker> m_workers;

    std::shared_ptr<abort_callback_impl> m_abort;
    unsigned m_generation = 0;
    // The automatic search in flight: its track and where its answer goes. Asking for the
    // same track again redirects the answer instead of starting another search.
    std::wstring m_activeIdentity;
    std::shared_ptr<std::function<void(const Result&)>> m_activeDone;
    std::shared_ptr<abort_callback_impl> m_searchAbort;
    unsigned m_searchGeneration = 0;
};

// Picks the candidate that matches the query best, or nullptr if none is good enough.
const Candidate* BestCandidate(const Query& query, const std::vector<Candidate>& candidates);

} // namespace glasswave::lyrics
