#include "pch.h"
#include "cache/images.h"
#include "cache/art_absence.h"
#include "net/http.h"
#include "netease/log.h"

#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>

namespace glasswave::cache {

namespace {

constexpr unsigned kMaxSourceAttempts = 2;
constexpr size_t kWorkerCount = 4;
constexpr size_t kPolicyEntries = 4096;
constexpr size_t kInvalidationEntries = 128;
constexpr uint64_t kMaxCachedBytes = 20u * 1024u * 1024u;
constexpr uint64_t kMaxDecodedPixels = 64u * 1024u * 1024u;
constexpr UINT kMaxDecodedDimension = 32u * 1024u;

thread_local bool t_cacheOnly = false;
thread_local bool t_cacheMiss = false;
thread_local int t_sizeHint = 0;

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_wide_from_utf8(text.data(), text.size()).get_ptr();
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

bool EndsWith(const std::vector<unsigned char>& bytes, std::initializer_list<unsigned char> suffix) {
    if (bytes.size() < suffix.size()) return false;
    size_t at = bytes.size() - suffix.size();
    for (const unsigned char byte : suffix)
        if (bytes[at++] != byte) return false;
    return true;
}

bool IsCompleteImage(const std::vector<unsigned char>& bytes, const std::string& contentType) {
    std::string type = contentType;
    if (const size_t separator = type.find(';'); separator != std::string::npos) type.resize(separator);
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back())))
        type.pop_back();
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool jpeg = bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
    const bool png = bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4e &&
                     bytes[3] == 0x47 && bytes[4] == 0x0d && bytes[5] == 0x0a && bytes[6] == 0x1a &&
                     bytes[7] == 0x0a;
    const bool gif = bytes.size() >= 6 && bytes[0] == 0x47 && bytes[1] == 0x49 && bytes[2] == 0x46 &&
                     bytes[3] == 0x38 && (bytes[4] == 0x37 || bytes[4] == 0x39) && bytes[5] == 0x61;
    const bool webp = bytes.size() >= 12 && bytes[0] == 0x52 && bytes[1] == 0x49 && bytes[2] == 0x46 &&
                      bytes[3] == 0x46 && bytes[8] == 0x57 && bytes[9] == 0x45 && bytes[10] == 0x42 &&
                      bytes[11] == 0x50;
    // An encoded format's magic is authoritative when MIME and payload disagree. Validate
    // the selected format's cheap structural trailer before handing it to WIC.
    if (jpeg) return EndsWith(bytes, {0xff, 0xd9});
    if (png) return EndsWith(bytes, {0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82});
    if (gif) return bytes.back() == 0x3b;
    if (webp) {
        const uint64_t declared = static_cast<uint64_t>(bytes[4]) | static_cast<uint64_t>(bytes[5]) << 8 |
                                  static_cast<uint64_t>(bytes[6]) << 16 |
                                  static_cast<uint64_t>(bytes[7]) << 24;
        return declared + 8 == bytes.size();
    }

    // Other image formats have no cheap trailer check here. Let WIC reject non-images and
    // validate formats it recognizes regardless of a missing or inaccurate MIME subtype.
    return type.rfind("image/", 0) == 0 && !bytes.empty();
}

std::string HeaderValue(const net::HttpResponse& response, std::wstring_view name) {
    for (const auto& [key, value] : response.headers)
        if (_wcsicmp(key.c_str(), std::wstring(name).c_str()) == 0) return Utf8(value);
    return {};
}

bool NoStore(const net::HttpResponse& response) {
    std::string value = HeaderValue(response, L"cache-control");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.find("no-store") != std::string::npos;
}

// The response is a complete file of a format WIC can open, within sane dimensions. The
// pixels are decoded once, by the consumer that draws them; decoding them here as well would
// double the cost of every first download.
bool VerifyImageHeader(IWICImagingFactory* wic, const std::vector<unsigned char>& bytes) {
    if (!wic || bytes.empty() || bytes.size() > UINT32_MAX) return false;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                            decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())))
        return false;

    UINT width = 0, height = 0;
    return SUCCEEDED(frame->GetSize(&width, &height)) && width && height && width <= kMaxDecodedDimension &&
           height <= kMaxDecodedDimension && static_cast<uint64_t>(width) * height <= kMaxDecodedPixels;
}

