#pragma once

// A damped spring for UI motion (mass, stiffness, damping as in CSS/Apple spring
// animations). Integrated in fixed sub-steps so large frame gaps stay stable. A new target
// can be scheduled after a delay, which the lyrics view uses for its staggered line motion.

namespace glasswave::ui {

struct SpringParams {
    float mass = 1, stiffness = 100, damping = 10;
};

class Spring {
public:
    explicit Spring(float position = 0) : m_pos(position), m_target(position) {}

    float Position() const { return m_pos; }
    float Target() const { return m_hasPending ? m_pending : m_target; }
    bool Moving() const { return m_hasPending || m_pos != m_target || m_vel != 0; }

    void SetParams(const SpringParams& p) { m_params = p; }
    // Jumps to `position` and stops.
    void Snap(float position) {
        m_pos = m_target = position;
        m_vel = 0;
        m_hasPending = false;
    }
    // Translates the spring without changing its velocity or remaining motion.
    void Shift(float delta) {
        m_pos += delta;
        m_target += delta;
        m_pending += delta;
    }
    // Moves towards `target`, starting after `delay` seconds.
    void SetTarget(float target, float delay = 0) {
        if (delay > 0) {
            if (m_hasPending && m_pending == target) return;
            m_pending = target;
            m_delay = delay;
            m_hasPending = true;
        } else {
            m_hasPending = false;
            m_target = target;
        }
    }
    // Advances by `dt` seconds; returns whether it is still moving.
    bool Step(float dt) {
        if (m_hasPending) {
            m_delay -= dt;
            if (m_delay <= 0) {
                m_target = m_pending;
                m_hasPending = false;
            }
        }
        constexpr float kStep = 1.0f / 240.0f;
        while (dt > 0) {
            const float h = std::min(dt, kStep);
            const float force = -m_params.stiffness * (m_pos - m_target) - m_params.damping * m_vel;
            m_vel += force / m_params.mass * h;
            m_pos += m_vel * h;
            dt -= h;
        }
        if (std::abs(m_pos - m_target) < 0.01f && std::abs(m_vel) < 0.01f) {
            m_pos = m_target;
            m_vel = 0;
        }
        return Moving();
    }

private:
    SpringParams m_params;
    float m_pos = 0, m_vel = 0, m_target = 0;
    float m_pending = 0, m_delay = 0;
    bool m_hasPending = false;
};

// Exponential approach for values CSS would animate with a transition (opacity, blur).
inline float Approach(float value, float target, float dt, float seconds) {
    const float k = 1.0f - std::exp(-dt / std::max(seconds / 3.0f, 1e-3f));
    const float next = value + (target - value) * k;
    return std::abs(next - target) < 1e-3f ? target : next;
}

}  // namespace glasswave::ui
