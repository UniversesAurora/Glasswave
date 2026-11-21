#pragma once

#include "netease/convert.h"
#include "stream/catalog.h"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace glasswave::netease {

// What the UI saw or playback asked about, as stream:: records in a stream::Catalog (Records()),
// plus what NetEase keeps of a song beyond them: the quality of its stream (SongTech). The
// metadb handles come from the ncm:// song and cover paths of Records(); metadata is pushed into
// the core cache so persisted playlists show titles after a restart.
class Catalog {
  public:
    static Catalog& Get();

    // The shared records: albums, artists and playlists are put and found there directly, and
    // so are the handles, paths and PreferCover.
    stream::Catalog& Records() { return m_records; }

    void Put(const SongRecord& song);
    void Put(const std::vector<SongRecord>& songs);
    // Main thread: store and feed the songs to metadb in one batch. `persist` queues the
    // restart cache for a deferred write on a worker; list pages pass false because the generic
    // list cache already owns their persistence.
    void PutAndHint(const std::vector<SongRecord>& songs, bool persist = true);
    // Main thread: merge rows of a provider list cache into richer records (they never replace
    // an existing cover or quality), then feed the result to metadb without writing the cache.
    void RestoreAndHint(const std::vector<stream::Track>& tracks);

    std::optional<SongRecord> FindSong(const stream::Id& id) const;

    // Restart cache: <profile>\glasswave\netease\cache\songs.tsv. Load also pushes the cached
    // tags into metadb so persisted ncm:// playlist entries format correctly.
    void Load();
    // Main thread, at quit: writes the restart cache now when a deferred write is still pending.
    void FlushPendingSave() { m_records.FlushPendingSave(); }

  private:
    Catalog();
    static stream::CatalogTraits Traits(Catalog& self);
    void WriteTags(const stream::Track& stored, file_info& info) const;
    std::wstring EncodeExtra(const stream::Id& id) const;

    stream::Catalog m_records;
    mutable std::mutex m_mutex;
    std::unordered_map<stream::Id, SongTech> m_tech;
};

}  // namespace glasswave::netease
