#include "pch.h"
#include "net/resumable_fetch.h"

#include "core/log.h"
#include "core/text.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace glasswave::net {
namespace {

constexpr size_t kReadBytes = 64 * 1024;

// Lightweight abort_callback for standalone contexts where foobar2000 services are not loaded.
class LocalAbort final : public abort_callback {
  public:
    LocalAbort() : m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~LocalAbort() {
        if (m_event) CloseHandle(m_event);
    }
    LocalAbort(const LocalAbort&) = delete;
    LocalAbort& operator=(const LocalAbort&) = delete;

    void abort() noexcept {
        m_aborted.store(true, std::memory_order_release);
        if (m_event) SetEvent(m_event);
    }
    void reset() noexcept {
        m_aborted.store(false, std::memory_order_release);
        if (m_event) ResetEvent(m_event);
    }
    bool is_aborting() const override { return m_aborted.load(std::memory_order_acquire); }
    abort_callback_event get_abort_event() const override { return m_event; }

  private:
    std::atomic<bool> m_aborted{false};
    HANDLE m_event = nullptr;
};

// Per-attempt abort_callback that forwards caller cancellation and adds a one-shot stall timer
// via the Windows thread pool (zero dedicated threads, deterministic wait-and-drain teardown).
class AttemptAbort {
  public:
    explicit AttemptAbort(abort_callback& outer) : m_outer(outer) {
        if (core_api::are_services_available()) m_sdkInner.emplace();
        m_timer = CreateThreadpoolTimer(OnTimer, this, nullptr);
        if (m_outer.is_aborting()) {
            AbortInner();
            return;
        }
        const HANDLE outerEvent = m_outer.get_abort_event();
        if (outerEvent && outerEvent != INVALID_HANDLE_VALUE) {
            RegisterWaitForSingleObject(&m_wait, outerEvent, OnOuterAbort, this, INFINITE,
                                        WT_EXECUTEONLYONCE | WT_EXECUTEDEFAULT);
        }
        if (m_outer.is_aborting()) AbortInner();
    }

    ~AttemptAbort() {
        Disarm();
        if (m_timer) CloseThreadpoolTimer(m_timer);
        if (m_wait) UnregisterWaitEx(m_wait, INVALID_HANDLE_VALUE);
    }

    AttemptAbort(const AttemptAbort&) = delete;
    AttemptAbort& operator=(const AttemptAbort&) = delete;

    abort_callback& Callback() noexcept {
        return m_sdkInner ? static_cast<abort_callback&>(*m_sdkInner)
                          : static_cast<abort_callback&>(m_localInner);
    }
    bool TimedOut() const noexcept { return m_timedOut.load(std::memory_order_acquire); }

    void Arm(int timeoutMs) noexcept {
        if (timeoutMs <= 0 || !m_timer) return;
        const LONGLONG due100ns = -static_cast<LONGLONG>(timeoutMs) * 10'000LL;
        FILETIME dueTime{};
        dueTime.dwLowDateTime = static_cast<DWORD>(due100ns & 0xFFFFFFFFULL);
        dueTime.dwHighDateTime = static_cast<DWORD>(static_cast<ULONGLONG>(due100ns) >> 32);
        SetThreadpoolTimer(m_timer, &dueTime, 0, 0);
    }

    void Disarm() noexcept {
        if (!m_timer) return;
        SetThreadpoolTimer(m_timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(m_timer, TRUE);
    }

    // Clears a timer that fired just as an I/O call finished with progress, while preserving any
    // genuine caller abort. Must be called only while disarmed.
    void ResetAfterProgress() noexcept {
        if (m_outer.is_aborting()) {
            AbortInner();
            return;
        }
        if (!m_timedOut.load(std::memory_order_acquire) && !Callback().is_aborting()) return;
        m_timedOut.store(false, std::memory_order_release);
        if (m_sdkInner)
            m_sdkInner->reset();
        else
            m_localInner.reset();
        if (m_outer.is_aborting()) AbortInner();
    }

  private:
    void AbortInner() noexcept {
        if (m_sdkInner)
            m_sdkInner->abort();
        else
            m_localInner.abort();
    }

    static VOID CALLBACK OnTimer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
        auto* self = static_cast<AttemptAbort*>(context);
        self->m_timedOut.store(true, std::memory_order_release);
        self->AbortInner();
    }

    static VOID CALLBACK OnOuterAbort(PVOID context, BOOLEAN) {
        auto* self = static_cast<AttemptAbort*>(context);
        self->AbortInner();
    }

