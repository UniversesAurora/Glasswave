#include "pch.h"
#include "core/log.h"
#include "core/task_queue.h"

namespace glasswave::core {

TaskQueue::TaskQueue(int threads) {
    const int n = threads > 0 ? threads : 1;
    for (int i = 0; i < n; ++i)
        m_threads.emplace_back([this] { Worker(); });
}

TaskQueue::~TaskQueue() {
    Stop();
}

void TaskQueue::Post(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping) return;
        m_tasks.push_back(std::move(task));
    }
    m_cv.notify_one();
}

void TaskQueue::Stop() {
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping) return;
        m_stopping = true;
    }
    m_cv.notify_all();
    for (auto& t : m_threads)
        if (t.joinable()) t.join();
    m_threads.clear();
    std::deque<std::function<void()>> drop;
    {
        std::lock_guard lock(m_mutex);
        drop.swap(m_tasks);
    }
}

void TaskQueue::Worker() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
            if (m_stopping && m_tasks.empty()) return;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            Log(std::wstring(L"task failed: ") + pfc::stringcvt::string_wide_from_utf8(e.what()).get_ptr());
        } catch (...) {
            Log(L"task failed: unknown exception");
        }
    }
}

} // namespace glasswave::core
