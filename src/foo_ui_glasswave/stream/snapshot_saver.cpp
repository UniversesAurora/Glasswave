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

void SnapshotSaver::Queue() {
    if (!m_hooks.eligible()) return;
    uint64_t epoch = 0;
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        m_dirty = true;
        if (m_posted) return;
        m_posted = true;
        m_dirty = false;
        epoch = m_epoch;
        generation = m_executor.Generation();
    }

    const auto abandon = [this, epoch] {
        std::lock_guard lock(m_mutex);
        if (epoch == m_epoch) m_posted = false;
    };
    auto draft = std::make_shared<Draft>();
    try {
        if (!Prepare(*draft)) return abandon();
    } catch (...) {
        return abandon();
    }

    const bool queued = m_executor.Post(m_queue, generation, [this, epoch, generation, draft] {
        if (m_hooks.eligible()) Commit(*draft);
        m_executor.PostMain(generation, [this, epoch] { Complete(epoch); });
    });
    if (!queued) abandon();
}

void SnapshotSaver::Complete(uint64_t epoch) {
    bool dirty = false;
    {
        std::lock_guard lock(m_mutex);
        if (epoch != m_epoch) return;
        m_posted = false;
        dirty = m_dirty;
    }
    if (!dirty) return;
    // Changes arrived while the write ran: write once more when it still makes sense.
    if (m_hooks.eligible()) {
        Queue();
        return;
    }
    std::lock_guard lock(m_mutex);
    if (epoch == m_epoch) m_dirty = false;
}

} // namespace glasswave::stream
