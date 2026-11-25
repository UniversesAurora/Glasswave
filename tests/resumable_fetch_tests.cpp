#include "pch.h"
#include "net/resumable_fetch.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace core_api {
bool are_services_available() {
    return false;
}
bool is_main_thread() {
    return true;
}
} // namespace core_api

namespace glasswave::core {
void Log(std::wstring_view) {}
std::wstring Wide(std::string_view utf8) {
    return std::wstring(utf8.begin(), utf8.end());
}
} // namespace glasswave::core

namespace {

using namespace glasswave::net;
using Clock = std::chrono::steady_clock;

int g_checks = 0;

class TestAbort final : public abort_callback {
  public:
    TestAbort() : m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~TestAbort() {
        if (m_event) CloseHandle(m_event);
    }
    void abort() noexcept {
        m_aborted.store(true, std::memory_order_release);
        if (m_event) SetEvent(m_event);
    }
    bool is_aborting() const override { return m_aborted.load(std::memory_order_acquire); }
    abort_callback_event get_abort_event() const override { return m_event; }

  private:
    std::atomic<bool> m_aborted{false};
    HANDLE m_event = nullptr;
};

void Check(bool condition, const char* message) {
    ++g_checks;
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

class FakeHttpReply : public file, public http_reply {
  public:
    struct Config {
        std::string status = "200 OK";
        std::string contentRange;
        t_filesize declaredSize = filesize_invalid;
        std::string payload;
        size_t stallAfterBytes = std::string::npos;
        int slowDelayMs = 0;
        size_t slowAfterBytes = std::string::npos;
    };

    explicit FakeHttpReply(Config config) : m_config(std::move(config)) {}

    // service_base
    int service_add_ref() noexcept override { return ++m_refs; }
    int service_release() noexcept override {
        const int left = --m_refs;
        if (left == 0) delete this;
        return left;
    }
    bool service_query(service_ptr& out, const GUID& guid) override {
        if (guid == file::class_guid) {
            out = static_cast<file*>(this);
            return true;
        }
        if (guid == http_reply::class_guid) {
            out = static_cast<http_reply*>(this);
            return true;
        }
        return false;
    }

    // http_reply
    void get_status(pfc::string_base& out) override { out = m_config.status.c_str(); }
    bool get_http_header(const char* name, pfc::string_base& out) override {
        if (_stricmp(name, "content-range") == 0 && !m_config.contentRange.empty()) {
            out = m_config.contentRange.c_str();
            return true;
        }
        return false;
    }
    bool get_http_header_multi(const char* name, pfc::string_base& out) override {
        return get_http_header(name, out);
    }

    // file
    t_size read(void* buffer, t_size bytes, abort_callback& abort) override {
        abort.check();
        if (m_pos == m_config.stallAfterBytes) {
            // Simulate a half-open / hung TCP socket waiting until abort fires.
            const HANDLE ev = abort.get_abort_event();
            WaitForSingleObject(ev, 5000);
            abort.check();
            throw exception_io_data("unexpected test wait completion");
        }
        if (m_pos == m_config.slowAfterBytes && m_config.slowDelayMs > 0) {
            abort.sleep(static_cast<double>(m_config.slowDelayMs) / 1000.0);
            m_config.slowDelayMs = 0;
        }
        if (m_pos >= m_config.payload.size()) return 0;
        size_t limit = m_config.payload.size() - m_pos;
        if (m_config.stallAfterBytes > m_pos)
            limit = std::min(limit, m_config.stallAfterBytes - m_pos);
        if (m_config.slowDelayMs > 0 && m_config.slowAfterBytes > m_pos)
            limit = std::min(limit, m_config.slowAfterBytes - m_pos);
        const size_t count = std::min<size_t>(bytes, limit);
        std::memcpy(buffer, m_config.payload.data() + m_pos, count);
        m_pos += count;
        return count;
    }
    void write(const void*, t_size, abort_callback&) override { throw exception_io_denied(); }
    t_filesize get_size(abort_callback&) override { return m_config.declaredSize; }
    t_filesize get_position(abort_callback&) override { return m_pos; }
    void resize(t_filesize, abort_callback&) override { throw exception_io_denied(); }
    void seek(t_filesize, abort_callback&) override { throw exception_io_object_not_seekable(); }
    bool can_seek() override { return false; }
    bool get_content_type(pfc::string_base& out) override {
        out = "audio/flac";
        return true;
    }
    void reopen(abort_callback&) override { m_pos = 0; }
    bool is_remote() override { return true; }
    void on_idle(abort_callback& abort) override { abort.check(); }
    t_filetimestamp get_timestamp(abort_callback&) override { return filetimestamp_invalid; }

  private:
    Config m_config;
    size_t m_pos = 0;
    std::atomic<int> m_refs{0};
};

std::string MakePayload(size_t size) {
    std::string out(size, '\0');
    for (size_t i = 0; i < size; ++i)
        out[i] = static_cast<char>('A' + (i % 26));
    return out;
}

void TestMidReadStallResumesCleanly() {
    const std::string expected = MakePayload(100);
    int opens = 0;
    std::vector<std::string> logs;

    ResumableSource source;
    source.label = "NetEase";
    source.target.url = "https://cdn.example/song.flac";

    FetchPolicy policy;
    policy.openTimeoutMs = 150;
    policy.readStallTimeoutMs = 80;
    policy.retryBackoffSeconds = 0.01;

    std::string received;
    TestAbort abort;
    const auto start = Clock::now();
    detail::FetchResumableWithOpener(
        source,
        [&](const void* data, size_t size, abort_callback&) {
            received.append(static_cast<const char*>(data), size);
        },
        abort, policy,
        [&](const FetchTarget&, uint64_t from, abort_callback&) -> file::ptr {
            ++opens;
            if (opens == 1) {
                Check(from == 0, "first open must start at byte 0");
                FakeHttpReply::Config cfg;
                cfg.status = "200 OK";
                cfg.declaredSize = expected.size();
                cfg.payload = expected;
                cfg.stallAfterBytes = 40;
                return file::ptr(new FakeHttpReply(std::move(cfg)));
            }
            Check(from == 40, "resume must request exact Range offset 40");
            FakeHttpReply::Config cfg;
            cfg.status = "206 Partial Content";
            cfg.contentRange = "bytes 40-99/100";
            cfg.declaredSize = 60;
            cfg.payload = expected.substr(40);
            return file::ptr(new FakeHttpReply(std::move(cfg)));
        },
        [&](const std::string& line) { logs.push_back(line); });

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    Check(opens == 2, "mid-read stall must recover in 2 opens");
    Check(received == expected, "resumed payload must match original bytes exactly");
    Check(elapsedMs < 1000, "stall recovery must complete promptly");
    Check(!logs.empty() && logs.front().find("stalled at 40 of 100 bytes; reconnecting") != std::string::npos,
          "stall recovery must log stalled offset");
}

void TestOpenTimeoutRenewsAndSucceeds() {
    const std::string expected = MakePayload(64);
    int opens = 0;
    int renews = 0;
    std::vector<std::string> logs;

    ResumableSource source;
    source.label = "NetEase";
    source.target.url = "https://stale.example/song.flac";
    source.renew = [&](abort_callback&) {
        ++renews;
        return FetchTarget{"https://fresh.example/song.flac", {}};
    };

    FetchPolicy policy;
    policy.openTimeoutMs = 80;
    policy.readStallTimeoutMs = 80;
    policy.retryBackoffSeconds = 0.01;

    std::string received;
    TestAbort abort;
    detail::FetchResumableWithOpener(
        source,
        [&](const void* data, size_t size, abort_callback&) {
            received.append(static_cast<const char*>(data), size);
        },
        abort, policy,
        [&](const FetchTarget& target, uint64_t from, abort_callback& attemptAbort) -> file::ptr {
            ++opens;
            Check(from == 0, "open timeout retry must still start at byte 0");
            if (opens == 1) {
                WaitForSingleObject(attemptAbort.get_abort_event(), 5000);
                attemptAbort.check();
                throw exception_io_data("open should have aborted on timeout");
            }
            Check(target.url == "https://fresh.example/song.flac", "retry must use renewed URL");
            FakeHttpReply::Config cfg;
            cfg.status = "200 OK";
            cfg.declaredSize = expected.size();
            cfg.payload = expected;
            return file::ptr(new FakeHttpReply(std::move(cfg)));
        },
        [&](const std::string& line) { logs.push_back(line); });

    Check(opens == 2, "open timeout should succeed on second attempt");
    Check(renews == 1, "open timeout should renew URL once");
    Check(received == expected, "renewed open must deliver complete bytes");
}

void TestOuterAbortPropagatesImmediately() {
    int opens = 0;
    ResumableSource source;
    source.label = "NetEase";
    source.target.url = "https://cdn.example/song.flac";

    FetchPolicy policy;
    policy.openTimeoutMs = 500;
    policy.readStallTimeoutMs = 500;
    policy.retryBackoffSeconds = 0.01;

    TestAbort abort;
    std::jthread canceller([&] {
        Sleep(30);
        abort.abort();
    });

    bool aborted = false;
    const auto start = Clock::now();
    try {
        detail::FetchResumableWithOpener(
            source, [](const void*, size_t, abort_callback&) {}, abort, policy,
            [&](const FetchTarget&, uint64_t, abort_callback&) -> file::ptr {
                ++opens;
                FakeHttpReply::Config cfg;
                cfg.status = "200 OK";
                cfg.declaredSize = 100;
                cfg.payload = MakePayload(100);
                cfg.stallAfterBytes = 10;
                return file::ptr(new FakeHttpReply(std::move(cfg)));
            },
            nullptr);
    } catch (const exception_aborted&) {
        aborted = true;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    Check(aborted, "caller abort must throw exception_aborted");
    Check(opens == 1, "caller abort must not retry");
    Check(elapsedMs < 350, "caller abort must not wait for stall timeout");
}

void TestRepeatedStallsFailWithIoData() {
    int opens = 0;
    ResumableSource source;
    source.label = "NetEase";
    source.target.url = "https://cdn.example/song.flac";

    FetchPolicy policy;
    policy.maxStalledResumes = 2;
    policy.openTimeoutMs = 80;
    policy.readStallTimeoutMs = 60;
    policy.retryBackoffSeconds = 0.01;

    TestAbort abort;
    bool failedWithIoData = false;
    std::string errorMessage;
    try {
        detail::FetchResumableWithOpener(
            source, [](const void*, size_t, abort_callback&) {}, abort, policy,
            [&](const FetchTarget&, uint64_t from, abort_callback&) -> file::ptr {
                ++opens;
                FakeHttpReply::Config cfg;
                cfg.status = from == 0 ? "200 OK" : "206 Partial Content";
                if (from > 0) cfg.contentRange = "bytes " + std::to_string(from) + "-99/100";
                cfg.declaredSize = 100 - from;
                cfg.payload = MakePayload(static_cast<size_t>(100 - from));
                cfg.stallAfterBytes = (opens == 1) ? 16 : 0;
                return file::ptr(new FakeHttpReply(std::move(cfg)));
            },
            nullptr);
    } catch (const exception_io_data& e) {
        failedWithIoData = true;
        errorMessage = e.what();
    }

    Check(failedWithIoData, "repeated stalls must throw exception_io_data (not exception_aborted)");
    Check(errorMessage.find("timed out") != std::string::npos, "error message must mention timeout");
    Check(opens == 4, "1 initial progress + 3 zero-progress stalled retries (maxStalledResumes=2)");
}

void TestUnknownLengthChunkedStreamDoesNotFalseTimeoutMidBody() {
    const std::string expected = MakePayload(50);
    int opens = 0;
    ResumableSource source;
    source.label = "Apple Music";
    source.target.url = "http://127.0.0.1/audio";

    FetchPolicy policy;
    policy.openTimeoutMs = 150;
    policy.readStallTimeoutMs = 60;
    policy.retryBackoffSeconds = 0.01;

    std::string received;
    TestAbort abort;
    detail::FetchResumableWithOpener(
        source,
        [&](const void* data, size_t size, abort_callback&) {
            received.append(static_cast<const char*>(data), size);
        },
        abort, policy,
        [&](const FetchTarget&, uint64_t, abort_callback&) -> file::ptr {
            ++opens;
            FakeHttpReply::Config cfg;
            cfg.status = "200 OK";
            cfg.declaredSize = filesize_invalid;
            cfg.payload = expected;
            cfg.slowAfterBytes = 20;
            cfg.slowDelayMs = 120; // longer than readStallTimeoutMs (60 ms)
            return file::ptr(new FakeHttpReply(std::move(cfg)));
        },
        nullptr);

    Check(opens == 1, "unknown-length chunked stream must not abort mid-body");
    Check(received == expected, "unknown-length stream must receive all bytes");
}

} // namespace

int main() {
    TestMidReadStallResumesCleanly();
    TestOpenTimeoutRenewsAndSucceeds();
    TestOuterAbortPropagatesImmediately();
    TestRepeatedStallsFailWithIoData();
    TestUnknownLengthChunkedStreamDoesNotFalseTimeoutMidBody();
    std::cout << "resumable_fetch_tests: " << g_checks << " checks passed\n";
    return 0;
}
