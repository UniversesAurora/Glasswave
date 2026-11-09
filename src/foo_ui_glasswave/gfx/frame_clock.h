#pragma once

#include <atomic>

// Paces per-frame animation to the compositor (the display's refresh rate: 60, 120, 144 Hz
// ...). While running, a worker thread waits for each compositor clock tick
// (DCompositionWaitForCompositorClock, Windows 11; DwmFlush elsewhere) and posts `message`
// to the window, at most one pending at a time. Stopped, the thread sleeps: idle stays idle.

namespace glasswave::gfx {

class FrameClock {
public:
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
    void Run();

    HWND m_window;
    UINT m_message;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic_bool m_running{false}, m_pending{false};
    bool m_quit = false;
};

}  // namespace glasswave::gfx
