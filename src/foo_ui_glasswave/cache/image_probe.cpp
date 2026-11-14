#include "pch.h"
#include "cache/images.h"
#include "cache/art_absence.h"
#include "net/http.h"

#include <SDK/initquit.h>
#include <bcrypt.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <latch>
#include <stdexcept>

namespace glasswave::cache::image_probe {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr std::string_view kProvider = "cache-image-test";
constexpr size_t kMaxMarkerBytes = 4096;
constexpr std::string_view kPngABase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGPgEpH7DwABpAE8k4sOtwAAAABJRU5ErkJggg==";
constexpr const char* kPngASha = "c764e18e52e0452bff7f084e04326af8f57205a3c1d3975af5541866029c6c94";
constexpr const char* kPngBSha = "a3803073e2cbd7f1fa00674ca4480d102c16d3ed869937510b53d3defc1b9fcd";
constexpr const char* kPngCSha = "6ab8f39df6eb4c80502d8289acfe6392f4f8ad0f4e882e14a6afca3ebbb55b26";

struct Paths {
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct Marker {
    bool valid = false;
    std::string runId;
    std::wstring baseUrl;
};

struct CaseResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct State {
    explicit State(fs::path resultPath) : result(std::move(resultPath)) {}

    fs::path result;
    mutable std::mutex eventsMutex;
    std::condition_variable eventsChanged;
    std::vector<ImageEvent> events;
    std::vector<Key> cleanupKeys;
    std::vector<CaseResult> cases;
    std::vector<std::string> failures;
    std::atomic_bool stopping = false;
    Clock::time_point started = Clock::now();
};

struct OpenAttempt {
    bool invoked = false;
    bool aborted = false;
    std::optional<ImageResult> result;
};

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_wide_from_utf8(text.data(), text.size()).get_ptr();
}

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

JsonValue Number(size_t value) {
    return JsonValue::CreateNumberValue(static_cast<double>(value));
}

JsonValue Number(double value) {
    return JsonValue::CreateNumberValue(value);
}

std::string Sha256(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<unsigned char, 32> digest{};
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
                   digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const unsigned char byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

std::vector<unsigned char> DecodeBase64(std::string_view text) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<signed char, 256> decode{};
    decode.fill(-1);
    for (size_t i = 0; i < 64; ++i)
        decode[static_cast<unsigned char>(alphabet[i])] = static_cast<signed char>(i);
    std::vector<unsigned char> bytes;
    unsigned value = 0;
    int bits = -8;
    for (const unsigned char c : text) {
        if (c == '=') break;
        const signed char digit = decode[c];
        if (digit < 0) return {};
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 0) {
            bytes.push_back(static_cast<unsigned char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return bytes;
}

bool DigestIs(const std::optional<ImageResult>& result, std::string_view expected) {
    return result && result->bytes.size() && Sha256(result->bytes) == expected;
}

std::optional<fs::path> NativeProfileDirectory() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    fs::path profile(pfc::stringcvt::string_wide_from_utf8(native.get_ptr()).get_ptr());
    profile = profile.lexically_normal();
    while (!profile.empty() && profile.filename().empty())
        profile = profile.parent_path();
    return profile;
}

std::optional<Paths> ProbePaths() {
    const auto profile = NativeProfileDirectory();
    if (!profile || profile->filename() != L"profile" || profile->parent_path().filename() != L"fb2k-dev")
        return std::nullopt;
    const fs::path directory = *profile / L"glasswave" / L"cache-image-test";
    return Paths{directory / L"request.json", directory / L"request.claimed.json",
                 directory / L"result.json"};
}

bool ValidRunId(std::string_view value) {
    if (value.size() < 8 || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}

bool ValidLoopbackUrl(std::string_view value) {
    constexpr std::string_view prefix = "http://127.0.0.1:";
    if (value.size() <= prefix.size() || value.substr(0, prefix.size()) != prefix) return false;
    const std::string_view port = value.substr(prefix.size());
    if (port.empty() || port.size() > 5 ||
        !std::all_of(port.begin(), port.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return false;
    const int parsed = std::stoi(std::string(port));
    return parsed > 0 && parsed <= 65535;
}

Marker ReadMarker(const fs::path& path) {
    Marker marker;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxMarkerBytes) return marker;
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<size_t>(input.gcount()) != bytes.size()) return marker;

    try {
        const JsonObject request = JsonObject::Parse(winrt::to_hstring(bytes));
        const auto probe = request.GetNamedString(L"probe");
        marker.runId = winrt::to_string(request.GetNamedString(L"runId"));
        marker.baseUrl = request.GetNamedString(L"baseUrl").c_str();
        marker.valid = wcscmp(probe.c_str(), L"image-cache") == 0 && ValidRunId(marker.runId) &&
                       ValidLoopbackUrl(Utf8(marker.baseUrl));
    } catch (...) {
    }
    return marker;
}

void WriteResult(const State& state, std::string fatal = {}) {
    JsonObject result;
    result.Insert(L"probe", JsonValue::CreateStringValue(L"image-cache"));
    result.Insert(L"provider", JsonValue::CreateStringValue(Wide(kProvider)));
    result.Insert(L"elapsedMs", Number(ElapsedMs(state.started)));

    JsonArray cases;
    for (const auto& item : state.cases) {
        JsonObject row;
        row.Insert(L"name", JsonValue::CreateStringValue(Wide(item.name)));
        row.Insert(L"ok", JsonValue::CreateBooleanValue(item.ok));
        row.Insert(L"detail", item.detail.empty() ? JsonValue::CreateNullValue()
                                                  : JsonValue::CreateStringValue(Wide(item.detail)));
        cases.Append(row);
    }
    result.Insert(L"cases", cases);
    result.Insert(L"caseCount", Number(state.cases.size()));

    JsonArray failures;
    for (const auto& failure : state.failures)
        failures.Append(JsonValue::CreateStringValue(Wide(failure)));
    result.Insert(L"failureCodes", failures);

    size_t eventCount = 0;
    {
        std::lock_guard lock(state.eventsMutex);
        eventCount = state.events.size();
    }
    result.Insert(L"eventCount", Number(eventCount));
    const bool ok = fatal.empty() && state.failures.empty();
    const std::string error = ok ? std::string() : (fatal.empty() ? "probe_failed" : fatal);
    result.Insert(L"ok", JsonValue::CreateBooleanValue(ok));
    result.Insert(L"error", JsonValue::CreateStringValue(Wide(error)));

    try {
        const fs::path temporary = state.result.wstring() + L".tmp";
        const std::string bytes = Utf8(result.Stringify().c_str());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) return;
        MoveFileExW(temporary.c_str(), state.result.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } catch (...) {
    }
}

void AddCase(const std::shared_ptr<State>& state, std::string name, bool ok, std::string detail = {}) {
    if (!ok && !detail.empty()) state->failures.push_back(name + ": " + detail);
    state->cases.push_back({std::move(name), ok, std::move(detail)});
}

bool Sleep(const std::shared_ptr<State>& state, abort_callback& abort, std::chrono::milliseconds total) {
    const auto deadline = Clock::now() + total;
    while (Clock::now() < deadline) {
        if (abort.is_aborting() || state->stopping) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= std::chrono::milliseconds(0)) break;
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(50)));
    }
    return !abort.is_aborting() && !state->stopping;
}

