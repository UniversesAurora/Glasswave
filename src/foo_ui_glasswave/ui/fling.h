#pragma once

#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

// A dragged list let go keeps moving, as Apple Music's do: the last positions of the drag give
// the speed it is released with, and that speed decays frame by frame. The queue and lyrics
// views share it; each decides when the motion is slow enough to stop.

namespace glasswave::ui {

class FlingTracker {
  public:
    using Clock = std::chrono::steady_clock;

    void Reset() { m_trail.clear(); }
    // The drag's position now (any unit); the last eight are kept.
    void Sample(float position) {
        m_trail.emplace_back(Clock::now(), position);
        if (m_trail.size() > kSamples) m_trail.erase(m_trail.begin());
    }
    // Units per second, in the direction the position grew: 0 when the drag had stopped
    // (no sample in the last 80 ms) or moved slower than 50 per second.
    float ReleaseVelocity() const {
        if (m_trail.size() < 2) return 0;
        const auto& [firstAt, first] = m_trail.front();
        const auto& [lastAt, last] = m_trail.back();
        const float secs = std::chrono::duration<float>(lastAt - firstAt).count();
        const float age = std::chrono::duration<float>(Clock::now() - lastAt).count();
        const float velocity = secs > 0.005f && age < 0.08f ? (last - first) / secs : 0.0f;
        return std::abs(velocity) < 50 ? 0.0f : velocity;
    }
    // A velocity after `dt` seconds of friction: 5 % per 60 Hz frame.
    static float Decay(float velocity, float dt) { return velocity * std::pow(0.95f, dt * 60.0f); }

  private:
    static constexpr size_t kSamples = 8;
    std::vector<std::pair<Clock::time_point, float>> m_trail;
};

// How many notches a wheel message turned (WHEEL_DELTA each; fractions on fine wheels).
inline float WheelNotches(int delta) { return delta / 120.0f; }

} // namespace glasswave::ui
