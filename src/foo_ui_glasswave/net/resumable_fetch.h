#pragma once

#include <SDK/foobar2000.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// One HTTP fetcher for the encoded bytes of a streaming source's song: it reads the reply into
// a sink and, when the connection is cut, asks again for the rest with a Range request. A source
// supplies only where the bytes are (URL and headers) and, when its URL expires, how to get a
// fresh one.
namespace glasswave::net {

struct FetchTarget {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct ResumableSource {
    // Names the source in log lines and errors ("Apple Music", "NetEase").
    std::string label;
    // Where the bytes are now. The fetcher adds `Range: bytes=N-` when it asks for the rest.
    FetchTarget target;
    // Optional: a fresh target (NetEase's signed URL expires). Called when a request fails or
    // the transfer has brought nothing, once until bytes arrive again, and `renewAfterMs`
    // after the current target was opened. It throws when no usable target exists.
    std::function<FetchTarget(abort_callback& abort)> renew;
    // Optional, with `renew`: the age of a target after which it is renewed before it expires
    // (0: only on failure). The transfer continues from the bytes already received.
    uint64_t renewAfterMs = 0;
};

struct FetchPolicy {
    // A server still preparing the bytes answers a range with 503; wait for it, Retry-After
    // seconds each time (1 s when it gives none, at most maxRetryAfterSeconds), this long in all.
    // A lossless song caches in 5-15 s.
    int maxCachingWaitSeconds = 90;
    int maxRetryAfterSeconds = 10;
    // Resumes in a row that bring no byte, and resumes in all: a link that delivers a little
    // each time must end as well.
    int maxStalledResumes = 3;
    int maxResumes = 20;
};

using ByteSink = std::function<void(const void* bytes, size_t size, abort_callback& abort)>;

// Adapts any sink with Write(bytes, size, abort) (cache::audio::Sink) to a ByteSink.
template <class Sink> ByteSink WriteTo(Sink& sink) {
    return [&sink](const void* bytes, size_t size, abort_callback& abort) { sink.Write(bytes, size, abort); };
}

// Reads the whole body into `sink`. The length stated by the first reply (Content-Length) is
// the song's: a connection that closes before it is a failed transfer, not a whole song, and
// continues with a range request. The reply to it must be that range (206, Content-Range
// "bytes N-.../total" with the same total) or the whole body again (200, same length; the bytes
// already received are skipped). Only a failed read is a cut: a failing sink (disk full, cache
// limit) propagates at once. Throws exception_io_data when the transfer cannot continue and
// exception_aborted on abort.
void FetchResumable(const ResumableSource& source, const ByteSink& sink, abort_callback& abort,
                    const FetchPolicy& policy = {});

} // namespace glasswave::net
