#ifndef GLASSWAVE_FRAME_CLOCK_STANDALONE
#define GLASSWAVE_FRAME_CLOCK_STANDALONE
#endif
#ifndef GLASSWAVE_FRAME_CLOCK_TEST
#define GLASSWAVE_FRAME_CLOCK_TEST
#endif

#include <Windows.h>

#include "gfx/frame_clock.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using glasswave::gfx::FrameClock;

struct Counters {
    std::atomic<uint64_t> waits{0};
    std::atomic<uint64_t> posts{0};
    std::atomic<FrameClock*> clock{nullptr};
};

double ProcessCpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return NAN;
    auto toSeconds = [](const FILETIME& value) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = value.dwLowDateTime;
        ticks.HighPart = value.dwHighDateTime;
        return static_cast<double>(ticks.QuadPart) / 10'000'000.0;
    };
    return toSeconds(kernel) + toSeconds(user);
}

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool HealthyCadence() {
    Counters counters;
    FrameClock clock(
        [&] {
            ++counters.waits;
            std::this_thread::sleep_for(20ms);
            return FrameClock::WaitResult::Signaled;
        },
        [&] {
            ++counters.posts;
            if (auto* frameClock = counters.clock.load(std::memory_order_acquire)) frameClock->Ack();
            return true;
        },
        50ms);
    counters.clock.store(&clock, std::memory_order_release);
    clock.Start();
    std::this_thread::sleep_for(180ms);
    clock.Stop();
    counters.clock.store(nullptr, std::memory_order_release);
    const uint64_t posts = counters.posts.load();
    return Check(posts >= 5, "healthy compositor cadence was throttled by fallback")
               ? true
               : (std::fprintf(stderr, "healthy posts=%llu\n", static_cast<unsigned long long>(posts)),
                  false);
}

bool ImmediateFailureBounded() {
    Counters counters;
    const double cpuBefore = ProcessCpuSeconds();
    auto time = Clock::now();
    std::chrono::nanoseconds stopLatency{};
    {
        auto clock = std::make_unique<FrameClock>(
            [&] {
                ++counters.waits;
                return FrameClock::WaitResult::Unavailable;
            },
            [&] {
                ++counters.posts;
                if (auto* frameClock = counters.clock.load(std::memory_order_acquire)) frameClock->Ack();
                return true;
            },
            10ms);
        counters.clock.store(clock.get(), std::memory_order_release);
        clock->Start();
        std::this_thread::sleep_for(220ms);
        time = Clock::now();
        clock->Stop();
        counters.clock.store(nullptr, std::memory_order_release);
        clock.reset();
        stopLatency = Clock::now() - time;
    }
    const double cpu = ProcessCpuSeconds() - cpuBefore;
    const uint64_t waits = counters.waits.load();
    const uint64_t posts = counters.posts.load();
    return Check(posts == 0, "failed compositor wait posted animation frames") &&
           Check(waits >= 5 && waits <= 40, "failed compositor retry count was not paced") &&
           Check(std::isfinite(cpu) && cpu < 0.15, "failed compositor path consumed excessive CPU") &&
           Check(stopLatency < 250ms, "fallback pacer delayed Stop");
}

bool MixedFailureFastAckBounded() {
    Counters counters;
    const double cpuBefore = ProcessCpuSeconds();
    {
        FrameClock clock(
            [&] {
                const uint64_t call = ++counters.waits;
                return call % 5 == 0 ? FrameClock::WaitResult::Signaled : FrameClock::WaitResult::Unavailable;
            },
            [&] {
                ++counters.posts;
                if (auto* frameClock = counters.clock.load(std::memory_order_acquire)) frameClock->Ack();
                return true;
            },
            10ms);
        counters.clock.store(&clock, std::memory_order_release);
        clock.Start();
        std::this_thread::sleep_for(220ms);
        clock.Stop();
        counters.clock.store(nullptr, std::memory_order_release);
    }
    const double cpu = ProcessCpuSeconds() - cpuBefore;
    const uint64_t posts = counters.posts.load();
    const uint64_t waits = counters.waits.load();
    return Check(posts <= 12, "mixed immediate failure produced unbounded posts") &&
           Check(waits <= 40, "mixed immediate failure bypassed retry pacing") &&
           Check(std::isfinite(cpu) && cpu < 0.15, "mixed failure path consumed excessive CPU");
}

bool CompositorWakeClassification() {
    return Check(FrameClock::IsCompositorWakeForTest(0, 0), "count=0 compositor wake was rejected") &&
           Check(FrameClock::IsCompositorWakeForTest(1, 1), "count=1 compositor wake was rejected") &&
           Check(!FrameClock::IsCompositorWakeForTest(0, 1),
                 "stop handle wake was treated as a compositor tick");
}

bool StopsQuicklyAndIdles() {
    Counters counters;
    {
        auto clock = std::make_unique<FrameClock>(
            [&] {
                ++counters.waits;
                return FrameClock::WaitResult::Unavailable;
            },
            [&] {
                ++counters.posts;
                if (auto* frameClock = counters.clock.load(std::memory_order_acquire)) frameClock->Ack();
                return true;
            },
            500ms);
        counters.clock.store(clock.get(), std::memory_order_release);
        clock->Start();
        std::this_thread::sleep_for(30ms);
        const auto started = Clock::now();
        clock->Stop();
        counters.clock.store(nullptr, std::memory_order_release);
        clock.reset();
        if (!Check(Clock::now() - started < 200ms, "long fallback interval delayed Stop")) return false;
    }
    const uint64_t requestsAfterStop = counters.waits.load();
    const uint64_t postsAfterStop = counters.posts.load();
    std::this_thread::sleep_for(120ms);
    return Check(counters.waits.load() == requestsAfterStop, "stopped clock continued compositor polling") &&
           Check(counters.posts.load() == postsAfterStop, "stopped clock continued posting frames") &&
           Check(postsAfterStop == 0, "failed wait posted while demand was active");
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view selected = argc > 1 ? argv[1] : "all";
    const bool all = selected == "all";
    if (!all && selected != "compositor-wake" && selected != "healthy" && selected != "immediate-failure" &&
        selected != "mixed-failure" && selected != "stop-idle") {
        std::fprintf(stderr, "FAIL: unknown frame clock test case\n");
        return 2;
    }
    const bool classification = (all || selected == "compositor-wake") && CompositorWakeClassification();
    const bool healthy = (all || selected == "healthy") && HealthyCadence();
    const bool failed = (all || selected == "immediate-failure") && ImmediateFailureBounded();
    const bool mixed = (all || selected == "mixed-failure") && MixedFailureFastAckBounded();
    const bool stopped = (all || selected == "stop-idle") && StopsQuicklyAndIdles();
    std::printf("compositorWake=%s healthyCadence=%s immediateFailure=%s mixedFastAck=%s stopIdle=%s\n",
                classification ? "pass" : "skip-or-fail", healthy ? "pass" : "skip-or-fail",
                failed ? "pass" : "skip-or-fail", mixed ? "pass" : "skip-or-fail",
                stopped ? "pass" : "skip-or-fail");
    if (!all) return classification || healthy || failed || mixed || stopped ? 0 : 1;
    return classification && healthy && failed && mixed && stopped ? 0 : 1;
}
