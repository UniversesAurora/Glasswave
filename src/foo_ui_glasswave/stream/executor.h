#pragma once

#include "core/task_queue.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>

namespace glasswave::stream {

// The worker queues of one provider, gated by a generation. A provider starts the executor with
// the thread count of each queue it wants, and every task it posts names the generation it was
// prepared for. A task whose generation is no longer current (Bump: the account changed, the
// language changed, the user signed out; Stop: the provider shut down) never runs; its
// destruction completes a stream::GuardDone completion it captured with an error.
//
// All state lives in one shared block that every queued task and every pending main-thread
// callback holds, so a post that arrives after Shutdown (or after the executor itself is gone)
// finds a stopped block and is dropped; there is no window in which it reaches a destroyed
// queue. Post, PostMain, Generation, Current and Abort are safe from any thread. Start, Stop,
// Join and Shutdown are for the main thread, and Join must not run on a worker of its own queue.
class Executor {
  public:
    Executor();
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Creates the queues (one per entry, with that many threads), clears the abort flag and
    // starts a new generation, which it returns. Does nothing and returns the current
    // generation when already running.
    uint64_t Start(std::initializer_list<int> threads);
    // Invalidates every queued and running task at once: the generation moves on, the abort
    // callback is set and further posts are dropped. Cheap, so a provider calls it while it
    // holds its own lock; Join then waits for the workers.
    void Stop();
    // Waits for the workers of a stopped executor and drops the tasks still queued.
    void Join();
    void Shutdown() {
        Stop();
        Join();
    }

    bool Running() const;
    uint64_t Generation() const;
    // True while running and `generation` is the current one.
    bool Current(uint64_t generation) const;
    // Starts a new generation (in-flight tasks of the previous one stop running); returns it.
    uint64_t Bump();

    // Runs `task` on a worker of `queue` unless `generation` is stale by then. Returns whether
    // it was queued.
    bool Post(size_t queue, uint64_t generation, std::function<void()> task);
    // Runs `task` on the main thread unless `generation` is stale by then.
    void PostMain(uint64_t generation, std::function<void()> task);

    // Set by Stop; every blocking call of the provider takes it. Valid for the executor's life.
    abort_callback& Abort() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace glasswave::stream