bool WaitMockCaseRequested(const std::shared_ptr<State>& state, const std::wstring& baseUrl,
                           std::string_view route, abort_callback& abort) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline && !abort.is_aborting() && !state->stopping) {
        try {
            net::HttpOptions options;
            options.noProxy = true;
            options.timeoutMs = 500;
            options.maxBodyBytes = 64u * 1024u;
            const auto response = net::SendHttp(baseUrl + L"/__counters", options, abort);
            if (response.ok() && !response.body.empty()) {
                const JsonObject counters = JsonObject::Parse(winrt::to_hstring(response.body));
                const JsonObject byCase = counters.GetNamedObject(L"byCase");
                const JsonObject selected = byCase.GetNamedObject(Wide(route).c_str());
                if (selected.GetNamedNumber(L"requests") >= 1) return true;
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
        }
        if (!Sleep(state, abort, std::chrono::milliseconds(50))) return false;
    }
    return false;
}

ImageIdentity Identity(std::string_view runId, std::string_view name) {
    return {std::string(kProvider), "regression", std::string(runId) + '-' + std::string(name), "probe-v1"};
}

std::string MockRoute(std::string_view name) {
    if (name == "cached-first-304") return "cached-first";
    if (name == "changed-200") return "changed";
    if (name == "invalidate-inflight") return "invalidate";
    if (name == "rotation-late") return "rotation";
    if (name == "shutdown-final") return "shutdown";
    return std::string(name);
}