    abort_callback& m_outer;
    std::optional<abort_callback_impl> m_sdkInner;
    LocalAbort m_localInner;
    std::atomic<bool> m_timedOut{false};
    PTP_TIMER m_timer = nullptr;
    HANDLE m_wait = nullptr;
};

struct ArmedGuard {
    AttemptAbort& attempt;
    ArmedGuard(AttemptAbort& target, int timeoutMs) : attempt(target) { attempt.Arm(timeoutMs); }
    ~ArmedGuard() { attempt.Disarm(); }
};

// The status code of a reply ("206 Partial Content"), 0 when there is none.
int ReplyStatus(const file::ptr& reply) {
    http_reply::ptr http;
    if (!reply->service_query_t(http)) return 0;
    pfc::string8 status;
    http->get_status(status);
    const char* text = status.get_ptr();
    while (*text && (*text < '0' || *text > '9'))
        ++text;
    return std::atoi(text);
}

// The length a reply states; filesize_invalid when it states none.
t_filesize ReplyLength(const file::ptr& reply, abort_callback& abort) {
    try {
        return reply->get_size(abort);
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        return filesize_invalid;
    }
}

double RetryAfterSeconds(const file::ptr& reply, const FetchPolicy& policy) {
    http_reply::ptr http;
    pfc::string8 value;
    long seconds = 1;
    if (reply->service_query_t(http) && http->get_http_header("retry-after", value))
        seconds = std::atol(value.get_ptr());
    return static_cast<double>(std::clamp<long>(seconds, 1, policy.maxRetryAfterSeconds));
}

file::ptr Open(const FetchTarget& target, uint64_t from, abort_callback& abort) {
    auto request = http_client::get()->create_request("GET");
    for (const auto& [name, value] : target.headers)
        request->add_header(name.c_str(), value.c_str());
    if (!from) return request->run(target.url.c_str(), abort);
    request->add_header("Range", ("bytes=" + std::to_string(from) + "-").c_str());
    return request->run_ex(target.url.c_str(), abort);
}

void DefaultLog(const std::string& line) {
    console::print(("Glasswave " + line).c_str());
    core::Log(core::Wide("input: " + line));
}

} // namespace

