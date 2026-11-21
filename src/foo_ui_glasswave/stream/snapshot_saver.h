#pragma once

#include "cache/provider_snapshot.h"
#include "stream/executor.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace glasswave::stream {

// Keeps a provider's bootstrap snapshot (cache::provider_snapshot: the navigation lists of the
// signed-in account, shown at the next start before the session is verified) up to date. The
// provider says how to describe itself and when it may; the saver owns the rest: writing on a
// worker while changes arriving meanwhile queue one more write, dropping a write whose account
// changed under it (cache::AccountRevision and AccountScope are compared again right before
// it), and the final write at shutdown.
class SnapshotSaver {
  public:
    struct Hooks {
        // Whether a snapshot may be written now: a verified, signed-in account, not restoring.
        // Any thread; takes the provider's own lock.
        std::function<bool()> eligible;
        // Fills everything of the snapshot but `provider` (which the saver sets); false when it
        // is not eligible after all. Main thread; may throw (the write is skipped).
        std::function<bool(cache::provider_snapshot::Snapshot&)> fill;
    };

    // A snapshot ready to be written, with the account it describes.
    struct Draft {
        cache::provider_snapshot::Snapshot payload;
        std::string scope;
        uint64_t revision = 0;
    };

    // `key` is the provider's cache key; the saver posts on `queue` of `executor`.
    SnapshotSaver(std::string key, Executor& executor, size_t queue, Hooks hooks);

    struct Loaded {
        cache::provider_snapshot::Snapshot snapshot;
        std::string scope;
    };
    // The cached snapshot of the current account, when there is one with an account id.
    // `sourceOrigin` and `context` are the provider's identity of the data (they must match what
    // was saved). Main thread; reads a file.
    std::optional<Loaded> Load(const std::string& sourceOrigin, const std::string& context) const;

    // Something the snapshot shows changed. Writes it in the background, or after the write in
    // flight when there is one. Main thread.
    void Queue();
    // The session was reset or the account changed (call with the provider's lock held, in the
    // same step): a write in flight is forgotten and nothing is pending.
    void Reset();

    // The final write at shutdown: Prepare while the provider still has its state, Commit after
    // the workers have stopped. Commit writes nothing when the account changed in between.
    std::optional<Draft> Prepare() const;
    void Commit(const Draft& draft) const;

  private:
    struct Flight;
    bool Prepare(Draft& draft) const;
    // The write of `epoch` has ended, run or dropped (any thread).
    void Release(uint64_t epoch);

    std::string m_key;
    Executor& m_executor;
    size_t m_queue;
    Hooks m_hooks;
    std::mutex m_mutex; // the three below only; never held while calling a hook
    bool m_posted = false;
    bool m_dirty = false;
    uint64_t m_epoch = 0;
};

} // namespace glasswave::stream
