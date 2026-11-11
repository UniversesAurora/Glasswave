#include "pch.h"

#include "gfx/frame_clock.h"

namespace glasswave::gfx {

namespace {

using WaitForCompositorClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

WaitForCompositorClock CompositorClock() {
    static const auto fn = [] {
        HMODULE dcomp = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dcomp ? reinterpret_cast<WaitForCompositorClock>(
                           GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"))
                     : nullptr;
    }();
    return fn;
}

} // namespace

FrameClock::FrameClock(HWND window, UINT message) : m_window(window), m_message(message) {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BeginThread();
}

#ifdef GLASSWAVE_FRAME_CLOCK_TEST
FrameClock::FrameClock(TestWait wait, TestPost post, std::chrono::milliseconds fallbackPeriod)
    : m_window(nullptr), m_message(0),
      m_fallbackPeriod(std::max(fallbackPeriod, std::chrono::milliseconds(1))), m_testWait(std::move(wait)),
      m_testPost(std::move(post)) {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BeginThread();
}

bool FrameClock::IsCompositorWakeForTest(DWORD status, UINT handleCount) {
    return IsCompositorWake(status, handleCount);
}
#endif

FrameClock::~FrameClock() {
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_running.store(false, std::memory_order_release);
    }
    if (m_stopEvent) SetEvent(m_stopEvent);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_stopEvent) CloseHandle(std::exchange(m_stopEvent, nullptr));
}

void FrameClock::BeginThread() {
    m_thread = std::thread([this] { Run(); });
}

void FrameClock::Start() {
    {
        std::lock_guard lock(m_mutex);
        if (m_quit || m_running.load(std::memory_order_acquire)) return;
        if (m_stopEvent) ResetEvent(m_stopEvent);
        m_running.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
}

void FrameClock::Stop() {
    m_running.store(false, std::memory_order_release);
    if (m_stopEvent) SetEvent(m_stopEvent);
    m_cv.notify_all();
}

bool FrameClock::ContextRunning() {
    std::lock_guard lock(m_mutex);
    return !m_quit && m_running.load(std::memory_order_acquire);
}

bool FrameClock::IsCompositorWake(DWORD status, UINT handleCount) {
    // The documented API is described as returning NTSTATUS, but the shipped Windows 11
    // implementation reports the wake index: zero-based event waits use 0..count-1 and the
    // compositor clock itself uses count.
    return status == handleCount;
}

bool FrameClock::WaitForNextSignal() {
#ifdef GLASSWAVE_FRAME_CLOCK_TEST
    if (m_testWait) return m_testWait() == WaitResult::Signaled && ContextRunning();
#endif
    if (const auto wait = CompositorClock()) {
        const HANDLE* handles = m_stopEvent ? &m_stopEvent : nullptr;
        const UINT handleCount = handles ? 1 : 0;
        const DWORD status = wait(handleCount, handles, 100);
        return ContextRunning() && IsCompositorWake(status, handleCount);
    }
    const HRESULT result = DwmFlush();
    return ContextRunning() && SUCCEEDED(result);
}

bool FrameClock::WaitForFallbackRetry() {
    std::unique_lock lock(m_mutex);
    m_cv.wait_until(lock, std::chrono::steady_clock::now() + m_fallbackPeriod,
                    [this] { return m_quit || !m_running.load(std::memory_order_acquire); });
    return !m_quit && m_running.load(std::memory_order_acquire);
}

bool FrameClock::PostFrame() {
#ifdef GLASSWAVE_FRAME_CLOCK_TEST
    if (m_testPost) return m_testPost();
#endif
    return PostMessageW(m_window, m_message, 0, 0) != FALSE;
}

void FrameClock::Run() {
    SetThreadDescription(GetCurrentThread(), L"Glasswave frame clock");
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_quit || m_running; });
            if (m_quit) return;
        }
        while (ContextRunning()) {
            if (!WaitForNextSignal()) {
                if (!WaitForFallbackRetry()) break;
                continue;
            }
            if (!m_pending.exchange(true)) {
                if (!PostFrame()) m_pending = false;
            }
        }
    }
}

} // namespace glasswave::gfx