std::vector<unsigned char> ReadPayload(const std::filesystem::path& path, uint64_t size, size_t limit) {
    if (size == 0 || size > limit || size > static_cast<uint64_t>(SIZE_MAX)) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<size_t>(input.gcount()) != bytes.size()) return {};
    return bytes;
}

bool WritePayload(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return static_cast<bool>(output);
}

int64_t BackoffSeconds(const ImageLimits& limits, unsigned failures) {
    const unsigned shift = failures > 0 ? std::min(failures - 1, 6u) : 0;
    const int64_t limit = std::max<int64_t>(1, limits.retryMaxSeconds);
    const int64_t base = std::min(limit, std::max<int64_t>(1, limits.retryBaseSeconds));
    const int64_t factor = int64_t{1} << shift;
    return base > limit / factor ? limit : base * factor;
}

enum class RunFailureReason {
    aborted,
    resolver_failed,
    resolver_empty,
    transport_error,
    unexpected_status,
    empty_body,
    image_format,
    image_decode,
    account_revision_changed,
    account_scope_changed,
    invalidated,
    cache_publish_unavailable,
    internal_exception,
};

std::wstring_view RunFailureReasonName(RunFailureReason reason) {
    switch (reason) {
    case RunFailureReason::aborted:
        return L"aborted";
    case RunFailureReason::resolver_failed:
        return L"resolver_failed";
    case RunFailureReason::resolver_empty:
        return L"resolver_empty";
    case RunFailureReason::transport_error:
        return L"transport_error";
    case RunFailureReason::unexpected_status:
        return L"unexpected_status";
    case RunFailureReason::empty_body:
        return L"empty_body";
    case RunFailureReason::image_format:
        return L"image_format";
    case RunFailureReason::image_decode:
        return L"image_decode";
    case RunFailureReason::account_revision_changed:
        return L"account_revision_changed";
    case RunFailureReason::account_scope_changed:
        return L"account_scope_changed";
    case RunFailureReason::invalidated:
        return L"invalidated";
    case RunFailureReason::cache_publish_unavailable:
        return L"cache_publish_unavailable";
    case RunFailureReason::internal_exception:
        return L"internal_exception";
    }
    return L"unknown";
}

void LogImageRunDiagnostic(std::string_view artKey, unsigned long status, RunFailureReason reason) noexcept {
    try {
        netease::Log(L"image cache: art=" + Wide(artKey) + L" status=" + std::to_wstring(status) +
                     L" reason=" + std::wstring(RunFailureReasonName(reason)));
    } catch (...) {
        // Diagnostics must never affect image delivery.
    }
}

std::mutex g_instanceMutex;
std::unique_ptr<Images> g_images;
bool g_stopped = false;

} // namespace

std::string ImageArtKey(const ImageIdentity& identity) {
    return identity.provider + ':' + identity.artKind + ':' + identity.id;
}

Key MakeImageKey(const ImageIdentity& identity) {
    Key key;
    key.kind = Kind::Image;
    key.provider = identity.provider;
    key.scope = AccountScope(identity.provider);
    key.identity = identity.artKind + '/' + identity.id;
    key.variant = identity.renditionVersion;
    return key;
}

struct Images::Impl {
    struct Flight;

    struct Policy {
        int64_t nextAttempt = 0;
        unsigned failures = 0;
        uint64_t lastScheduledInvalidation = 0;
        uint64_t lastSeen = 0;
    };

    struct InvalidationRecord {
        std::string prefix;
        uint64_t version = 0;
    };

    struct Job {
        ImageRequest request;
        Key key;
        std::string artKey;
        std::string digest;
        uint64_t accountRevision = 0;
        uint64_t invalidation = 0;
        bool persistent = false;
        std::optional<Snapshot> cached;
        std::shared_ptr<Flight> flight;
    };

