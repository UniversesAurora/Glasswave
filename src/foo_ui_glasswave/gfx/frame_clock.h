#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// Paces per-frame animation to the compositor (the display's refresh rate: 60, 120, 144 Hz
// ...). While running, a worker thread waits for each compositor clock tick
// (DCompositionWaitForCompositorClock, Windows 11; DwmFlush elsewhere) and posts `message`
// to the window, at most one pending at a time. A failed or occluded compositor wait posts
// nothing and retries after 100 ms. Stopped, the thread sleeps: idle stays idle.

namespace glasswave::gfx {

class FrameClock {
  public:
#ifdef GLASSWAVE_FRAME_CLOCK_TEST
    enum class WaitResult {
        Signaled,
        Unavailable,
    };
    using TestWait = std::function<WaitResult()>;
    using TestPost = std::function<bool()>;
    FrameClock(TestWait wait, TestPost post, std::chrono::milliseconds fallbackPeriod);
    static bool IsCompositorWakeForTest(DWORD status, UINT handleCount);
#endif
    FrameClock(HWND window, UINT message);
    ~FrameClock();
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;

    void Start();
    void Stop();
    bool Running() const { return m_running; }
    // The window handled the message; the next tick may post again.
    void Ack() { m_pending = false; }

  private:
    static bool IsCompositorWake(DWORD status, UINT handleCount);
    void BeginThread();
    bool ContextRunning();
    bool WaitForNextSignal();
    bool WaitForFallbackRetry();
    bool PostFrame();
    void Run();

    HWND m_window;
    UINT m_message;
    std::chrono::milliseconds m_fallbackPeriod{100};
    HANDLE m_stopEvent = nullptr;
#ifdef GLASSWAVE_FRAME_CLOCK_TEST
    TestWait m_testWait;
    TestPost m_testPost;
#endif
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic_bool m_running{false}, m_pending{false};
    bool m_quit = false;
};

} // namespace glasswave::gfx
