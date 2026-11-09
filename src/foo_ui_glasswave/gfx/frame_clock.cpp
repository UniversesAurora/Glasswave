#include "pch.h"

#include "gfx/frame_clock.h"

namespace glasswave::gfx {

namespace {

using WaitForCompositorClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

WaitForCompositorClock CompositorClock() {
    static const auto fn = [] {
        HMODULE dcomp = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dcomp ? reinterpret_cast<WaitForCompositorClock>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"))
                     : nullptr;
    }();
    return fn;
}

}  // namespace

FrameClock::FrameClock(HWND window, UINT message) : m_window(window), m_message(message) {
    m_thread = std::thread([this] { Run(); });
}

FrameClock::~FrameClock() {
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_running = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void FrameClock::Start() {
    if (m_running) return;
    {
        std::lock_guard lock(m_mutex);
        m_running = true;
    }
    m_cv.notify_all();
}

void FrameClock::Stop() { m_running = false; }

void FrameClock::Run() {
    SetThreadDescription(GetCurrentThread(), L"Glasswave frame clock");
    const auto wait = CompositorClock();
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_quit || m_running; });
            if (m_quit) return;
        }
        while (m_running) {
            if (wait) wait(0, nullptr, 100);
            else DwmFlush();
            if (!m_running) break;
            if (!m_pending.exchange(true)) {
                if (!PostMessageW(m_window, m_message, 0, 0)) m_pending = false;
            }
        }
    }
}

}  // namespace glasswave::gfx