    struct Pending {
        ImageRequest request;
        Key key;
        std::string artKey;
        uint64_t accountRevision = 0;
        bool persistent = false;
        std::optional<Snapshot> cached;
    };

    struct Flight {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
        bool success = false;
        bool cancelled = false;
        bool dirty = false;
        uint64_t scheduledInvalidation = 0;
        std::optional<Pending> pending;
        std::vector<unsigned char> bytes;
    };

    struct Outcome {
        bool success = false;
        bool changed = false;
        std::vector<unsigned char> bytes;
        std::string artKey;
        uint64_t accountRevision = 0;
    };

    Impl() {
        for (size_t i = 0; i < kWorkerCount; ++i)
            m_threads.emplace_back([this] { Worker(); });
    }

    void Stop() noexcept {
        {
            std::lock_guard lock(m_mutex);
            if (m_quit) return;
            m_quit = true;
            m_queue.clear();
        }
        m_abort.abort();
        m_wake.notify_all();
        {
            std::lock_guard lock(m_mutex);
            for (const auto& [digest, flight] : m_flights) {
                std::lock_guard flightLock(flight->mutex);
                flight->cancelled = true;
                flight->done = true;
                flight->changed.notify_all();
            }
        }
        for (auto& thread : m_threads)
            if (thread.joinable()) thread.join();
        {
            std::lock_guard lock(m_mutex);
            m_flights.clear();
            m_policies.clear();
            m_invalidations.clear();
            m_listeners.clear();
        }
    }

    ~Impl() { Stop(); }

    uint64_t InvalidationLocked(std::string_view artKey) const {
        uint64_t version = 0;
        for (const auto& item : m_invalidations)
            if (artKey.compare(0, item.prefix.size(), item.prefix) == 0)
                version = std::max(version, item.version);
        return version;
    }

    uint64_t Invalidation(std::string_view artKey) {
        std::lock_guard lock(m_mutex);
        return InvalidationLocked(artKey);
    }

    Policy& PolicyLocked(const std::string& digest) {
        auto& policy = m_policies[digest];
        policy.lastSeen = ++m_clock;
        if (m_policies.size() > kPolicyEntries) {
            auto oldest = m_policies.end();
            for (auto it = m_policies.begin(); it != m_policies.end(); ++it) {
                if (m_flights.count(it->first)) continue;
                if (oldest == m_policies.end() || it->second.lastSeen < oldest->second.lastSeen) oldest = it;
            }
            if (oldest != m_policies.end() && oldest->first != digest) m_policies.erase(oldest);
        }
        return policy;
    }

    // Starts or joins the one transfer of this image. Returns null when the cached copy needs
    // no network: it was validated within its freshness window and nothing invalidated it,
    // or the last attempt failed and its backoff has not elapsed.
    std::shared_ptr<Flight> EnsureFlight(ImageRequest request, Key key, std::string artKey,
                                         uint64_t accountRevision, std::optional<Snapshot> cached,
                                         bool fromCache, bool persistent) {
        const std::string digest = Store::Digest(key);
        std::shared_ptr<Flight> flight;
        {
            std::lock_guard lock(m_mutex);
            if (m_quit || !request.resolve) return {};
            auto found = m_flights.find(digest);
            if (found != m_flights.end()) {
                found->second->pending = Pending{request, key, artKey, accountRevision, persistent, cached};
                if (InvalidationLocked(artKey) > found->second->scheduledInvalidation)
                    found->second->dirty = true;
                return found->second;
            }

            Policy& policy = PolicyLocked(digest);
            const uint64_t invalidation = InvalidationLocked(artKey);
            const bool invalidated = invalidation > policy.lastScheduledInvalidation;
            if (fromCache && cached && !invalidated) {
                const int64_t validated = cached->Info().validatedAt;
                if (validated > 0 &&
                    Store::Now() - validated < std::max<int64_t>(0, request.limits.freshForSeconds))
                    return {};
            }
            if (policy.nextAttempt > Store::Now() && !invalidated) return {};

            policy.lastScheduledInvalidation = invalidation;
            flight = std::make_shared<Flight>();
            flight->scheduledInvalidation = invalidation;
            flight->pending = Pending{request, key, artKey, accountRevision, persistent, cached};
            m_flights.emplace(digest, flight);
            m_queue.push_front({std::move(request), std::move(key), std::move(artKey), digest,
                                accountRevision, invalidation, persistent, std::move(cached), flight});
        }
        m_wake.notify_one();
        return flight;
    }

