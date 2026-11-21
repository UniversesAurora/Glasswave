#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace glasswave::core {

// A listener registry for one thread (in practice the main thread): no locking.
//
// Emit calls every listener that is connected when it starts and still connected when its turn
// comes: a listener removed by an earlier listener of the same emission is skipped, a listener
// added during an emission waits for the next one, and a listener may disconnect itself or
// others, or emit again, without invalidating anything.
template <class... Args> class Signal {
    struct State {
        std::map<uint64_t, std::function<void(Args...)>> slots;
        uint64_t next = 1;
    };

  public:
    using Token = uint64_t;  // never 0, so callers may use 0 for "not connected"

    // Disconnects on destruction or Disconnect(); safe to outlive the signal.
    class Connection {
      public:
        Connection() = default;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&& other) noexcept
            : m_state(std::move(other.m_state)), m_token(std::exchange(other.m_token, 0)) {}
        Connection& operator=(Connection&& other) noexcept {
            if (this != &other) {
                Disconnect();
                m_state = std::move(other.m_state);
                m_token = std::exchange(other.m_token, 0);
            }
            return *this;
        }
        ~Connection() { Disconnect(); }

        void Disconnect() {
            if (const auto state = m_state.lock()) state->slots.erase(m_token);
            m_state.reset();
            m_token = 0;
        }
        bool Connected() const {
            const auto state = m_state.lock();
            return state && state->slots.count(m_token) != 0;
        }

      private:
        friend class Signal;
        Connection(std::weak_ptr<State> state, Token token) : m_state(std::move(state)), m_token(token) {}
        std::weak_ptr<State> m_state;
        Token m_token = 0;
    };

    Signal() : m_state(std::make_shared<State>()) {}
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    // Token form, for APIs that hand tokens to their callers.
    Token Add(std::function<void(Args...)> listener) {
        const Token token = m_state->next++;
        m_state->slots.emplace(token, std::move(listener));
        return token;
    }
    void Remove(Token token) { m_state->slots.erase(token); }

    // RAII form.
    Connection Connect(std::function<void(Args...)> listener) {
        return Connection(m_state, Add(std::move(listener)));
    }

    void Clear() { m_state->slots.clear(); }
    bool Empty() const { return m_state->slots.empty(); }

    void Emit(Args... args) {
        // The state outlives this call even when a listener destroys the signal's owner.
        const auto state = m_state;
        std::vector<std::pair<Token, std::function<void(Args...)>>> snapshot(state->slots.begin(),
                                                                              state->slots.end());
        for (const auto& [token, listener] : snapshot) {
            if (state->slots.count(token) == 0) continue;
            if (listener) listener(args...);
        }
    }

  private:
    std::shared_ptr<State> m_state;
};

}  // namespace glasswave::core
