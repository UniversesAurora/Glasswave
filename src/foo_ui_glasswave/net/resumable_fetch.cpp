#include "pch.h"
#include "net/resumable_fetch.h"

#include "core/log.h"
#include "core/text.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace glasswave::net {
namespace {

constexpr size_t kReadBytes = 64 * 1024;

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

} // namespace

void FetchResumable(const ResumableSource& source, const ByteSink& sink, abort_callback& abort,
                    const FetchPolicy& policy) {
    const auto note = [&](const std::string& text) {
        const std::string line = source.label + ": " + text;
        console::print(("Glasswave " + line).c_str());
        core::Log(core::Wide("input: " + line));
    };
    const auto fail = [&](const std::string& why) {
        throw exception_io_data(("The " + source.label + " audio transfer " + why).c_str());
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
        file::ptr reply;
        try {
            reply = Open(target, received, abort);
            openedAt = GetTickCount64();
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            if (source.renew && !renewedSinceProgress) {
                renew("the request failed");
                continue;
            }
            if (!resuming || ++stalled > policy.maxStalledResumes) throw;
            abort.sleep(1.0);
            continue;
        }
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
            if (status == 206 && reply->service_query_t(http) && http->get_http_header("content-range", range)) {
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
        for (;;) {
            size_t count = 0;
            try {
                count = reply->read(buffer.data(), buffer.size(), abort);
            } catch (const exception_aborted&) {
                throw;
            } catch (...) {
                // A connection that breaks mid-read is a cut like one that closes early; without
                // a stated length there is nothing to continue towards. Only the read is a cut:
                // a failing sink is not retried.
                if (expected == filesize_invalid) throw;
                break;
            }
            if (!count) break;
            const auto drop = static_cast<size_t>(std::min<uint64_t>(skip, count));
            skip -= drop;
            if (count > drop) {
                sink(buffer.data() + drop, count - drop, abort);
                received += count - drop;
                waited = 0;
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
        if (renewed) continue;
        // Nothing came at all: the address may have expired, so ask for a fresh one before counting.
        if (received == 0 && source.renew && !renewedSinceProgress) {
            renew("the reply was empty");
            continue;
        }
        if (expected == filesize_invalid || received >= expected) return;
        stalled = received > before ? 0 : stalled + 1;
        if (stalled > policy.maxStalledResumes || ++resumes > policy.maxResumes)
            fail("ended early (" + std::to_string(received) + " of " + std::to_string(expected) + " bytes)");
        note("audio transfer cut at " + std::to_string(received) + " of " + std::to_string(expected) +
             " bytes; continuing");
    }
}

} // namespace glasswave::net