    std::optional<ImageResult> Open(ImageRequest request, abort_callback& abort) {
        {
            std::lock_guard lock(m_mutex);
            if (m_quit) return std::nullopt;
        }
        uint64_t accountRevision = AccountRevision(request.identity.provider);
        Key key;
        bool persistent = true;
        try {
            key = MakeImageKey(request.identity);
            const uint64_t after = AccountRevision(request.identity.provider);
            if (after != accountRevision) {
                accountRevision = after;
                persistent = false;
            }
        } catch (...) {
            persistent = false;
        }
        if (!persistent) {
            key.kind = Kind::Image;
            key.provider = request.identity.provider;
            key.scope = "@ephemeral:" + request.identity.provider + ':' + std::to_string(accountRevision);
            key.identity = request.identity.artKind + '/' + request.identity.id;
            key.variant = request.identity.renditionVersion;
        }
        const std::string artKey = ImageArtKey(request.identity);
        std::optional<Snapshot> snapshot;
        if (persistent) {
            try {
                snapshot = GetStore().Lookup(key);
            } catch (...) {
                persistent = false;
                key.scope = "@ephemeral:" + request.identity.provider + ':' + std::to_string(accountRevision);
            }
        }

        // The store publishes complete, flushed generations atomically and checks the payload
        // size on lookup, so the bytes are used as read.
        std::vector<unsigned char> cachedBytes;
        if (snapshot) {
            const size_t limit = std::min(request.limits.maxBodyBytes, static_cast<size_t>(kMaxCachedBytes));
            cachedBytes = ReadPayload(snapshot->Path(), snapshot->Size(), limit);
            if (cachedBytes.empty()) snapshot.reset();
        }
        const uint64_t currentRevision = AccountRevision(request.identity.provider);
        if (currentRevision != accountRevision) {
            accountRevision = currentRevision;
            persistent = false;
            key.scope = "@ephemeral:" + request.identity.provider + ':' + std::to_string(accountRevision);
            snapshot.reset();
            cachedBytes.clear();
        }
        if (cachedBytes.empty() && persistent) {
            for (const auto& alternate : request.alternates) {
                try {
                    const auto stored = GetStore().Lookup(MakeImageKey(alternate));
                    if (!stored) continue;
                    const size_t limit =
                        std::min(request.limits.maxBodyBytes, static_cast<size_t>(kMaxCachedBytes));
                    auto bytes = ReadPayload(stored->Path(), stored->Size(), limit);
                    if (!bytes.empty() && AccountRevision(request.identity.provider) == accountRevision)
                        return ImageResult{std::move(bytes), true, false};
                } catch (...) {
                }
            }
        }
        const bool fromCache = !cachedBytes.empty();
        if (!fromCache && t_cacheOnly) {
            t_cacheMiss = true;
            return std::nullopt;
        }
        auto flight =
            EnsureFlight(std::move(request), key, artKey, accountRevision, snapshot, fromCache, persistent);

        if (fromCache) return ImageResult{std::move(cachedBytes), true, static_cast<bool>(flight)};
        if (!flight) return std::nullopt;

        std::unique_lock lock(flight->mutex);
        while (!flight->done) {
            if (abort.is_aborting()) throw exception_aborted();
            flight->changed.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (flight->cancelled || !flight->success || flight->bytes.empty()) return std::nullopt;
        return ImageResult{flight->bytes, false, false};
    }

    Outcome Run(Job job, IWICImagingFactory* wic) {
        Outcome outcome;
        outcome.artKey = job.artKey;
        outcome.accountRevision = job.accountRevision;
        unsigned long responseStatus = 0;
        const auto fail = [&](RunFailureReason reason, unsigned long status) {
            LogImageRunDiagnostic(outcome.artKey, status, reason);
            return outcome;
        };
        const auto note = [&](RunFailureReason reason, unsigned long status) {
            LogImageRunDiagnostic(outcome.artKey, status, reason);
        };
        try {
            if (AccountRevision(job.key.provider) != job.accountRevision)
                return fail(RunFailureReason::account_revision_changed, responseStatus);
            if (job.persistent) {
                try {
                    if (AccountScope(job.key.provider) != job.key.scope)
                        return fail(RunFailureReason::account_scope_changed, responseStatus);
                } catch (...) {
                    job.persistent = false;
                    note(RunFailureReason::cache_publish_unavailable, responseStatus);
                }
            }

            net::HttpOptions options;
            options.noProxy = false;
            options.timeoutMs = 15000;
            options.maxBodyBytes =
                std::min(job.request.limits.maxBodyBytes, static_cast<size_t>(kMaxCachedBytes));
            options.headers = {{L"Accept", L"image/*"}, {L"Accept-Encoding", L"identity"}};
            const Metadata previous = job.cached ? job.cached->Info() : Metadata();
            if (!previous.etag.empty()) options.headers.emplace_back(L"If-None-Match", Wide(previous.etag));
            if (!previous.lastModified.empty())
                options.headers.emplace_back(L"If-Modified-Since", Wide(previous.lastModified));

            net::HttpResponse response;
            bool haveResponse = false;
            for (unsigned attempt = 0; attempt < kMaxSourceAttempts; ++attempt) {
                std::optional<HttpSource> source;
                try {
                    source = job.request.resolve(m_abort, attempt);
                } catch (const exception_aborted&) {
                    throw;
                } catch (...) {
                    return fail(RunFailureReason::resolver_failed, responseStatus);
                }
                if (!source || source->url.empty())
                    return fail(RunFailureReason::resolver_empty, responseStatus);

                net::HttpOptions current = options;
                current.headers.insert(current.headers.end(), source->headers.begin(), source->headers.end());
                if (attempt > 0) {
                    current.headers.erase(
                        std::remove_if(current.headers.begin(), current.headers.end(),
                                       [](const HttpHeader& header) {
                                           return _wcsicmp(header.first.c_str(), L"If-None-Match") == 0 ||
                                                  _wcsicmp(header.first.c_str(), L"If-Modified-Since") == 0;
                                       }),
                        current.headers.end());
                }
                response = net::SendHttp(source->url, current, m_abort);
                haveResponse = true;
                responseStatus = response.status;
                if (!response.error.empty()) return fail(RunFailureReason::transport_error, responseStatus);
                if (response.status == 304) break;
                if (response.status != 401 && response.status != 403 && response.status != 404) break;
            }
            if (!haveResponse) return fail(RunFailureReason::transport_error, responseStatus);
            if (AccountRevision(job.key.provider) != job.accountRevision)
                return fail(RunFailureReason::account_revision_changed, responseStatus);

            if (response.status == 304 && job.cached) {
                if (NoStore(response)) {
                    if (job.persistent) {
                        try {
                            GetStore().Erase(job.key);
                        } catch (...) {
                        }
                    }
                    outcome.success = true;
                    return outcome;
                }
                if (!job.persistent) {
                    outcome.success = true;
                    return outcome;
                }
                if (Invalidation(job.artKey) != job.invalidation)
                    return fail(RunFailureReason::invalidated, responseStatus);
                Metadata metadata = job.cached->Info();
                const std::string etag = HeaderValue(response, L"etag");
                const std::string lastModified = HeaderValue(response, L"last-modified");
                if (!etag.empty()) metadata.etag = etag;
                if (!lastModified.empty()) metadata.lastModified = lastModified;
                metadata.validatedAt = Store::Now();
                try {
                    // Unchanged: only the validators move. A newer generation published in
                    // the meantime already carries its own.
                    GetStore().Revalidate(job.key, *job.cached, metadata);
                } catch (...) {
                    // Cache I/O is unavailable. The leased old bytes already reached the caller.
                    note(RunFailureReason::cache_publish_unavailable, responseStatus);
                }
                outcome.success = true;
                return outcome;
            }

            if (response.status != 200) return fail(RunFailureReason::unexpected_status, responseStatus);
            if (response.body.empty()) return fail(RunFailureReason::empty_body, responseStatus);
            std::vector<unsigned char> bytes(response.body.size());
            std::memcpy(bytes.data(), response.body.data(), response.body.size());
            const std::string contentType = Utf8(response.contentType);
            if (!IsCompleteImage(bytes, contentType))
                return fail(RunFailureReason::image_format, responseStatus);
            if (!VerifyImageHeader(wic, bytes)) return fail(RunFailureReason::image_decode, responseStatus);
            // Listeners reload only when the picture really differs from the one they show.
            const bool differs =
                job.cached && ReadPayload(job.cached->Path(), job.cached->Size(), kMaxCachedBytes) != bytes;

            if (NoStore(response)) {
                if (job.persistent && job.cached) {
                    try {
                        GetStore().Erase(job.key);
                    } catch (...) {
                    }
                }
                outcome.success = true;
                outcome.bytes = std::move(bytes);
                return outcome;
            }

            if (AccountRevision(job.key.provider) != job.accountRevision ||
                (job.persistent && Invalidation(job.artKey) != job.invalidation)) {
                if (AccountRevision(job.key.provider) != job.accountRevision)
                    return fail(RunFailureReason::account_revision_changed, responseStatus);
                return fail(RunFailureReason::invalidated, responseStatus);
            }
            if (!job.persistent) {
                outcome.success = true;
                outcome.bytes = std::move(bytes);
                return outcome;
            }

            Metadata metadata;
            metadata.contentType = contentType;
            metadata.etag = HeaderValue(response, L"etag");
            metadata.lastModified = HeaderValue(response, L"last-modified");
            metadata.validatedAt = Store::Now();

            std::unique_ptr<Writer> writer;
            try {
                writer = GetStore().Begin(job.key);
            } catch (...) {
                note(RunFailureReason::cache_publish_unavailable, responseStatus);
                outcome.success = true;
                outcome.bytes = std::move(bytes);
                return outcome;
            }
            if (!writer || !WritePayload(writer->Path(), bytes)) {
                note(RunFailureReason::cache_publish_unavailable, responseStatus);
                outcome.success = true;
                outcome.bytes = std::move(bytes);
                return outcome;
            }
            if (AccountRevision(job.key.provider) != job.accountRevision)
                return fail(RunFailureReason::account_revision_changed, responseStatus);
            if (Invalidation(job.artKey) != job.invalidation)
                return fail(RunFailureReason::invalidated, responseStatus);
            bool canPublish = true;
            bool scopeMismatch = false;
            try {
                scopeMismatch = AccountScope(job.key.provider) != job.key.scope;
                canPublish = !scopeMismatch;
            } catch (...) {
                canPublish = false;
                note(RunFailureReason::cache_publish_unavailable, responseStatus);
            }
            if (!canPublish) {
                if (scopeMismatch) note(RunFailureReason::account_scope_changed, responseStatus);
                outcome.success = true;
                outcome.bytes = std::move(bytes);
                return outcome;
            }
            bool committed = false;
            try {
                committed = writer->Commit(metadata).has_value();
            } catch (...) {
                note(RunFailureReason::cache_publish_unavailable, responseStatus);
                outcome.success = true;
                outcome.bytes = std::move(bytes);
                return outcome;
            }
            if (!committed) {
                // A newer ticket may already hold the current generation. Otherwise return
                // these validated network bytes without changing persistent state.
                note(RunFailureReason::cache_publish_unavailable, responseStatus);
                if (AccountRevision(job.key.provider) != job.accountRevision)
                    return fail(RunFailureReason::account_revision_changed, responseStatus);
                try {
                    if (auto latest = GetStore().Lookup(job.key))
                        outcome.bytes = ReadPayload(latest->Path(), latest->Size(), options.maxBodyBytes);
                } catch (...) {
                }
                if (outcome.bytes.empty()) outcome.bytes = std::move(bytes);
                outcome.success = true;
                return outcome;
            }

            outcome.success = true;
            outcome.changed = differs;
            outcome.bytes = std::move(bytes);
            return outcome;
        } catch (const exception_aborted&) {
            return fail(RunFailureReason::aborted, responseStatus);
        } catch (...) {
            return fail(RunFailureReason::internal_exception, responseStatus);
        }
    }

    void Finish(const std::string& digest, const std::shared_ptr<Flight>& flight, Outcome outcome,
                const ImageLimits& limits) {
        Pending checked;
        bool hasRetry = false;
        {
            std::lock_guard lock(m_mutex);
            if (flight->dirty && flight->pending &&
                InvalidationLocked(flight->pending->artKey) > flight->scheduledInvalidation) {
                checked = *flight->pending;
                hasRetry = true;
            }
        }

        bool scopeValid = hasRetry;
        if (hasRetry) {
            scopeValid = AccountRevision(checked.request.identity.provider) == checked.accountRevision;
            if (scopeValid && checked.persistent) {
                try {
                    scopeValid = AccountScope(checked.key.provider) == checked.key.scope;
                } catch (...) {
                    scopeValid = false;
                }
            }
        }

        bool queuedRetry = false;
        std::vector<unsigned char> completedBytes = std::move(outcome.bytes);
        {
            std::lock_guard lock(m_mutex);
            m_flights.erase(digest);
            Policy& policy = PolicyLocked(digest);
            if (outcome.success) {
                policy.failures = 0;
                policy.nextAttempt = 0;
            } else if (InvalidationLocked(outcome.artKey) > policy.lastScheduledInvalidation) {
                policy.failures = 0;
                policy.nextAttempt = Store::Now();
            } else {
                ++policy.failures;
                policy.nextAttempt = Store::Now() + BackoffSeconds(limits, policy.failures);
            }
            if (!m_quit && scopeValid && flight->dirty && flight->pending) {
                Pending& latest = *flight->pending;
                const uint64_t invalidation = InvalidationLocked(latest.artKey);
                if (invalidation > flight->scheduledInvalidation &&
                    latest.accountRevision == checked.accountRevision &&
                    latest.key.scope == checked.key.scope &&
                    AccountRevision(latest.request.identity.provider) == latest.accountRevision) {
                    auto retry = std::make_shared<Flight>();
                    retry->scheduledInvalidation = invalidation;
                    retry->pending = latest;
                    m_flights.emplace(digest, retry);
                    m_queue.push_front({std::move(latest.request), std::move(latest.key),
                                        std::move(latest.artKey), digest, latest.accountRevision,
                                        invalidation, latest.persistent, std::move(latest.cached), retry});
                    policy.lastScheduledInvalidation = invalidation;
                    policy.failures = 0;
                    policy.nextAttempt = 0;
                    queuedRetry = true;
                }
            }
        }
        {
            std::lock_guard lock(flight->mutex);
            flight->success = outcome.success;
            flight->bytes = std::move(completedBytes);
            flight->done = true;
        }
        flight->changed.notify_all();
        if (outcome.changed) Notify({std::move(outcome.artKey), outcome.accountRevision});
        if (queuedRetry) m_wake.notify_one();
    }

    void Worker() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        winrt::com_ptr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return m_quit || !m_queue.empty(); });
                if (m_quit) break;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            const ImageLimits limits = job.request.limits;
            const std::string digest = job.digest;
            const auto flight = job.flight;
            Outcome outcome = Run(std::move(job), wic.get());
            Finish(digest, flight, std::move(outcome), limits);
        }
        winrt::uninit_apartment();
    }

    void Notify(ImageEvent event) {
        std::vector<std::pair<int, std::function<void(ImageEvent)>>> callbacks;
        {
            std::lock_guard lock(m_mutex);
            for (const auto& item : m_listeners)
                callbacks.push_back(item);
        }
        for (auto& [token, callback] : callbacks) {
            {
                std::lock_guard lock(m_mutex);
                if (!m_listeners.count(token)) continue;
            }
            callback(event);
        }
    }

    void Invalidate(std::string_view prefix) {
        std::lock_guard lock(m_mutex);
        if (m_quit) return;
        m_invalidations.push_back({std::string(prefix), ++m_invalidation});
        for (auto& [digest, flight] : m_flights) {
            (void)digest;
            if (flight->pending && flight->pending->artKey.compare(0, prefix.size(), prefix) == 0)
                flight->dirty = true;
        }
        if (m_invalidations.size() > kInvalidationEntries)
            m_invalidations.erase(
                m_invalidations.begin(),
                m_invalidations.begin() +
                    static_cast<std::ptrdiff_t>(m_invalidations.size() - kInvalidationEntries));
    }

    int Subscribe(std::function<void(ImageEvent)> callback) {
        std::lock_guard lock(m_mutex);
        if (m_quit) return 0;
        const int token = m_nextListener++;
        m_listeners.emplace(token, std::move(callback));
        return token;
    }

    void Unsubscribe(int token) {
        std::lock_guard lock(m_mutex);
        m_listeners.erase(token);
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    std::unordered_map<std::string, std::shared_ptr<Flight>> m_flights;
    std::unordered_map<std::string, Policy> m_policies;
    std::vector<InvalidationRecord> m_invalidations;
    std::map<int, std::function<void(ImageEvent)>> m_listeners;
    uint64_t m_invalidation = 0;
    uint64_t m_clock = 0;
    int m_nextListener = 1;
    bool m_quit = false;
    abort_callback_impl m_abort;
    std::vector<std::thread> m_threads;
};

