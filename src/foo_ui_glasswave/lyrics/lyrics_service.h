#pragma once

#include "lyrics/lyrics.h"
#include <atomic>

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
};

// One result of a searcher, already converted to LRC: enhanced LRC (<mm:ss.xx> word times)
// for word-timed lyrics, translations as reference lines with the same timestamp.
struct Candidate {
    std::wstring title, artist, album; // what the source matched
    std::wstring source;               // shown to the user, normally the searcher's name
    std::wstring lrc;
    int confidence = -1; // 0..100 if the searcher knows, else -1
};

class Searcher {
  public:
    virtual ~Searcher() = default;
    virtual std::wstring Name() const = 0;
    virtual std::wstring Key() const { return Name(); }
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

class Service {
  public:
    static Service& Get();

    // Looks up lyrics for `track` and calls `done` on the main thread: first with Searching
    // (unless a cached answer is ready at once), then with Found or NotFound. A new request
    // cancels the previous one, whose callback is never called again.
    void Request(const metadb_handle_ptr& track, std::function<void(const Result&)> done);
    // Drops the cached result for `track` and searches again (the "search again" command).
    void Research(const metadb_handle_ptr& track, std::function<void(const Result&)> done);
    void Cancel();
    void Shutdown();
    static Query QueryFor(const metadb_handle_ptr& track);
    // Manual searches do not replace the displayed lyric until Select is called.
    void FindCandidates(Query query, std::wstring source,
                        std::function<void(std::vector<Candidate>, bool finished)> done);
    void CancelSearch();
    Result Select(const metadb_handle_ptr& track, const Candidate& candidate);
    Result LoadFile(const metadb_handle_ptr& track, const std::wstring& path);
    void UseAutomatic(const metadb_handle_ptr& track);
    void ClearCache();

    // Folder of cached results: <profile>\glasswave\lyrics.
    static std::wstring CacheFolder();

  private:
    Service() = default;
    void Start(const metadb_handle_ptr& track, bool useCache, std::function<void(const Result&)> done);
    void Run(std::shared_ptr<abort_callback_impl> abort, std::function<void()> work);

    struct Worker {
        std::thread thread;
        std::shared_ptr<abort_callback_impl> abort;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Worker> m_workers;

    std::shared_ptr<abort_callback_impl> m_abort;
    unsigned m_generation = 0;
    std::shared_ptr<abort_callback_impl> m_searchAbort;
    unsigned m_searchGeneration = 0;
};

// Picks the candidate that matches the query best, or nullptr if none is good enough.
const Candidate* BestCandidate(const Query& query, const std::vector<Candidate>& candidates);

} // namespace glasswave::lyrics
