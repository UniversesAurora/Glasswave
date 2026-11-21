#include "pch.h"
#include "stream/executor.h"

#include <mutex>
#include <vector>

namespace glasswave::stream {

struct Executor::State {
    mutable std::mutex mutex;
    bool running = false;
    uint64_t generation = 0;
    std::vector<std::unique_ptr<core::TaskQueue>> queues;
    abort_callback_impl abort;

    bool Current(uint64_t value) const {
        std::lock_guard lock(mutex);
        return running && value == generation;
    }
};

Executor::Executor() : m_state(std::make_shared<State>()) {}

Executor::~Executor() {
    Shutdown();
}

uint64_t Executor::Start(std::initializer_list<int> threads) {
    std::lock_guard lock(m_state->mutex);
    if (m_state->running) return m_state->generation;
    m_state->running = true;
    ++m_state->generation;
    m_state->abort.reset();
    m_state->queues.clear();
    for (const int count : threads)
        m_state->queues.push_back(std::make_unique<core::TaskQueue>(count));
    return m_state->generation;
}

void Executor::Stop() {
    std::lock_guard lock(m_state->mutex);
    if (!m_state->running) return;
    m_state->running = false;
    ++m_state->generation;
    m_state->abort.abort();
}

void Executor::Join() {
    std::vector<std::unique_ptr<core::TaskQueue>> queues;
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->running) return;
        queues.swap(m_state->queues);
    }
    // Outside the lock: a worker finishing its task takes it to see that the task is stale.
    for (auto& queue : queues)
        queue->Stop();
}

bool Executor::Running() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->running;
}

uint64_t Executor::Generation() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->generation;
}

bool Executor::Current(uint64_t generation) const {
    return m_state->Current(generation);
}

uint64_t Executor::Bump() {
    std::lock_guard lock(m_state->mutex);
    return ++m_state->generation;
}

bool Executor::Post(size_t queue, uint64_t generation, std::function<void()> task) {
    if (!task) return false;
    std::lock_guard lock(m_state->mutex);
    if (!m_state->running || generation != m_state->generation || queue >= m_state->queues.size())
        return false;
    // Posted under the lock: Join takes the queues out under it, so a late post either lands
    // before that (the queue drops it with its pending tasks) or sees no queue.
    m_state->queues[queue]->Post([state = m_state, generation, task = std::move(task)] {
        if (state->Current(generation)) task();
    });
    return true;
}

void Executor::PostMain(uint64_t generation, std::function<void()> task) {
    if (!task) return;
    fb2k::inMainThread([state = m_state, generation, task = std::move(task)] {
        if (state->Current(generation)) task();
    });
}

abort_callback& Executor::Abort() const {
    return m_state->abort;
}

} // namespace glasswave::stream