ImageRequest RequestFor(std::string_view runId, std::wstring baseUrl, std::string_view name) {
    ImageRequest request;
    request.identity = Identity(runId, name);
    request.limits.maxBodyBytes = 1024u * 1024u;
    request.limits.freshForSeconds = 300;
    request.limits.retryBaseSeconds = 1;
    request.limits.retryMaxSeconds = 4;
    request.resolve = [baseUrl = std::move(baseUrl),
                       path = MockRoute(name)](foobar2000_io::abort_callback& abort, unsigned) {
        abort.check();
        HttpSource source;
        source.url = baseUrl + L"/image/" + Wide(path);
        source.headers.emplace_back(L"User-Agent", L"Glasswave image-cache probe/1");
        return std::optional<HttpSource>(std::move(source));
    };
    return request;
}

OpenAttempt OpenOnce(Images& images, ImageRequest request, abort_callback& abort,
                     const std::shared_ptr<State>& state) {
    OpenAttempt attempt;
    if (state->stopping || abort.is_aborting()) {
        attempt.aborted = true;
        return attempt;
    }
    attempt.invoked = true;
    try {
        attempt.result = images.Open(std::move(request), abort);
    } catch (const exception_aborted&) {
        attempt.aborted = true;
    } catch (...) {
    }
    return attempt;
}

size_t EventCount(const std::shared_ptr<State>& state, std::string_view artKey) {
    std::lock_guard lock(state->eventsMutex);
    return static_cast<size_t>(
        std::count_if(state->events.begin(), state->events.end(),
                      [&](const ImageEvent& event) { return event.artKey == artKey; }));
}

bool WaitForEvent(const std::shared_ptr<State>& state, abort_callback& abort, std::string_view artKey,
                  size_t baseline, std::chrono::milliseconds timeout) {
    std::unique_lock lock(state->eventsMutex);
    const bool signaled = state->eventsChanged.wait_for(lock, timeout, [&] {
        if (abort.is_aborting() || state->stopping) return true;
        const auto count = static_cast<size_t>(
            std::count_if(state->events.begin(), state->events.end(),
                          [&](const ImageEvent& event) { return event.artKey == artKey; }));
        return count > baseline;
    });
    return signaled && !abort.is_aborting() && !state->stopping;
}

bool RemoveTestKey(const Key& key) noexcept {
    try {
        GetStore().Erase(key);
        return true;
    } catch (...) {
        return false;
    }
}

bool RecordKey(const std::shared_ptr<State>& state, const ImageIdentity& identity, Key& key) {
    try {
        key = MakeImageKey(identity);
        state->cleanupKeys.push_back(key);
        return RemoveTestKey(key);
    } catch (...) {
        return false;
    }
}

bool Seed(const std::shared_ptr<State>& state, const ImageIdentity& identity,
          const std::vector<unsigned char>& bytes, std::string etag) {
    Key key;
    if (!RecordKey(state, identity, key)) return false;
    try {
        auto writer = GetStore().Begin(key);
        if (!writer) return false;
        {
            std::ofstream output(writer->Path(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) return false;
        }
        Metadata metadata;
        metadata.contentType = "image/png";
        metadata.etag = std::move(etag);
        metadata.lastModified = "Tue, 10 Nov 2026 00:00:00 GMT";
        metadata.validatedAt = Store::Now();
        return writer->Commit(std::move(metadata)).has_value();
    } catch (...) {
        return false;
    }
}

bool WaitStoreMiss(const std::shared_ptr<State>& state, abort_callback& abort, const Key& key,
                   std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        try {
            if (!GetStore().Lookup(key)) return true;
        } catch (...) {
            return true;
        }
        if (!Sleep(state, abort, std::chrono::milliseconds(100))) return false;
    }
    return false;
}

