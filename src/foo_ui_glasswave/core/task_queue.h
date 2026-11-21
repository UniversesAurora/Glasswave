#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace glasswave::core {

// A small fixed-size worker pool for blocking network calls. Results are delivered by the
// task itself (normally through fb2k::inMainThread).
class TaskQueue {
public:
    explicit TaskQueue(int threads = 2);
    ~TaskQueue();
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    void Post(std::function<void()> task);
    void Stop();

private:
    void Worker();
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
};

}  // namespace glasswave::core
