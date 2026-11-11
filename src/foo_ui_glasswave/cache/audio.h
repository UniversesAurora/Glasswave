#pragma once

#include "cache/store.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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
};

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