Images& Images::Get() {
    std::lock_guard lock(g_instanceMutex);
    if (!g_images) {
        if (g_stopped) throw std::runtime_error("The image cache service is stopped.");
        g_images.reset(new Images());
    }
    return *g_images;
}

Images* Images::Existing() {
    std::lock_guard lock(g_instanceMutex);
    return g_images.get();
}

void Images::Shutdown() {
    Images* instance = nullptr;
    {
        std::lock_guard lock(g_instanceMutex);
        if (g_stopped) return;
        g_stopped = true;
        instance = g_images.get();
    }
    // Keep references valid through on_quit; the lazy owner frees the stopped object at
    // component unload. Get never creates workers again after the shutdown barrier.
    if (instance) instance->m_impl->Stop();
}

Images::Images() : m_impl(std::make_unique<Impl>()) {}

Images::~Images() = default;

CacheOnlyScope::CacheOnlyScope() : m_previous(t_cacheOnly) {
    t_cacheOnly = true;
    t_cacheMiss = false;
}

CacheOnlyScope::~CacheOnlyScope() {
    t_cacheOnly = m_previous;
}

bool TakeCacheMiss() {
    return std::exchange(t_cacheMiss, false);
}

SizeHintScope::SizeHintScope(int px) : m_previous(t_sizeHint) {
    t_sizeHint = px;
}

SizeHintScope::~SizeHintScope() {
    t_sizeHint = m_previous;
}

int SizeHint() {
    return t_sizeHint;
}

std::optional<ImageResult> Images::Open(ImageRequest request, abort_callback& abort) {
    return m_impl->Open(std::move(request), abort);
}

int Images::Subscribe(std::function<void(ImageEvent)> callback) {
    return m_impl->Subscribe(std::move(callback));
}

void Images::Unsubscribe(int token) {
    m_impl->Unsubscribe(token);
}

void Images::Invalidate(std::string_view prefix) {
    art_absence::Invalidate(prefix);
    m_impl->Invalidate(prefix);
}

} // namespace glasswave::cache

namespace {

class ImageCacheInit : public initquit {
  public:
    void on_init() override {}
    void on_quit() override { glasswave::cache::Images::Shutdown(); }
};

FB2K_SERVICE_FACTORY(ImageCacheInit);

} // namespace