bool StoreContains(const Key& key) {
    try {
        return GetStore().Lookup(key).has_value();
    } catch (...) {
        return false;
    }
}

std::string RunCases(const std::shared_ptr<State>& state, const Marker& marker, abort_callback& abort) {
    Images& images = Images::Get();
    const int listener = images.Subscribe([state](ImageEvent event) {
        {
            std::lock_guard lock(state->eventsMutex);
            state->events.push_back(std::move(event));
        }
        state->eventsChanged.notify_all();
    });

    std::string fatal;
    try {
        const std::vector<unsigned char> pngA = DecodeBase64(kPngABase64);
        if (pngA.empty() || Sha256(pngA) != kPngASha) throw std::runtime_error("fixture");

        {
            const auto identity = Identity(marker.runId, "confirmed-absence");
            const auto artKey = ImageArtKey(identity);
            const auto account = AccountRevision(identity.provider);
            state->cleanupKeys.push_back(art_absence::StorageKey(identity));
            const bool initiallyAbsent = !art_absence::KnownMissing(identity);
            art_absence::StoreMissing(identity, account);
            bool ok = initiallyAbsent && art_absence::KnownMissing(identity) && art_absence::Fresh(artKey);
            auto otherSize = identity;
            otherSize.renditionVersion = "different-thumbnail-size";
            ok = ok && art_absence::KnownMissing(otherSize);
            {
                std::lock_guard lock(art_absence::mutex);
                art_absence::entries.erase(artKey);
            }
            // After a restart (memory dropped) the persisted record is trusted at once.
            ok = ok && art_absence::KnownMissing(identity) && art_absence::Fresh(artKey);
            images.Invalidate(artKey);
            ok = ok && !art_absence::Fresh(artKey) && !art_absence::KnownMissing(identity);
            const auto staleEpoch = art_absence::Epoch();
            images.Invalidate(artKey);
            art_absence::StoreMissing(identity, account, staleEpoch);
            ok = ok && !art_absence::Fresh(artKey);
            art_absence::StoreMissing(identity, account);
            {
                std::lock_guard lock(art_absence::mutex);
                art_absence::entries.at(artKey).checked = Store::Now() - art_absence::kFreshSeconds - 1;
            }
            ok = ok && !art_absence::KnownMissing(identity);
            art_absence::Clear(identity, account);
            ok = ok && !art_absence::Fresh(artKey);
            AddCase(state, "confirmed-absence", ok,
                    ok ? std::string() : "absence_persistence_expiry_or_invalidation_failed");
        }

        {
            const std::string name = "cached-first-304";
            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            bool ok = Seed(state, identity, pngA, R"("cached-first-a")");
            if (ok) {
                // A fresh entry is served without a request; ask for the revalidation.
                images.Invalidate(artKey);
                const auto start = Clock::now();
                const OpenAttempt attempt =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                const double elapsed = ElapsedMs(start);
                ok = !attempt.aborted && attempt.result && attempt.result->fromCache &&
                     attempt.result->revalidating && DigestIs(attempt.result, kPngASha) && elapsed < 600;
                if (!ok) {
                    AddCase(state, name, false, "cached_render_not_immediate");
                } else {
                    ok = Sleep(state, abort, std::chrono::milliseconds(1100)) &&
                         EventCount(state, artKey) == 0;
                    try {
                        const auto snapshot = GetStore().Lookup(MakeImageKey(identity));
                        ok = ok && snapshot && snapshot->Info().etag == R"("cached-first-b")" &&
                             snapshot->Info().lastModified == "Wed, 11 Nov 2026 00:00:00 GMT";
                    } catch (...) {
                        ok = false;
                    }
                    AddCase(state, name, ok, ok ? std::string() : "validated_304_mismatch");
                }
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }

        {
            const std::string name = "changed-200";
            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            bool ok = Seed(state, identity, pngA, R"("changed-a")");
            if (ok) {
                images.Invalidate(artKey);
                const OpenAttempt cached =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = !cached.aborted && cached.result && cached.result->fromCache &&
                     cached.result->revalidating && DigestIs(cached.result, kPngASha);
                if (!ok) {
                    AddCase(state, name, false, "cached_generation_not_returned");
                } else {
                    const size_t baseline = EventCount(state, artKey);
                    ok = WaitForEvent(state, abort, artKey, baseline, std::chrono::milliseconds(3000)) &&
                         EventCount(state, artKey) == baseline + 1;
                    const OpenAttempt changed =
                        OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                    ok = ok && !changed.aborted && changed.result && changed.result->fromCache &&
                         DigestIs(changed.result, kPngBSha);
                    AddCase(state, name, ok, ok ? std::string() : "changed_bytes_not_delivered");
                }
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }

        {
            const std::string name = "mislabelled-mime";
            const ImageIdentity identity = Identity(marker.runId, name);
            Key key;
            bool ok = RecordKey(state, identity, key);
            if (ok) {
                const OpenAttempt cold =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = !cold.aborted && cold.result && !cold.result->fromCache &&
                     DigestIs(cold.result, kPngASha);
                const OpenAttempt cached =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = ok && !cached.aborted && cached.result && cached.result->fromCache &&
                     !cached.result->revalidating && DigestIs(cached.result, kPngASha) && StoreContains(key);
                AddCase(state, name, ok, ok ? std::string() : "mislabelled_png_not_accepted_by_magic");
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }

        const auto badResponse = [&](std::string name) {
            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            const std::string etag = "\"" + name + "-a\"";
            bool ok = Seed(state, identity, pngA, etag);
            if (ok) {
                images.Invalidate(artKey);
                const OpenAttempt cached =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = !cached.aborted && cached.result && cached.result->fromCache &&
                     DigestIs(cached.result, kPngASha);
                ok = ok && Sleep(state, abort, std::chrono::milliseconds(700)) &&
                     EventCount(state, artKey) == 0;
                const OpenAttempt kept =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = ok && !kept.aborted && kept.result && kept.result->fromCache &&
                     DigestIs(kept.result, kPngASha);
                AddCase(state, name, ok, ok ? std::string() : "invalid_body_replaced_cached_image");
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        };
        badResponse("bad-html");
        badResponse("truncated");

        {
            const std::string name = "no-store";
            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            bool ok = Seed(state, identity, pngA, R"("no-store-a")");
            Key key;
            try {
                key = MakeImageKey(identity);
            } catch (...) {
                ok = false;
            }
            if (ok) {
                images.Invalidate(artKey);
                const OpenAttempt cached =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = !cached.aborted && cached.result && cached.result->fromCache &&
                     DigestIs(cached.result, kPngASha);
                ok = ok && WaitStoreMiss(state, abort, key, std::chrono::milliseconds(2500)) &&
                     EventCount(state, artKey) == 0;
                const OpenAttempt network =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = ok && !network.aborted && network.result && !network.result->fromCache &&
                     DigestIs(network.result, kPngBSha) && !StoreContains(key) &&
                     EventCount(state, artKey) == 0;
                AddCase(state, name, ok, ok ? std::string() : "no_store_was_persisted");
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }

        {
            const std::string name = "coalesce";
            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            Key key;
            bool ok = RecordKey(state, identity, key);
            if (ok) {
                std::latch gate(3);
                std::array<OpenAttempt, 2> attempts;
                std::vector<std::thread> threads;
                for (size_t i = 0; i < attempts.size(); ++i) {
                    threads.emplace_back([&, i] {
                        gate.arrive_and_wait();
                        attempts[i] =
                            OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                    });
                }
                gate.arrive_and_wait();
                for (auto& thread : threads)
                    thread.join();
                ok = std::all_of(attempts.begin(), attempts.end(), [](const OpenAttempt& attempt) {
                    return !attempt.aborted && attempt.result && DigestIs(attempt.result, kPngASha);
                });
                ok = ok && EventCount(state, artKey) == 0;
                AddCase(state, name, ok, ok ? std::string() : "coalesced_open_failed");
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }

        {
            const std::string name = "invalidate-inflight";
            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            bool ok = Seed(state, identity, pngA, R"("invalidate-a")");
            if (ok) {
                images.Invalidate(artKey);
                const OpenAttempt cached =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = !cached.aborted && cached.result && cached.result->fromCache &&
                     DigestIs(cached.result, kPngASha);
                ok = ok && Sleep(state, abort, std::chrono::milliseconds(180));
                const size_t baseline = EventCount(state, artKey);
                images.Invalidate(artKey);
                const bool refreshed =
                    WaitForEvent(state, abort, artKey, baseline, std::chrono::milliseconds(3000));
                const OpenAttempt replacement =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = ok && refreshed && !replacement.aborted && replacement.result &&
                     DigestIs(replacement.result, kPngCSha);
                AddCase(state, name, ok, ok ? std::string() : "invalidated_inflight_refresh_missing");
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }

        {
            const std::string name = "rotation-late";
            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            bool ok = Seed(state, identity, pngA, R"("rotation-a")");
            if (ok) {
                images.Invalidate(artKey);
                const OpenAttempt oldCached =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = !oldCached.aborted && oldCached.result && oldCached.result->fromCache &&
                     DigestIs(oldCached.result, kPngASha);
                ok = ok && Sleep(state, abort, std::chrono::milliseconds(180));
                RotateAccountScope(std::string(kProvider));
                Key newKey;
                ok = ok && RecordKey(state, identity, newKey);
                ok = ok && Sleep(state, abort, std::chrono::milliseconds(1100)) &&
                     EventCount(state, artKey) == 0 && !StoreContains(newKey);
                const OpenAttempt replacement =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                ok = ok && !replacement.aborted && replacement.result && !replacement.result->fromCache &&
                     DigestIs(replacement.result, kPngCSha) && StoreContains(newKey) &&
                     EventCount(state, artKey) == 0;
                AddCase(state, name, ok, ok ? std::string() : "old_scope_reply_was_observable");
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }

        {
            const std::string name = "shutdown-final";
            bool ok = true;
            std::string detail;
            const auto require = [&](bool condition, std::string_view failure) {
                if (!condition && detail.empty()) detail.assign(failure.data(), failure.size());
                ok = ok && condition;
            };

            for (size_t round = 0; round < 3; ++round) {
                const std::string finishName = "shutdown-finish-" + std::to_string(round);
                const ImageIdentity finishIdentity = Identity(marker.runId, finishName);
                const std::string finishArtKey = ImageArtKey(finishIdentity);
                Key finishKey;
                const bool recorded = RecordKey(state, finishIdentity, finishKey);
                require(recorded, "finish_wait_seed_failed");
                if (!recorded) continue;

                std::latch gate(3);
                std::atomic_size_t entered = 0;
                std::array<OpenAttempt, 2> attempts;
                std::vector<std::thread> threads;
                for (size_t i = 0; i < attempts.size(); ++i) {
                    threads.emplace_back([&, i] {
                        ++entered;
                        gate.arrive_and_wait();
                        attempts[i] = OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, finishName),
                                               abort, state);
                    });
                }
                gate.arrive_and_wait();
                const auto entryDeadline = Clock::now() + std::chrono::seconds(1);
                while (entered.load() < attempts.size() && Clock::now() < entryDeadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                require(entered.load() == attempts.size(), "finish_waiters_not_entered");
                require(WaitMockCaseRequested(state, marker.baseUrl, finishName, abort),
                        "finish_wait_request_not_observed");
                for (auto& thread : threads)
                    thread.join();
                require(std::all_of(attempts.begin(), attempts.end(),
                                    [](const OpenAttempt& attempt) {
                                        return attempt.invoked && !attempt.aborted && attempt.result &&
                                               !attempt.result->fromCache &&
                                               DigestIs(attempt.result, kPngASha);
                                    }),
                        "finish_wait_result_failed");
                require(StoreContains(finishKey), "finish_wait_not_published_before_return");
                require(EventCount(state, finishArtKey) == 0, "cold_finish_emitted_change_event");
            }

            const ImageIdentity identity = Identity(marker.runId, name);
            const std::string artKey = ImageArtKey(identity);
            Key key;
            const bool recorded = RecordKey(state, identity, key);
            require(recorded, "shutdown_seed_failed");
            if (recorded) {
                std::latch gate(3);
                std::atomic_size_t entered = 0;
                std::array<OpenAttempt, 2> attempts;
                std::vector<std::thread> threads;
                for (size_t i = 0; i < attempts.size(); ++i) {
                    threads.emplace_back([&, i] {
                        ++entered;
                        gate.arrive_and_wait();
                        attempts[i] =
                            OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                    });
                }
                gate.arrive_and_wait();
                const auto entryDeadline = Clock::now() + std::chrono::seconds(1);
                while (entered.load() < attempts.size() && Clock::now() < entryDeadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                require(entered.load() == attempts.size(), "shutdown_waiters_not_entered");
                require(WaitMockCaseRequested(state, marker.baseUrl, "shutdown", abort),
                        "in_flight_request_not_observed");

                Images* const before = Images::Existing();
                const auto shutdownStart = Clock::now();
                Images::Shutdown();
                for (auto& thread : threads)
                    thread.join();
                require(ElapsedMs(shutdownStart) < 2500, "shutdown_wait_failed");
                require(std::all_of(attempts.begin(), attempts.end(),
                                    [](const OpenAttempt& attempt) {
                                        return attempt.invoked && !attempt.aborted && !attempt.result;
                                    }),
                        "in_flight_waiter_not_cancelled");
                require(!StoreContains(key), "aborted_request_was_published");
                require(EventCount(state, artKey) == 0, "aborted_request_notified");

                const auto secondShutdownStart = Clock::now();
                Images::Shutdown();
                require(ElapsedMs(secondShutdownStart) < 500, "second_shutdown_not_idempotent");
                bool stableGet = false;
                try {
                    stableGet = &Images::Get() == before;
                } catch (...) {
                }
                require(stableGet && before && Images::Existing() == before,
                        "singleton_changed_after_shutdown");
                images.Invalidate(artKey);
                const int postShutdownListener = images.Subscribe([](ImageEvent) {});
                require(postShutdownListener == 0, "post_shutdown_subscribe_succeeded");
                const OpenAttempt late =
                    OpenOnce(images, RequestFor(marker.runId, marker.baseUrl, name), abort, state);
                require(late.invoked && !late.result, "post_shutdown_open_succeeded");
                AddCase(state, name, ok, detail);
            } else {
                AddCase(state, name, false, "seed_failed");
            }
        }
    } catch (const exception_aborted&) {
        fatal = "aborted";
    } catch (...) {
        fatal = "probe_exception";
    }

    images.Unsubscribe(listener);
    return fatal;
}

class ProbeLifecycle : public initquit {
  public:
    void on_init() override {
        const auto paths = ProbePaths();
        if (!paths) return;
        std::error_code ec;
        if (!fs::is_regular_file(paths->request, ec) || ec) return;
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;

        const Marker marker = ReadMarker(paths->claimed);
        DeleteFileW(paths->claimed.c_str());
        DeleteFileW(paths->result.c_str());
        auto state = std::make_shared<State>(paths->result);
        if (!marker.valid) {
            state->failures.push_back("invalid_probe_marker");
            WriteResult(*state, "invalid_probe_marker");
            return;
        }

        m_abort = std::make_unique<abort_callback_impl>();
        m_state = state;
        m_thread = std::thread([state, marker, this] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            const std::string fatal = RunCases(state, marker, *m_abort);
            bool cleanupOk = true;
            for (const auto& key : state->cleanupKeys)
                cleanupOk = RemoveTestKey(key) && cleanupOk;
            if (!cleanupOk) state->failures.push_back("cleanup_failed");
            WriteResult(*state, fatal);
            winrt::uninit_apartment();
        });
    }

    void on_quit() override {
        if (m_state) m_state->stopping = true;
        if (m_abort) m_abort->abort();
        if (m_thread.joinable()) m_thread.join();
        m_abort.reset();
        m_state.reset();
    }

  private:
    std::shared_ptr<State> m_state;
    std::thread m_thread;
    std::unique_ptr<abort_callback_impl> m_abort;
};

FB2K_SERVICE_FACTORY(ProbeLifecycle);

} // namespace

} // namespace glasswave::cache::image_probe
