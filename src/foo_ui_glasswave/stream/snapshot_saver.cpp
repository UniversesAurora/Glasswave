#include "pch.h"
#include "stream/snapshot_saver.h"
#include "cache/store.h"

namespace glasswave::stream {

SnapshotSaver::SnapshotSaver(std::string key, Executor& executor, size_t queue, Hooks hooks)
    : m_key(std::move(key)), m_executor(executor), m_queue(queue), m_hooks(std::move(hooks)) {}

std::optional<SnapshotSaver::Loaded> SnapshotSaver::Load(const std::string& sourceOrigin,
                                                         const std::string& context) const {
    try {
        std::string scope = cache::AccountScope(m_key);
        auto cached = cache::provider_snapshot::Load(cache::GetStore(), scope, m_key, sourceOrigin, context);
        if (!cached || cached->accountId.empty()) return std::nullopt;
        return Loaded{std::move(*cached), std::move(scope)};
    } catch (...) {
        return std::nullopt;
    }
}

bool SnapshotSaver::Prepare(Draft& draft) const {
    draft.payload.provider = m_key;
    if (!m_hooks.fill(draft.payload)) return false;
    draft.scope = cache::AccountScope(m_key);
    draft.revision = cache::AccountRevision(m_key);
    return true;
}

std::optional<SnapshotSaver::Draft> SnapshotSaver::Prepare() const {
    if (!m_hooks.eligible()) return std::nullopt;
    try {
        Draft draft;
        if (Prepare(draft)) return draft;
    } catch (...) {
    }
    return std::nullopt;
}

void SnapshotSaver::Commit(const Draft& draft) const {
    try {
        if (cache::AccountRevision(m_key) == draft.revision && cache::AccountScope(m_key) == draft.scope)
            cache::provider_snapshot::Save(cache::GetStore(), draft.scope, draft.payload);
    } catch (...) {
    }
}

void SnapshotSaver::Reset() {
    std::lock_guard lock(m_mutex);
    ++m_epoch;
    m_posted = false;
    m_dirty = false;
}

// Held by a queued write: when the write has run, and also when the executor dropped it unrun
// (a generation bump that is not a Reset, such as a language change), the saver is free for the
// next one. A change that arrived meanwhile queues that next write.
// Made in place and never copied: a copy's end would release the saver early.
struct SnapshotSaver::Flight {
    Flight(SnapshotSaver& saver, uint64_t epoch) : saver(saver), epoch(epoch) {}
    Flight(const Flight&) = delete;
    Flight& operator=(const Flight&) = delete;
    ~Flight() { saver.Release(epoch); }

    SnapshotSaver& saver;
    uint64_t epoch;
};

void SnapshotSaver::Queue() {
    if (!m_hooks.eligible()) return;
    uint64_t generation = 0;
    std::shared_ptr<Flight> flight;
    {
        std::lock_guard lock(m_mutex);
        m_dirty = true;
        if (m_posted) return;
        m_posted = true;
        m_dirty = false;
        flight = std::make_shared<Flight>(*this, m_epoch);
        generation = m_executor.Generation();
    }
    auto draft = std::make_shared<Draft>();
    try {
        if (!Prepare(*draft)) return;
    } catch (...) {
        return;
    }
    m_executor.Post(m_queue, generation, [this, draft, flight = std::move(flight)] {
        if (m_hooks.eligible()) Commit(*draft);
    });
}

void SnapshotSaver::Release(uint64_t epoch) {
    {
        std::lock_guard lock(m_mutex);
        if (epoch != m_epoch) return;
        m_posted = false;
        if (!std::exchange(m_dirty, false)) return;
    }
    // Changes arrived while the write ran: write once more (Queue asks whether it still may).
    fb2k::inMainThread([this] { Queue(); });
}

} // namespace glasswave::stream