namespace detail {

void FetchResumableWithOpener(const ResumableSource& source, const ByteSink& sink, abort_callback& abort,
                              const FetchPolicy& policy, const RequestOpener& opener, const LogSink& log) {
    const auto note = [&](const std::string& text) {
        if (log) log(source.label + ": " + text);
    };
    const auto fail = [&](const std::string& why) {
        throw exception_io_data(("The " + source.label + " audio transfer " + why).c_str());
    };
    const auto backoff = [&] {
        if (policy.retryBackoffSeconds > 0) abort.sleep(policy.retryBackoffSeconds);
    };

    FetchTarget target = source.target;
    uint64_t openedAt = GetTickCount64();
    bool renewedSinceProgress = false;
    const auto renew = [&](const std::string& why) {
        target = source.renew(abort);
        openedAt = GetTickCount64();
        renewedSinceProgress = true;
        note("renewed the stream address (" + why + ")");
    };

    // The length of the whole song, stated by the first reply: a connection that closes before
    // it is a failed transfer, not a whole song (kept as one, the decoder met the cut 12 s in
    // and called the file corrupt).
    t_filesize expected = filesize_invalid;
    uint64_t received = 0;
    double waited = 0;
    int stalled = 0, resumes = 0;
    std::array<uint8_t, kReadBytes> buffer{};
    for (;;) {
        abort.check();
        const bool resuming = received > 0;
        uint64_t skip = 0;
        // Declare attempt before reply so reply closes its socket before attempt tears down.
        AttemptAbort attempt(abort);
        file::ptr reply;
        try {
            ArmedGuard openGuard(attempt, policy.openTimeoutMs);
            reply = opener(target, received, attempt.Callback());
            openedAt = GetTickCount64();
        } catch (const exception_aborted&) {
            abort.check();
            if (!attempt.TimedOut()) throw;
            if (resuming) {
                note("reconnect timed out at " + std::to_string(received) + " of " +
                     std::to_string(expected) + " bytes; retrying");
            } else {
                note("request timed out; retrying");
            }
            if (source.renew && !renewedSinceProgress) {
                renew("the request timed out");
                continue;
            }
            if (++stalled > policy.maxStalledResumes || (resuming && ++resumes > policy.maxResumes))
                fail("timed out while connecting");
            backoff();
            continue;
        } catch (...) {
            abort.check();
            if (source.renew && !renewedSinceProgress) {
                renew("the request failed");
                continue;
            }
            if (!resuming || ++stalled > policy.maxStalledResumes || ++resumes > policy.maxResumes) throw;
            backoff();
            continue;
        }
        attempt.ResetAfterProgress();
        if (!resuming) {
            if (expected == filesize_invalid) expected = ReplyLength(reply, abort);
        } else {
            const int status = ReplyStatus(reply);
            if (status == 503) { // the server is still preparing the bytes
                const double wait = RetryAfterSeconds(reply, policy);
                if (waited + wait <= policy.maxCachingWaitSeconds) {
                    waited += wait;
                    abort.sleep(wait);
                    continue;
                }
            }
            pfc::string8 range;
            http_reply::ptr http;
            if (status == 206 && reply->service_query_t(http) &&
                http->get_http_header("content-range", range)) {
                // "bytes <first>-<last>/<total>": it must start where this one stopped.
                unsigned long long first = 0, last = 0, total = 0;
                if (sscanf_s(range.get_ptr(), "bytes %llu-%llu/%llu", &first, &last, &total) != 3 ||
                    first != received || total != expected)
                    fail("could not continue (range " + std::string(range.get_ptr()) + ")");
            } else if (status == 200) {
                if (ReplyLength(reply, abort) != expected) fail("could not continue (another length)");
                skip = received;
            } else if (status >= 400 && source.renew && !renewedSinceProgress) {
                renew("HTTP " + std::to_string(status));
                continue;
            } else {
                fail("could not continue (HTTP " + std::to_string(status) + ")");
            }
        }

        const uint64_t before = received;
        bool renewed = false;
        bool retried = false;
        bool readTimedOut = false;
        for (;;) {
            size_t count = 0;
            // Only arm the per-read stall timer when we can cleanly recover (known length for Range
            // resume, or before any bytes have been emitted to the sink).
            const int readTimeoutMs =
                (expected != filesize_invalid || received == 0) ? policy.readStallTimeoutMs : 0;
            try {
                ArmedGuard readGuard(attempt, readTimeoutMs);
                count = reply->read(buffer.data(), buffer.size(), attempt.Callback());
            } catch (const exception_aborted&) {
                abort.check();
                if (!attempt.TimedOut()) throw;
                readTimedOut = true;
                if (expected == filesize_invalid) {
                    if (received == 0) {
                        if (source.renew && !renewedSinceProgress) {
                            renew("the stream stalled before any bytes arrived");
                            renewed = true;
                            break;
                        }
                        if (++stalled <= policy.maxStalledResumes) {
                            note("stream stalled before any bytes arrived; retrying");
                            retried = true;
                            break;
                        }
                    }
                    fail("timed out before any bytes arrived");
                }
                break;
            } catch (...) {
                abort.check();
                // A connection that breaks mid-read is a cut like one that closes early; without
                // a stated length there is nothing to continue towards, unless no bytes arrived
                // yet and the source can open a fresh stream. Only the read is a cut: a failing
                // sink is not retried.
                if (expected == filesize_invalid) {
                    if (received == 0 && source.renew && !renewedSinceProgress) {
                        renew("the read failed before any bytes arrived");
                        renewed = true;
                        break;
                    }
                    throw;
                }
                break;
            }
            attempt.ResetAfterProgress();
            if (!count) break;
            const auto drop = static_cast<size_t>(std::min<uint64_t>(skip, count));
            skip -= drop;
            if (count > drop) {
                sink(buffer.data() + drop, count - drop, abort);
                received += count - drop;
                waited = 0;
                stalled = 0;
                renewedSinceProgress = false;
            }
            // A signed address about to expire is replaced while the transfer runs.
            if (source.renew && source.renewAfterMs && expected != filesize_invalid && received < expected &&
                GetTickCount64() - openedAt >= source.renewAfterMs) {
                renew("it is about to expire");
                renewed = true;
                break;
            }
        }
        reply.release();
        if (renewed) continue;
        if (retried) {
            backoff();
            continue;
        }
        // Nothing came at all: the address may have expired, so ask for a fresh one before counting.
        if (received == 0 && source.renew && !renewedSinceProgress) {
            renew(readTimedOut ? "the stream stalled before any bytes arrived" : "the reply was empty");
            continue;
        }
        if (expected == filesize_invalid || received >= expected) return;
        stalled = received > before ? 0 : stalled + 1;
        if (stalled > policy.maxStalledResumes || ++resumes > policy.maxResumes) {
            fail((readTimedOut ? "timed out (" : "ended early (") + std::to_string(received) + " of " +
                 std::to_string(expected) + " bytes)");
        }
        if (received == before && source.renew && !renewedSinceProgress) {
            renew(readTimedOut ? "the stream stalled" : "the transfer made no progress");
        }
        note((readTimedOut ? "audio transfer stalled at " : "audio transfer cut at ") +
             std::to_string(received) + " of " + std::to_string(expected) +
             (readTimedOut ? " bytes; reconnecting" : " bytes; continuing"));
        if (received == before) backoff();
    }
}

} // namespace detail

void FetchResumable(const ResumableSource& source, const ByteSink& sink, abort_callback& abort,
                    const FetchPolicy& policy) {
    detail::FetchResumableWithOpener(source, sink, abort, policy, Open, DefaultLog);
}

} // namespace glasswave::net
