#pragma once

#include "cache/store.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::cache::audio {

// Bump when the encoded-byte producer changes semantics. The original Apple cache used
// this value, so existing v10 bytes and new provider-neutral v10 bytes keep one identity.
inline constexpr std::string_view kProducerSalt = "cache-v10";
inline constexpr std::size_t kMaxWorkers = 3;
inline constexpr std::size_t kMaxPrefetchFlights = kMaxWorkers - 1;

enum class Workload {
    Inherit,
    Playback,
    Prefetch,
};

// A prefetch worker marks the current thread while it asks the core to open an input
// decoder. Inputs inherit the mark when they call OpenOrStart, which reserves worker
// capacity for realtime playback.
class PrefetchScope {
  public:
    PrefetchScope();
    ~PrefetchScope();
    PrefetchScope(const PrefetchScope&) = delete;
    PrefetchScope& operator=(const PrefetchScope&) = delete;
};

bool IsPrefetchThread() noexcept;

// These fields describe the actual delivered encoded source. They must never contain a
// signed URL, token, cookie or other credential-like identity.
struct Source {
    std::string provider;
    std::string accountScope;
    std::string identity;
    std::string quality;
    std::string codec;
    std::string sourceVersion;
};

Key MakeKey(Source source);

// What a source says about the bytes it delivers: everything that names the song's cache entry
// and describes it. NameSource turns it into the key and metadata of OpenOrStart's Request,
// scoped to the current account (a process-local "ephemeral-<pid>" scope, with `persistent`
// false, when no account is known).
struct SourceName {
    std::string provider;
    std::string identity;
    std::string quality;
    std::string codec;
    std::string sourceVersion;
    std::string contentType;
    // Appended to the entry's metadata after "schema=audio-v1;producer=...": ";codec=..." fields
    // for whatever else the producer wants to tell about the bytes.
    std::string extra;
};
struct NamedSource {
    Key key;
    Metadata metadata;
    bool persistent = true;
};
NamedSource NameSource(SourceName name);

class Sink {
  public:
    virtual ~Sink() = default;
    virtual void Write(const void* bytes, std::size_t size, abort_callback& abort) = 0;
    virtual uint64_t Size() const noexcept = 0;
};

using Fetcher = std::function<void(Sink& sink, abort_callback& abort)>;

class Flight;

struct Request {
    Key key;
    Metadata metadata;
    Fetcher fetch;
    Workload workload = Workload::Inherit;
    bool persistent = true;
    // Only use what is already there: a complete cached generation or a running transfer of
    // this key. Never starts one (fetch is not called); a miss returns admitted=false.
    bool cachedOnly = false;
};

// The source a track resolved to the last time it was opened: whatever its input needs to
// name the cache entry again (level, codec, bit rate, ...), as a small opaque text. A track
// that is in the cache can then be opened without asking the service which source it would
// deliver, so cached music starts at once and plays without a connection. `request` is what
// was asked for (the quality setting), so another setting resolves afresh. Scoped to the
// current account; never contains a URL or a credential. Call from a decode worker.
// The description is a list of text fields, stored length-prefixed: a field may hold any byte.
std::optional<std::vector<std::string>> RecallSource(const std::string& provider,
                                                     const std::string& identity,
                                                     const std::string& request);
void RememberSource(const std::string& provider, const std::string& identity, const std::string& request,
                    const std::vector<std::string>& description);
void ForgetSource(const std::string& provider, const std::string& identity,
                  const std::string& request) noexcept;

struct Opened {
    file::ptr stream;
    std::shared_ptr<Flight> flight;
    bool fromCache = false;
    bool admitted = false;
};

// Returns a cache-hit seekable stream, or starts/adopts one per-key transfer and returns a
// progressive sequential stream. Holding the Flight keeps the transfer alive after a
// prefetch decoder is closed. A later playback open adopts the same flight and promotes it.
// Prefetch may return admitted=false once kMaxPrefetchFlights is reached; playback never
// uses that soft refusal. Call from a provider/decode worker, never from painting.
Opened OpenOrStart(Request request, abort_callback& abort);

// Adopt an existing flight without starting a transfer. Returns null when no active flight
// exists. Use Workload::Playback to turn a prefetch flight into a realtime one.
std::shared_ptr<Flight> Adopt(const Key& key, Workload workload = Workload::Inherit);

class Flight {
  public:
    ~Flight();
    Flight(const Flight&) = delete;
    Flight& operator=(const Flight&) = delete;

    file::ptr Sequential() const;
    file::ptr Completed(abort_callback& abort) const;
    bool Done() const noexcept;
    bool Failed() const noexcept;
    uint64_t Available() const noexcept;
    void Promote();
    // A decoder rejected the bytes. Stop an active producer and make all future lookups
    // miss. Existing immutable/staging handles remain valid for the current playback.
    void Invalidate() noexcept;

  private:
    struct Impl;
    class View;
    explicit Flight(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
    friend class Manager;
};

std::size_t ActiveFlights();
std::size_t ActivePrefetchFlights();

// Invalidates a completed cache hit or an entry whose Flight is no longer reachable.
void Invalidate(const Key& key) noexcept;

// Component shutdown: abort every transfer, destroy queued producers and join all workers.
// It is idempotent.
void Shutdown();

} // namespace glasswave::cache::audio
