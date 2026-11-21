#include "pch.h"
#include "cache/audio.h"
#include "core/text.h"

#include <SDK/audio_chunk_impl.h>
#include <SDK/file_info_impl.h>
#include <SDK/initquit.h>
#include <SDK/input.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string_view>

namespace glasswave::cache::audio::probe {
namespace {

namespace fs = std::filesystem;
namespace json = winrt::Windows::Data::Json;

using IJsonValue = json::IJsonValue;
using JsonArray = json::JsonArray;
using JsonObject = json::JsonObject;
using JsonValue = json::JsonValue;
using JsonValueType = json::JsonValueType;

constexpr std::string_view kProvider = "glasswave-cache-test";
constexpr uint64_t kMaxFixtureBytes = 8ull * 1024 * 1024;
constexpr size_t kMaxRequestBytes = 64 * 1024;
constexpr auto kWaitTimeout = std::chrono::seconds(10);
constexpr uint64_t kParallelPatternBytes = 8ull * 1024 * 1024;
constexpr uint64_t kParallelPrefixBytes = 5ull * 1024 * 1024;
constexpr size_t kParallelReaderCount = 8;
constexpr size_t kParallelChunkBytes = 32 * 1024;
constexpr size_t kParallelPassIterations = 64;
constexpr uint64_t kParallelPassBytes = kParallelReaderCount * kParallelChunkBytes * kParallelPassIterations;

struct ProbePaths {
    fs::path directory;
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct ProbeRequest {
    std::wstring run_id;
    fs::path fixture;
};

struct FixtureInfo {
    std::wstring codec;
    std::string codec_key;
    std::string extension;
    uint32_t sample_rate = 0;
    uint32_t bit_depth = 0;
    uint32_t channels = 0;
    double duration_seconds = 0;
};

struct CaseResult {
    std::wstring name;
    std::wstring error;
    bool ok = false;
    std::optional<bool> progressive;
    std::optional<bool> cache_hit;
    std::optional<bool> positions_independent;
    std::optional<bool> cancel_isolated;
    std::optional<bool> prefetch_promoted;
    std::optional<bool> bytes_match;
    std::optional<bool> publish_visible;
    std::optional<bool> progressive_size_known;
    std::optional<bool> progressive_can_seek;
    std::optional<bool> completed_size_known;
    std::optional<bool> completed_can_seek;
    bool nonpersistent_fallback = false;
    uint64_t producer_calls = 0;
    uint64_t fetcher_calls = 0;
    uint64_t first_position = 0;
    uint64_t second_position = 0;
    uint64_t prefix_bytes = 0;
    uint64_t decoded_samples = 0;
    uint64_t staging_before = 0;
    uint64_t staging_during = 0;
    uint64_t staging_after = 0;
    uint64_t parallel_readers = 0;
    uint64_t parallel_passes = 0;
    uint64_t bytes_verified = 0;
    double first_decoded_ms = -1;
    size_t active_flights = 0;
};

struct ProbeResult {
    bool ok = false;
    bool key_invariants = false;
    bool prefetch_scope = false;
    std::wstring run_id;
    std::wstring fixture_name;
    std::wstring codec;
    std::wstring error;
    uint64_t fixture_bytes = 0;
    uint32_t sample_rate = 0;
    uint32_t bit_depth = 0;
    uint32_t channels = 0;
    double duration_seconds = 0;
    size_t cleanup_errors = 0;
    std::vector<CaseResult> cases;
};

using core::Utf8;
using core::Wide;

bool SameName(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool IsWithin(const fs::path& candidate, const fs::path& root) {
    auto candidatePart = candidate.begin();
    for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart) {
        if (candidatePart == candidate.end() || !SameName(*candidatePart, *rootPart)) return false;
        ++candidatePart;
    }
    return candidatePart != candidate.end();
}

std::optional<fs::path> FinalPath(const fs::path& path, bool directory) {
    const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (!length || length >= buffer.size()) return std::nullopt;

    std::wstring text(buffer.data(), length);
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        text = L"\\\\" + text.substr(8);
    } else if (text.rfind(L"\\\\?\\", 0) == 0) {
        text.erase(0, 4);
    }
    if (directory) {
        const size_t rootLength = fs::path(text).root_path().native().size();
        while (text.size() > rootLength && (text.back() == L'\\' || text.back() == L'/'))
            text.pop_back();
    }
    return fs::path(text).lexically_normal();
}

std::optional<fs::path> NativeProfileDirectory() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return FinalPath(fs::path(Wide(native.get_ptr())), true);
}

std::optional<fs::path> RepositoryRootFromDevProfile(const fs::path& profile) {
    if (!SameName(profile.filename(), fs::path(L"profile"))) return std::nullopt;
    const fs::path instance = profile.parent_path();
    const fs::path local = instance.parent_path();
    if (!SameName(instance.filename(), fs::path(L"fb2k-dev")) ||
        !SameName(local.filename(), fs::path(L".local"))) {
        return std::nullopt;
    }
    return local.parent_path();
}

std::optional<ProbePaths> ProbeFiles() {
    const auto profile = NativeProfileDirectory();
    if (!profile || !RepositoryRootFromDevProfile(*profile)) return std::nullopt;
    const fs::path directory = *profile / L"glasswave" / L"cache-probe";
    return ProbePaths{directory, directory / L"audio-request.json", directory / L"audio-request.claimed.json",
                      directory / L"audio-result.json"};
}

std::optional<std::string> ReadBounded(const fs::path& path, size_t limit) {
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    if (error || !size || size > limit) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) return std::nullopt;
    return bytes;
}

bool ValidRunId(std::wstring_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
               c == L'-' || c == L'_';
    });
}

std::optional<ProbeRequest> ParseRequest(std::string_view bytes, const fs::path& profile,
                                         std::wstring& error) {
    try {
        const JsonObject object = JsonObject::Parse(winrt::to_hstring(bytes));
        const IJsonValue run_id = object.Lookup(L"runId");
        const IJsonValue fixture_path = object.Lookup(L"fixturePath");
        if (!run_id || run_id.ValueType() != JsonValueType::String || !fixture_path ||
            fixture_path.ValueType() != JsonValueType::String) {
            error = L"invalid_request";
            return std::nullopt;
        }

        const std::wstring id = run_id.GetString().c_str();
        if (!ValidRunId(id)) {
            error = L"invalid_run_id";
            return std::nullopt;
        }
        const auto repository = RepositoryRootFromDevProfile(profile);
        const auto allowed =
            repository ? FinalPath(*repository / L".local" / L"tests", true) : std::optional<fs::path>();
        const auto fixture = FinalPath(fs::path(fixture_path.GetString().c_str()), false);
        if (!allowed || !fixture || !fs::is_regular_file(*fixture) || !IsWithin(*fixture, *allowed)) {
            error = L"fixture_outside_allowed_root";
            return std::nullopt;
        }
        std::wstring extension = fixture->extension().wstring();
        for (wchar_t& c : extension)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + L'a' - L'A');
        const uintmax_t size = fs::file_size(*fixture);
        if ((extension != L".m4a" && extension != L".flac" && extension != L".mp3") || !size ||
            size > kMaxFixtureBytes) {
            error = L"invalid_fixture";
            return std::nullopt;
        }
        return ProbeRequest{id, *fixture};
    } catch (...) {
        error = L"invalid_json";
        return std::nullopt;
    }
}

std::vector<uint8_t> ReadFixture(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw exception_io_data("Cannot open the cache probe fixture");
    const uintmax_t size = fs::file_size(path);
    if (!size || size > kMaxFixtureBytes) throw exception_io_data("Cache probe fixture is invalid");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size()))
        throw exception_io_data("Cache probe fixture read failed");
    return bytes;
}

std::optional<std::wstring> ActualCodec(const file_info& info) {
    const char* value = info.info_get("codec");
    if (!value || !*value) {
        pfc::string8 codec;
        if (info.info_get_codec_long(codec)) value = codec.get_ptr();
    }
    if (!value || !*value) return std::nullopt;
    return Wide(value);
}

bool Contains(std::wstring_view text, std::wstring_view needle) {
    auto lower = [](std::wstring_view value) {
        std::wstring result(value);
        for (wchar_t& c : result)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + L'a' - L'A');
        return result;
    };
    return lower(text).find(lower(needle)) != std::wstring::npos;
}

FixtureInfo InspectFixture(const fs::path& path, abort_callback& abort) {
    service_ptr_t<input_decoder> decoder;
    input_entry::g_open_for_decoding(decoder, nullptr, Utf8(path.wstring()).c_str(), abort);
    if (!decoder.is_valid()) throw exception_io_data("Cache probe fixture has no decoder");
    constexpr unsigned flags = input_flag_no_looping | input_flag_testing_integrity | input_flag_no_postproc |
                               input_flag_canonical_decode;
    decoder->initialize(0, flags, abort);
    file_info_impl info;
    decoder->get_info(0, info, abort);

    FixtureInfo result;
    const auto codec = ActualCodec(info);
    if (!codec || (!Contains(*codec, L"ALAC") && !Contains(*codec, L"AAC") && !Contains(*codec, L"FLAC") &&
                   !Contains(*codec, L"MP3")))
        throw exception_io_data("Cache probe requires ALAC, AAC, FLAC or MP3");
    result.codec = *codec;
    result.extension = Utf8(path.extension().wstring());
    result.codec_key = Utf8([&] {
        std::wstring value = *codec;
        for (wchar_t& c : value)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + L'a' - L'A');
        return value;
    }());
    result.sample_rate = static_cast<uint32_t>(std::max<int64_t>(0, info.info_get_int("samplerate")));
    result.bit_depth = static_cast<uint32_t>(std::max<int64_t>(0, info.info_get_int("bitspersample")));
    result.channels = static_cast<uint32_t>(std::max<int64_t>(0, info.info_get_int("channels")));
    if (!result.sample_rate || !result.channels) {
        audio_chunk_impl chunk;
        if (decoder->run(chunk, abort) && chunk.get_sample_count()) {
            result.sample_rate = chunk.get_sample_rate();
            result.channels = chunk.get_channel_count();
        }
    }
    decoder.release();
    result.duration_seconds = info.get_length();
    if (!result.sample_rate || !result.channels || !std::isfinite(result.duration_seconds) ||
        result.duration_seconds < 2.0 || result.duration_seconds > 5.0) {
        throw exception_io_data("Cache probe fixture must be a three-second audio file");
    }
    return result;
}

template <typename Predicate>
bool WaitUntil(
    Predicate predicate, abort_callback& abort,
    std::chrono::milliseconds timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kWaitTimeout)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        abort.check();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    abort.check();
    return predicate();
}

struct ProducerControl {
    mutable std::mutex mutex;
    std::condition_variable changed;
    uint64_t calls = 0;
    uint64_t prefix_bytes = 0;
    bool started = false;
    bool prefix_ready = false;
    bool released = false;
    bool finished = false;
    bool failed = false;

    void Start(uint64_t prefix) {
        std::lock_guard lock(mutex);
        ++calls;
        started = true;
        prefix_bytes = prefix;
        changed.notify_all();
    }

    void PrefixReady(uint64_t available) {
        std::lock_guard lock(mutex);
        prefix_bytes = available;
        prefix_ready = true;
        changed.notify_all();
    }

    void Release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }

    void Finish(bool success) {
        {
            std::lock_guard lock(mutex);
            finished = true;
            failed = !success;
        }
        changed.notify_all();
    }

    bool Started() const {
        std::lock_guard lock(mutex);
        return started;
    }
    bool PrefixReady() const {
        std::lock_guard lock(mutex);
        return prefix_ready;
    }
    bool Finished() const {
        std::lock_guard lock(mutex);
        return finished;
    }
    bool Released() const {
        std::lock_guard lock(mutex);
        return released;
    }
    uint64_t Calls() const {
        std::lock_guard lock(mutex);
        return calls;
    }
    uint64_t PrefixBytes() const {
        std::lock_guard lock(mutex);
        return prefix_bytes;
    }

    void WaitReleased(abort_callback& abort) {
        while (true) {
            abort.check();
            {
                std::unique_lock lock(mutex);
                if (changed.wait_for(lock, std::chrono::milliseconds(10), [this] { return released; }))
                    return;
            }
        }
    }
};

struct ProducerReleaseGuard {
    std::shared_ptr<ProducerControl> control;
    bool armed = true;
    ~ProducerReleaseGuard() {
        if (armed && control) control->Release();
    }
};

class FixtureProducer {
  public:
    FixtureProducer(std::shared_ptr<const std::vector<uint8_t>> bytes,
                    std::shared_ptr<ProducerControl> control, size_t prefix)
        : m_bytes(std::move(bytes)), m_control(std::move(control)),
          m_prefix(std::min(prefix, m_bytes->empty() ? 0 : m_bytes->size() - 1)) {}

    void operator()(Sink& sink, abort_callback& abort) const {
        m_control->Start(m_prefix);
        try {
            WriteRange(sink, 0, m_prefix, abort);
            m_control->PrefixReady(sink.Size());
            m_control->WaitReleased(abort);
            WriteRange(sink, m_prefix, m_bytes->size(), abort);
            m_control->Finish(true);
        } catch (...) {
            m_control->Finish(false);
            throw;
        }
    }

  private:
    void WriteRange(Sink& sink, size_t begin, size_t end, abort_callback& abort) const {
        constexpr size_t chunkSize = 16 * 1024;
        while (begin < end) {
            abort.check();
            const size_t count = std::min(chunkSize, end - begin);
            sink.Write(m_bytes->data() + begin, count, abort);
            begin += count;
        }
    }

    std::shared_ptr<const std::vector<uint8_t>> m_bytes;
    std::shared_ptr<ProducerControl> m_control;
    size_t m_prefix;
};

uint8_t PatternByte(uint64_t offset) noexcept {
    uint64_t value = offset + 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    return static_cast<uint8_t>(value ^ (value >> 8));
}

void FillPattern(uint8_t* bytes, uint64_t offset, size_t size) noexcept {
    for (size_t index = 0; index < size; ++index)
        bytes[index] = PatternByte(offset + index);
}

bool MatchesPattern(const uint8_t* bytes, uint64_t offset, size_t size) noexcept {
    for (size_t index = 0; index < size; ++index) {
        if (bytes[index] != PatternByte(offset + index)) return false;
    }
    return true;
}

class PatternProducer {
  public:
    PatternProducer(uint64_t total, uint64_t prefix, std::shared_ptr<ProducerControl> control)
        : m_total(total), m_prefix(prefix), m_control(std::move(control)) {}

    void operator()(Sink& sink, abort_callback& abort) const {
        m_control->Start(m_prefix);
        try {
            WriteRange(sink, 0, m_prefix, abort);
            m_control->PrefixReady(sink.Size());
            m_control->WaitReleased(abort);
            WriteRange(sink, m_prefix, m_total, abort);
            m_control->Finish(true);
        } catch (...) {
            m_control->Finish(false);
            throw;
        }
    }

  private:
    void WriteRange(Sink& sink, uint64_t begin, uint64_t end, abort_callback& abort) const {
        std::array<uint8_t, 16 * 1024> chunk{};
        while (begin < end) {
            abort.check();
            const size_t count = static_cast<size_t>(std::min<uint64_t>(chunk.size(), end - begin));
            FillPattern(chunk.data(), begin, count);
            sink.Write(chunk.data(), count, abort);
            begin += count;
        }
    }

    uint64_t m_total;
    uint64_t m_prefix;
    std::shared_ptr<ProducerControl> m_control;
};

Metadata ProbeMetadata(const FixtureInfo& fixtureInfo) {
    Metadata metadata;
    metadata.contentType = fixtureInfo.extension == ".flac"  ? "audio/flac"
                           : fixtureInfo.extension == ".mp3" ? "audio/mpeg"
                                                             : "audio/mp4";
    metadata.validatedAt = Store::Now();
    metadata.extra = "schema=audio-probe-v1;producer=" + std::string(kProducerSalt);
    return metadata;
}

Key MakeProbeKey(std::wstring_view scope, std::wstring_view runId, std::wstring_view suffix,
                 const FixtureInfo& info, uint64_t bytes) {
    Source source;
    source.provider = std::string(kProvider);
    source.accountScope = Utf8(scope);
    source.identity = Utf8(std::wstring(runId) + L"-" + std::wstring(suffix));
    source.quality = info.codec_key + ":" + std::to_string(info.sample_rate) + ":" +
                     std::to_string(info.bit_depth) + ":" + std::to_string(info.channels);
    source.codec = info.codec_key;
    source.sourceVersion = std::string(kProducerSalt) + ":fixture:" + std::to_string(bytes);
    return MakeKey(std::move(source));
}

bool ReadExact(file::ptr stream, void* bytes, size_t size, abort_callback& abort) {
    return stream.is_valid() && stream->read(bytes, size, abort) == static_cast<t_size>(size);
}

class StartGate {
  public:
    explicit StartGate(size_t participants) : m_remaining(participants) {}

    void Wait() {
        std::unique_lock lock(m_mutex);
        if (--m_remaining == 0) {
            m_open = true;
            m_changed.notify_all();
            return;
        }
        m_changed.wait(lock, [this] { return m_open; });
    }

    void Open() {
        std::lock_guard lock(m_mutex);
        m_open = true;
        m_changed.notify_all();
    }

  private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    size_t m_remaining;
    bool m_open = false;
};

bool RunParallelPass(const std::vector<file::ptr>& readers, const std::vector<uint64_t>& positions,
                     abort_callback& abort, uint64_t& verified) {
    if (readers.size() != kParallelReaderCount || positions.size() != readers.size()) return false;

    struct ReaderResult {
        std::exception_ptr failure;
        uint64_t bytes = 0;
    };
    std::array<ReaderResult, kParallelReaderCount> results{};
    std::atomic_bool stopped = false;
    StartGate gate(readers.size());
    const auto run = [&](size_t index) {
        try {
            std::array<uint8_t, kParallelChunkBytes> buffer{};
            uint64_t offset = positions[index];
            gate.Wait();
            for (size_t iteration = 0;
                 iteration < kParallelPassIterations && !stopped.load(std::memory_order_relaxed);
                 ++iteration) {
                const t_size read = readers[index]->read(buffer.data(), buffer.size(), abort);
                if (read != buffer.size() || !MatchesPattern(buffer.data(), offset, read))
                    throw exception_io_data("Parallel audio cache read returned the wrong bytes");
                offset += read;
                results[index].bytes += read;
            }
        } catch (...) {
            results[index].failure = std::current_exception();
            stopped.store(true, std::memory_order_relaxed);
        }
    };

    std::array<std::thread, kParallelReaderCount> workers;
    try {
        for (size_t index = 1; index < readers.size(); ++index)
            workers[index] = std::thread(run, index);
        run(0);
    } catch (...) {
        gate.Open();
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
        throw;
    }
    for (auto& worker : workers)
        if (worker.joinable()) worker.join();

    for (const auto& result : results)
        if (result.failure) std::rethrow_exception(result.failure);
    for (const auto& result : results)
        verified += result.bytes;
    return true;
}

uint64_t PrepareSequentialReader(file::ptr reader, uint64_t offset, uint64_t& verified,
                                 abort_callback& abort) {
    std::array<uint8_t, kParallelChunkBytes> buffer{};
    uint64_t position = 0;
    while (position < offset) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), offset - position));
        if (reader->read(buffer.data(), count, abort) != count ||
            !MatchesPattern(buffer.data(), position, count)) {
            throw exception_io_data("Parallel audio cache prefill returned the wrong bytes");
        }
        position += count;
        verified += count;
    }
    return position;
}

bool WaitNoFlights(abort_callback& abort);

struct ParallelVariantResult {
    bool progressive_size_known = true;
    bool progressive_can_seek = true;
    bool completed_size_known = true;
    bool completed_can_seek = true;
    bool nonpersistent_fallback = false;
    uint64_t passes = 0;
    uint64_t bytes = 0;
};

std::optional<std::wstring> RunParallelVariant(bool persistent, const std::wstring& scope,
                                               const std::wstring& runId, const FixtureInfo& fixtureInfo,
                                               abort_callback& abort, std::vector<Key>& keys,
                                               ParallelVariantResult& result) {
    try {
        auto control = std::make_shared<ProducerControl>();
        const Key key = MakeProbeKey(scope, runId, persistent ? L"parallel-persistent" : L"parallel-fallback",
                                     fixtureInfo, kParallelPatternBytes);
        keys.push_back(key);
        ProducerReleaseGuard releaseGuard{control};
        auto opened =
            OpenOrStart(Request{key, ProbeMetadata(fixtureInfo),
                                PatternProducer(kParallelPatternBytes, kParallelPrefixBytes, control),
                                Workload::Playback, persistent},
                        abort);
        if (!opened.admitted || !opened.flight || !opened.stream.is_valid())
            return L"parallel_transfer_not_admitted";
        if (!WaitUntil([&] { return control->PrefixReady(); }, abort))
            return L"parallel_producer_prefix_timeout";

        std::vector<file::ptr> readers;
        readers.reserve(kParallelReaderCount);
        readers.push_back(opened.stream);
        for (size_t index = 1; index < kParallelReaderCount; ++index)
            readers.push_back(opened.flight->Sequential());
        if (readers.size() != kParallelReaderCount) return L"parallel_reader_creation_failed";

        std::vector<uint64_t> positions;
        positions.reserve(kParallelReaderCount);
        for (size_t index = 0; index < readers.size(); ++index) {
            result.progressive_size_known &= readers[index]->get_size(abort) != filesize_invalid;
            result.progressive_can_seek &= readers[index]->can_seek();
            positions.push_back(
                PrepareSequentialReader(readers[index], index * kParallelChunkBytes, result.bytes, abort));
        }
        if (!RunParallelPass(readers, positions, abort, result.bytes))
            return L"parallel_before_completion_pass_failed";
        for (uint64_t& position : positions)
            position += kParallelChunkBytes * kParallelPassIterations;
        ++result.passes;

        control->Release();
        releaseGuard.armed = false;
        if (!WaitUntil([&] { return opened.flight->Done() || opened.flight->Failed(); }, abort))
            return L"parallel_producer_completion_timeout";
        if (opened.flight->Failed()) return L"parallel_producer_failed";
        if (opened.flight->Available() != kParallelPatternBytes) return L"parallel_completed_size_mismatch";

        const bool published = GetStore().Lookup(key).has_value();
        if (published != persistent) return L"parallel_persistence_mismatch";
        result.nonpersistent_fallback = !persistent && !published;
        for (size_t index = 0; index < readers.size(); ++index) {
            result.progressive_size_known &= readers[index]->get_size(abort) != filesize_invalid;
            result.progressive_can_seek &= readers[index]->can_seek();
            if (readers[index]->get_position(abort) != positions[index])
                return L"parallel_source_position_mismatch";
        }
        if (!RunParallelPass(readers, positions, abort, result.bytes))
            return L"parallel_after_completion_pass_failed";
        for (uint64_t& position : positions)
            position += kParallelChunkBytes * kParallelPassIterations;
        ++result.passes;
        readers.clear();

        std::vector<file::ptr> completed;
        completed.reserve(kParallelReaderCount);
        for (size_t index = 0; index < kParallelReaderCount; ++index)
            completed.push_back(opened.flight->Completed(abort));
        positions.clear();
        for (size_t index = 0; index < completed.size(); ++index) {
            result.completed_size_known &= completed[index]->get_size(abort) == kParallelPatternBytes;
            result.completed_can_seek &= completed[index]->can_seek();
            const uint64_t position = index * kParallelChunkBytes;
            completed[index]->seek(position, abort);
            positions.push_back(position);
        }
        if (!RunParallelPass(completed, positions, abort, result.bytes))
            return L"parallel_completed_reader_pass_failed";
        ++result.passes;
        completed.clear();
        opened.flight.reset();
        if (!WaitNoFlights(abort)) return L"parallel_active_flight_timeout";
        return std::nullopt;
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        return L"parallel_unexpected_exception";
    }
}

std::optional<std::vector<uint8_t>> ReadAll(file::ptr stream, abort_callback& abort) {
    std::vector<uint8_t> result;
    std::array<uint8_t, 64 * 1024> buffer{};
    for (;;) {
        abort.check();
        const t_size read = stream->read(buffer.data(), buffer.size(), abort);
        if (!read) break;
        if (result.size() + read > kMaxFixtureBytes) return std::nullopt;
        result.insert(result.end(), buffer.begin(), buffer.begin() + read);
    }
    return result;
}

bool SameBytes(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

bool SameSnapshot(const Snapshot& snapshot, const std::vector<uint8_t>& expected) {
    if (snapshot.Size() != expected.size()) return false;
    std::ifstream input(snapshot.Path(), std::ios::binary);
    if (!input) return false;
    std::array<char, 64 * 1024> buffer{};
    size_t offset = 0;
    while (offset < expected.size()) {
        const size_t count = std::min(buffer.size(), expected.size() - offset);
        input.read(buffer.data(), static_cast<std::streamsize>(count));
        if (!input || input.gcount() != static_cast<std::streamsize>(count) ||
            std::memcmp(buffer.data(), expected.data() + offset, count) != 0)
            return false;
        offset += count;
    }
    return true;
}

std::set<std::wstring> StagingFiles() {
    std::set<std::wstring> files;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(GetStore().Root() / L"staging", error)) {
        if (error) break;
        if (entry.is_regular_file()) files.insert(entry.path().filename().wstring());
    }
    return files;
}

size_t DifferenceCount(const std::set<std::wstring>& base, const std::set<std::wstring>& after) {
    size_t count = 0;
    for (const auto& name : after)
        if (!base.contains(name)) ++count;
    return count;
}

bool WaitNoFlights(abort_callback& abort) {
    return WaitUntil([] { return ActiveFlights() == 0; }, abort);
}

CaseResult Failed(std::wstring name, std::wstring error) {
    CaseResult result;
    result.name = std::move(name);
    result.error = std::move(error);
    return result;
}

CaseResult ProgressiveAndCacheHit(const std::wstring& scope, const std::wstring& runId,
                                  const std::vector<uint8_t>& fixture, const FixtureInfo& fixtureInfo,
                                  abort_callback& abort, std::vector<Key>& keys) {
    CaseResult result;
    result.name = L"progressive_first_decode";
    auto payload = std::make_shared<const std::vector<uint8_t>>(fixture);
    auto control = std::make_shared<ProducerControl>();
    const size_t prefix = fixture.size() > 8192 ? fixture.size() - 8192 : fixture.size() * 3 / 4;
    const Key key = MakeProbeKey(scope, runId, L"progress", fixtureInfo, fixture.size());
    keys.push_back(key);

    ProducerReleaseGuard releaseGuard{control};
    auto opened = OpenOrStart(Request{key, ProbeMetadata(fixtureInfo),
                                      FixtureProducer(payload, control, prefix), Workload::Playback, true},
                              abort);
    if (!opened.admitted || !opened.flight || !opened.stream.is_valid())
        return Failed(result.name, L"transfer_not_admitted");
    if (!WaitUntil([&] { return control->PrefixReady(); }, abort))
        return Failed(result.name, L"producer_prefix_timeout");

    const auto decodeStarted = std::chrono::steady_clock::now();
    service_ptr_t<input_decoder> decoder;
    const std::string decodePath = "encoded-cache-probe" + fixtureInfo.extension;
    input_entry::g_open_for_decoding(decoder, opened.stream, decodePath.c_str(), abort, true);
    constexpr unsigned decodeFlags = input_flag_no_looping | input_flag_testing_integrity |
                                     input_flag_no_postproc | input_flag_canonical_decode;
    decoder->initialize(0, decodeFlags, abort);
    file_info_impl decodedInfo;
    decoder->get_info(0, decodedInfo, abort);
    const auto decodedCodec = ActualCodec(decodedInfo);
    const bool formatMatches =
        decodedCodec && Contains(*decodedCodec, fixtureInfo.codec) &&
        decodedInfo.info_get_int("samplerate") == fixtureInfo.sample_rate &&
        decodedInfo.info_get_int("channels") == fixtureInfo.channels &&
        (!fixtureInfo.bit_depth || decodedInfo.info_get_int("bitspersample") == fixtureInfo.bit_depth);

    audio_chunk_impl chunk;
    uint64_t samples = 0;
    if (!decoder->run(chunk, abort) || !chunk.get_sample_count())
        return Failed(result.name, L"first_decoded_chunk_missing");
    result.first_decoded_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - decodeStarted).count();
    const uint32_t decodedRate = chunk.get_sample_rate();
    const uint32_t decodedChannels = chunk.get_channel_count();
    result.progressive = !control->Finished() && opened.flight->Available() < fixture.size();
    result.prefix_bytes = opened.flight->Available();
    samples += chunk.get_sample_count();
    control->Release();
    releaseGuard.armed = false;

    while (decoder->run(chunk, abort)) {
        if (chunk.get_sample_count()) {
            if (chunk.get_sample_rate() != decodedRate || chunk.get_channel_count() != decodedChannels)
                return Failed(result.name, L"decoded_format_changed");
            samples += chunk.get_sample_count();
        }
    }
    decoder.release();
    if (!WaitUntil([&] { return opened.flight->Done() || opened.flight->Failed(); }, abort))
        return Failed(result.name, L"producer_completion_timeout");
    if (opened.flight->Failed()) return Failed(result.name, L"producer_failed");
    if (!WaitNoFlights(abort)) return Failed(result.name, L"active_flight_timeout");

    abort_callback_dummy sourceViewAbort;
    result.progressive_size_known = opened.stream->get_size(sourceViewAbort) != filesize_invalid;
    result.progressive_can_seek = opened.stream->can_seek();
    auto snapshot = GetStore().Lookup(key);
    result.bytes_match = snapshot && SameSnapshot(*snapshot, fixture);
    result.decoded_samples = samples;
    result.producer_calls = control->Calls();
    const double decodedSeconds = decodedRate ? static_cast<double>(samples) / decodedRate : 0;
    const bool durationMatches =
        decodedSeconds > 0 && std::abs(decodedSeconds - fixtureInfo.duration_seconds) <= 0.125;
    result.ok = *result.progressive && !*result.progressive_size_known && !*result.progressive_can_seek &&
                *result.bytes_match && formatMatches && durationMatches && result.producer_calls == 1;
    if (!result.ok) result.error = L"progressive_verification_failed";
    return result;
}

CaseResult CacheHitSeekable(const Key& key, const std::vector<uint8_t>& fixture,
                            const FixtureInfo& fixtureInfo, abort_callback& abort) {
    CaseResult result;
    result.name = L"cache_hit";
    auto fetcherCalls = std::make_shared<std::atomic<uint64_t>>(0);
    auto opened = OpenOrStart(Request{key, ProbeMetadata(fixtureInfo),
                                      [fetcherCalls](Sink&, abort_callback&) {
                                          fetcherCalls->fetch_add(1);
                                          throw exception_io_data("Cache hit unexpectedly fetched");
                                      },
                                      Workload::Playback, true},
                              abort);
    result.cache_hit = opened.fromCache && !opened.flight && opened.stream.is_valid();
    result.fetcher_calls = fetcherCalls->load();
    auto bytes = ReadAll(opened.stream, abort);
    result.bytes_match = bytes && SameBytes(*bytes, fixture);
    result.ok = *result.cache_hit && result.fetcher_calls == 0 && *result.bytes_match;
    if (!result.ok) result.error = L"cache_hit_verification_failed";
    return result;
}

CaseResult ConcurrentReaders(const std::wstring& scope, const std::wstring& runId,
                             const std::vector<uint8_t>& fixture, const FixtureInfo& fixtureInfo,
                             abort_callback& abort, std::vector<Key>& keys) {
    CaseResult result;
    result.name = L"concurrent_readers";
    auto payload = std::make_shared<const std::vector<uint8_t>>(fixture);
    auto control = std::make_shared<ProducerControl>();
    const size_t prefix = std::min<size_t>(fixture.size() - 1, 96 * 1024);
    const Key key = MakeProbeKey(scope, runId, L"concurrent", fixtureInfo, fixture.size());
    keys.push_back(key);
    ProducerReleaseGuard releaseGuard{control};

    auto first = [&] {
        PrefetchScope speculative;
        return OpenOrStart(Request{key, ProbeMetadata(fixtureInfo), FixtureProducer(payload, control, prefix),
                                   Workload::Inherit, true},
                           abort);
    }();
    if (!first.admitted || !first.flight || !first.stream.is_valid())
        return Failed(result.name, L"first_reader_not_admitted");
    if (!WaitUntil([&] { return control->PrefixReady(); }, abort))
        return Failed(result.name, L"producer_prefix_timeout");
    const bool countedAsPrefetch = ActivePrefetchFlights() == 1;

    auto secondFetchCalls = std::make_shared<std::atomic<uint64_t>>(0);
    auto second = OpenOrStart(Request{key, ProbeMetadata(fixtureInfo),
                                      [secondFetchCalls](Sink&, abort_callback&) {
                                          secondFetchCalls->fetch_add(1);
                                          throw exception_io_data("Joined reader fetched");
                                      },
                                      Workload::Playback, true},
                              abort);
    if (!second.admitted || !second.flight || !second.stream.is_valid())
        return Failed(result.name, L"second_reader_not_adopted");
    result.prefetch_promoted = countedAsPrefetch && ActivePrefetchFlights() == 0;

    std::array<uint8_t, 4096> firstPrefix{};
    std::array<uint8_t, 1024> secondPrefix{};
    std::array<uint8_t, 1024> firstNext{};
    if (!ReadExact(first.stream, firstPrefix.data(), firstPrefix.size(), abort) ||
        !ReadExact(second.stream, secondPrefix.data(), secondPrefix.size(), abort) ||
        !ReadExact(first.stream, firstNext.data(), firstNext.size(), abort))
        return Failed(result.name, L"reader_prefix_failed");

    abort_callback_dummy positionAbort;
    result.first_position = first.stream->get_position(positionAbort);
    result.second_position = second.stream->get_position(positionAbort);
    result.positions_independent =
        result.first_position == 5120 && result.second_position == 1024 &&
        std::memcmp(firstPrefix.data(), fixture.data(), firstPrefix.size()) == 0 &&
        std::memcmp(firstNext.data(), fixture.data() + firstPrefix.size(), firstNext.size()) == 0 &&
        std::memcmp(secondPrefix.data(), fixture.data(), secondPrefix.size()) == 0;

    abort_callback_impl cancelFirst;
    cancelFirst.set();
    bool firstCancelled = false;
    try {
        std::array<uint8_t, 1> ignored{};
        first.stream->read(ignored.data(), ignored.size(), cancelFirst);
    } catch (const exception_aborted&) {
        firstCancelled = true;
    }
    first.stream.release();
    first.flight.reset();
    control->Release();
    releaseGuard.armed = false;

    auto survivor = ReadAll(second.stream, abort);
    result.cancel_isolated =
        firstCancelled && survivor && survivor->size() == fixture.size() - secondPrefix.size() &&
        std::memcmp(survivor->data(), fixture.data() + secondPrefix.size(), survivor->size()) == 0;
    result.producer_calls = control->Calls();
    result.fetcher_calls = secondFetchCalls->load();
    result.ok = *result.positions_independent && *result.cancel_isolated && *result.prefetch_promoted &&
                result.producer_calls == 1 && result.fetcher_calls == 0;
    if (!result.ok) result.error = L"concurrent_reader_verification_failed";
    return result;
}

CaseResult ConcurrentReaderIo(const std::wstring& scope, const std::wstring& runId,
                              const FixtureInfo& fixtureInfo, abort_callback& abort, std::vector<Key>& keys) {
    CaseResult result;
    result.name = L"concurrent_reader_io";
    ParallelVariantResult persistent;
    ParallelVariantResult fallback;
    if (const auto error = RunParallelVariant(true, scope, runId, fixtureInfo, abort, keys, persistent)) {
        return Failed(result.name, *error);
    }
    if (const auto error = RunParallelVariant(false, scope, runId, fixtureInfo, abort, keys, fallback)) {
        return Failed(result.name, *error);
    }

    result.parallel_readers = kParallelReaderCount;
    result.parallel_passes = persistent.passes + fallback.passes;
    result.bytes_verified = persistent.bytes + fallback.bytes;
    result.nonpersistent_fallback = fallback.nonpersistent_fallback;
    result.progressive_size_known = persistent.progressive_size_known && fallback.progressive_size_known;
    result.progressive_can_seek = persistent.progressive_can_seek && fallback.progressive_can_seek;
    result.completed_size_known = persistent.completed_size_known && fallback.completed_size_known;
    result.completed_can_seek = persistent.completed_can_seek && fallback.completed_can_seek;
    result.ok = result.nonpersistent_fallback && !*result.progressive_size_known &&
                !*result.progressive_can_seek && *result.completed_size_known && *result.completed_can_seek &&
                result.parallel_readers == kParallelReaderCount && result.parallel_passes == 6 &&
                result.bytes_verified >= kParallelPassBytes * 6;
    if (!result.ok) result.error = L"parallel_reader_io_verification_failed";
    return result;
}

CaseResult InvalidateDuringProduce(const std::wstring& scope, const std::wstring& runId,
                                   const std::vector<uint8_t>& fixture, const FixtureInfo& fixtureInfo,
                                   abort_callback& abort, std::vector<Key>& keys) {
    CaseResult result;
    result.name = L"invalidate_during_produce";
    auto payload = std::make_shared<const std::vector<uint8_t>>(fixture);
    auto control = std::make_shared<ProducerControl>();
    const Key key = MakeProbeKey(scope, runId, L"invalidate", fixtureInfo, fixture.size());
    keys.push_back(key);
    auto opened = OpenOrStart(
        Request{key, ProbeMetadata(fixtureInfo),
                FixtureProducer(payload, control, std::min<size_t>(fixture.size() - 1, 64 * 1024)),
                Workload::Playback, true},
        abort);
    if (!opened.admitted || !opened.flight || !opened.stream.is_valid())
        return Failed(result.name, L"transfer_not_admitted");
    if (!WaitUntil([&] { return control->PrefixReady(); }, abort))
        return Failed(result.name, L"producer_prefix_timeout");

    std::thread invalidator([&] { Invalidate(key); });
    std::thread releaser([&] { control->Release(); });
    invalidator.join();
    releaser.join();
    if (!WaitUntil([&] { return opened.flight->Done() || opened.flight->Failed(); }, abort))
        return Failed(result.name, L"invalidation_timeout");
    opened.stream.release();
    opened.flight.reset();
    result.active_flights = ActiveFlights();
    if (!WaitNoFlights(abort)) return Failed(result.name, L"active_flight_timeout");
    result.active_flights = ActiveFlights();
    result.publish_visible = GetStore().Lookup(key).has_value();
    result.producer_calls = control->Calls();
    result.ok = !*result.publish_visible && result.active_flights == 0;
    if (!result.ok) result.error = L"late_publish_detected";
    return result;
}

CaseResult RotatedScopeRejectsPublish(const std::wstring& runId, const std::vector<uint8_t>& fixture,
                                      const FixtureInfo& fixtureInfo, abort_callback& abort,
                                      std::vector<Key>& keys) {
    CaseResult result;
    result.name = L"scope_rotation";
    auto payload = std::make_shared<const std::vector<uint8_t>>(fixture);
    auto control = std::make_shared<ProducerControl>();
    const std::wstring oldScope = Wide(GetStore().Scope(std::string(kProvider)));
    const Key oldKey = MakeProbeKey(oldScope, runId, L"scope", fixtureInfo, fixture.size());
    keys.push_back(oldKey);
    ProducerReleaseGuard releaseGuard{control};
    auto opened = OpenOrStart(
        Request{oldKey, ProbeMetadata(fixtureInfo),
                FixtureProducer(payload, control, std::min<size_t>(fixture.size() - 1, 64 * 1024)),
                Workload::Playback, true},
        abort);
    if (!opened.admitted || !opened.flight || !opened.stream.is_valid())
        return Failed(result.name, L"transfer_not_admitted");
    if (!WaitUntil([&] { return control->PrefixReady(); }, abort))
        return Failed(result.name, L"producer_prefix_timeout");

    GetStore().RotateScope(std::string(kProvider));
    control->Release();
    releaseGuard.armed = false;
    if (!WaitUntil([&] { return opened.flight->Done() || opened.flight->Failed(); }, abort))
        return Failed(result.name, L"scope_commit_timeout");
    if (opened.flight->Failed()) return Failed(result.name, L"scope_commit_invalidated_playback");
    result.publish_visible = GetStore().Lookup(oldKey).has_value();
    auto completed = opened.flight->Completed(abort);
    auto bytes = ReadAll(completed, abort);
    result.bytes_match = bytes && SameBytes(*bytes, fixture);
    opened.stream.release();
    opened.flight.reset();
    if (!WaitNoFlights(abort)) return Failed(result.name, L"active_flight_timeout");
    result.active_flights = ActiveFlights();
    result.producer_calls = control->Calls();
    result.ok = !*result.publish_visible && *result.bytes_match && result.active_flights == 0;
    if (!result.ok) result.error = L"scope_rotation_verification_failed";
    return result;
}

CaseResult LastReleaseCleansStaging(const std::wstring& scope, const std::wstring& runId,
                                    const std::vector<uint8_t>& fixture, const FixtureInfo& fixtureInfo,
                                    abort_callback& abort, std::vector<Key>& keys) {
    CaseResult result;
    result.name = L"staging_last_release";
    auto payload = std::make_shared<const std::vector<uint8_t>>(fixture);
    auto control = std::make_shared<ProducerControl>();
    const Key key = MakeProbeKey(scope, runId, L"cleanup", fixtureInfo, fixture.size());
    keys.push_back(key);
    const auto baseline = StagingFiles();
    result.staging_before = baseline.size();
    auto opened = OpenOrStart(
        Request{key, ProbeMetadata(fixtureInfo),
                FixtureProducer(payload, control, std::min<size_t>(fixture.size() - 1, 64 * 1024)),
                Workload::Playback, true},
        abort);
    if (!opened.admitted || !opened.flight || !opened.stream.is_valid())
        return Failed(result.name, L"transfer_not_admitted");
    if (!WaitUntil([&] { return control->PrefixReady(); }, abort))
        return Failed(result.name, L"producer_prefix_timeout");
    const auto during = StagingFiles();
    result.staging_during = DifferenceCount(baseline, during);

    opened.stream.release();
    opened.flight.reset();
    if (!WaitNoFlights(abort)) return Failed(result.name, L"last_release_timeout");
    const auto after = StagingFiles();
    result.staging_after = DifferenceCount(baseline, after);
    result.active_flights = ActiveFlights();
    result.producer_calls = control->Calls();
    result.ok = result.staging_during > 0 && result.staging_after == 0 && result.active_flights == 0;
    if (!result.ok) result.error = L"staging_cleanup_failed";
    return result;
}

bool KeyInvariants(const FixtureInfo& info, uint64_t bytes) {
    Source base;
    base.provider = std::string(kProvider);
    base.accountScope = "scope-invariant";
    base.identity = "track-invariant";
    base.quality = info.codec_key + ":" + std::to_string(info.sample_rate);
    base.codec = info.codec_key;
    base.sourceVersion = "cache-v10:" + std::to_string(bytes);
    const Key original = MakeKey(base);
    const Key same = MakeKey(base);
    auto quality = base;
    quality.quality += ":changed";
    auto codec = base;
    codec.codec += "-changed";
    auto version = base;
    version.sourceVersion += ":changed";
    auto account = base;
    account.accountScope += "-changed";
    auto identity = base;
    identity.identity += "-changed";
    return original.kind == Kind::Audio && Store::Digest(original) == Store::Digest(same) &&
           Store::Digest(original) != Store::Digest(MakeKey(quality)) &&
           Store::Digest(original) != Store::Digest(MakeKey(codec)) &&
           Store::Digest(original) != Store::Digest(MakeKey(version)) &&
           Store::Digest(original) != Store::Digest(MakeKey(account)) &&
           Store::Digest(original) != Store::Digest(MakeKey(identity)) && original.variant == same.variant &&
           original.variant.find("http") == std::string::npos &&
           original.variant.find("token") == std::string::npos;
}

bool PrefetchScopeInvariant() {
    if (IsPrefetchThread()) return false;
    {
        PrefetchScope scope;
        if (!IsPrefetchThread()) return false;
    }
    return !IsPrefetchThread();
}

JsonObject CaseJson(const CaseResult& result) {
    JsonObject object;
    object.Insert(L"name", JsonValue::CreateStringValue(result.name));
    object.Insert(L"ok", JsonValue::CreateBooleanValue(result.ok));
    object.Insert(L"error", result.error.empty() ? JsonValue::CreateNullValue()
                                                 : JsonValue::CreateStringValue(result.error));
    const auto optionalBool = [&object](const wchar_t* name, const std::optional<bool>& value) {
        object.Insert(name, value ? JsonValue::CreateBooleanValue(*value) : JsonValue::CreateNullValue());
    };
    optionalBool(L"progressive", result.progressive);
    optionalBool(L"cacheHit", result.cache_hit);
    optionalBool(L"positionsIndependent", result.positions_independent);
    optionalBool(L"cancelIsolated", result.cancel_isolated);
    optionalBool(L"prefetchPromoted", result.prefetch_promoted);
    optionalBool(L"bytesMatch", result.bytes_match);
    optionalBool(L"publishVisible", result.publish_visible);
    optionalBool(L"progressiveSizeKnown", result.progressive_size_known);
    optionalBool(L"progressiveCanSeek", result.progressive_can_seek);
    optionalBool(L"completedSizeKnown", result.completed_size_known);
    optionalBool(L"completedCanSeek", result.completed_can_seek);
    object.Insert(L"nonpersistentFallback", JsonValue::CreateBooleanValue(result.nonpersistent_fallback));
    object.Insert(L"producerCalls", JsonValue::CreateNumberValue(static_cast<double>(result.producer_calls)));
    object.Insert(L"fetcherCalls", JsonValue::CreateNumberValue(static_cast<double>(result.fetcher_calls)));
    object.Insert(L"firstPosition", JsonValue::CreateNumberValue(static_cast<double>(result.first_position)));
    object.Insert(L"secondPosition",
                  JsonValue::CreateNumberValue(static_cast<double>(result.second_position)));
    object.Insert(L"prefixBytes", JsonValue::CreateNumberValue(static_cast<double>(result.prefix_bytes)));
    object.Insert(L"decodedSamples",
                  JsonValue::CreateNumberValue(static_cast<double>(result.decoded_samples)));
    object.Insert(L"firstDecodedMs", result.first_decoded_ms < 0
                                         ? JsonValue::CreateNullValue()
                                         : JsonValue::CreateNumberValue(result.first_decoded_ms));
    object.Insert(L"stagingBefore", JsonValue::CreateNumberValue(static_cast<double>(result.staging_before)));
    object.Insert(L"stagingDuring", JsonValue::CreateNumberValue(static_cast<double>(result.staging_during)));
    object.Insert(L"stagingAfter", JsonValue::CreateNumberValue(static_cast<double>(result.staging_after)));
    object.Insert(L"parallelReaders",
                  JsonValue::CreateNumberValue(static_cast<double>(result.parallel_readers)));
    object.Insert(L"parallelPasses",
                  JsonValue::CreateNumberValue(static_cast<double>(result.parallel_passes)));
    object.Insert(L"bytesVerified", JsonValue::CreateNumberValue(static_cast<double>(result.bytes_verified)));
    object.Insert(L"activeFlights", JsonValue::CreateNumberValue(static_cast<double>(result.active_flights)));
    return object;
}

JsonObject ResultJson(const ProbeResult& result) {
    JsonObject object;
    object.Insert(L"ok", JsonValue::CreateBooleanValue(result.ok));
    object.Insert(L"runId", JsonValue::CreateStringValue(result.run_id));
    object.Insert(L"provider", JsonValue::CreateStringValue(winrt::to_hstring(std::string(kProvider))));
    object.Insert(L"fixtureName", JsonValue::CreateStringValue(result.fixture_name));
    object.Insert(L"fixtureBytes", JsonValue::CreateNumberValue(static_cast<double>(result.fixture_bytes)));
    object.Insert(L"codec", JsonValue::CreateStringValue(result.codec));
    object.Insert(L"sampleRate", JsonValue::CreateNumberValue(static_cast<double>(result.sample_rate)));
    object.Insert(L"bitDepth", JsonValue::CreateNumberValue(static_cast<double>(result.bit_depth)));
    object.Insert(L"channels", JsonValue::CreateNumberValue(static_cast<double>(result.channels)));
    object.Insert(L"durationSeconds", JsonValue::CreateNumberValue(result.duration_seconds));
    object.Insert(L"keyInvariants", JsonValue::CreateBooleanValue(result.key_invariants));
    object.Insert(L"prefetchScope", JsonValue::CreateBooleanValue(result.prefetch_scope));
    object.Insert(L"cleanupErrors", JsonValue::CreateNumberValue(static_cast<double>(result.cleanup_errors)));
    JsonArray cases;
    for (const auto& item : result.cases)
        cases.Append(CaseJson(item));
    object.Insert(L"cases", cases);
    object.Insert(L"error", result.error.empty() ? JsonValue::CreateNullValue()
                                                 : JsonValue::CreateStringValue(result.error));
    return object;
}

bool WriteAll(HANDLE file, std::string_view bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count =
            static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || !written) return false;
        offset += written;
    }
    return true;
}

bool WriteResult(const fs::path& resultPath, const ProbeResult& result) {
    const fs::path temporary = resultPath.wstring() + L".tmp";
    const std::string bytes = winrt::to_string(ResultJson(result).Stringify());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = WriteAll(file, bytes) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written || !MoveFileExW(temporary.c_str(), resultPath.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void RunProbe(const ProbePaths paths, std::shared_ptr<abort_callback_impl> abort) {
    SetThreadDescription(GetCurrentThread(), L"Glasswave audio cache probe");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    ProbeResult result;

    const auto requestBytes = ReadBounded(paths.claimed, kMaxRequestBytes);
    DeleteFileW(paths.claimed.c_str());
    const auto profile = NativeProfileDirectory();
    std::optional<ProbeRequest> request;
    std::wstring parseError;
    if (!requestBytes) {
        parseError = L"request_read_failed";
    } else if (!profile) {
        parseError = L"profile_path_unavailable";
    } else {
        request = ParseRequest(*requestBytes, *profile, parseError);
    }

    if (!request) {
        result.error = parseError.empty() ? L"invalid_request" : parseError;
    } else {
        result.run_id = request->run_id;
        result.fixture_name = request->fixture.filename().wstring();
        std::vector<Key> keys;
        std::atomic_bool timedOut = false;
        auto stopWatchdog = std::make_shared<std::atomic_bool>(false);
        std::thread watchdog([abort, stopWatchdog, timedOut = &timedOut] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!stopWatchdog->load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!stopWatchdog->load()) {
                timedOut->store(true);
                abort->abort();
            }
        });

        try {
            const auto fixture = ReadFixture(request->fixture);
            const auto info = InspectFixture(request->fixture, *abort);
            result.fixture_bytes = fixture.size();
            result.codec = info.codec;
            result.sample_rate = info.sample_rate;
            result.bit_depth = info.bit_depth;
            result.channels = info.channels;
            result.duration_seconds = info.duration_seconds;
            result.key_invariants = KeyInvariants(info, fixture.size());
            result.prefetch_scope = PrefetchScopeInvariant();

            const std::wstring scope = Wide(GetStore().Scope(std::string(kProvider)));
            auto recordCase = [&](std::wstring name, auto&& run) {
                try {
                    result.cases.push_back(run());
                } catch (const exception_aborted&) {
                    throw;
                } catch (...) {
                    result.cases.push_back(Failed(std::move(name), L"unexpected_exception"));
                }
            };

            recordCase(L"progressive_first_decode", [&] {
                return ProgressiveAndCacheHit(scope, request->run_id, fixture, info, *abort, keys);
            });
            const bool progressiveOk = !result.cases.empty() && result.cases.back().ok;
            recordCase(L"cache_hit", [&] {
                if (!progressiveOk || keys.empty())
                    return Failed(L"cache_hit", L"progressive_dependency_failed");
                return CacheHitSeekable(keys.front(), fixture, info, *abort);
            });
            recordCase(L"concurrent_readers", [&] {
                return ConcurrentReaders(scope, request->run_id, fixture, info, *abort, keys);
            });
            recordCase(L"concurrent_reader_io",
                       [&] { return ConcurrentReaderIo(scope, request->run_id, info, *abort, keys); });
            recordCase(L"invalidate_during_produce", [&] {
                return InvalidateDuringProduce(scope, request->run_id, fixture, info, *abort, keys);
            });
            recordCase(L"scope_rotation", [&] {
                return RotatedScopeRejectsPublish(request->run_id, fixture, info, *abort, keys);
            });
            const std::wstring cleanupScope = Wide(GetStore().Scope(std::string(kProvider)));
            recordCase(L"staging_last_release", [&] {
                return LastReleaseCleansStaging(cleanupScope, request->run_id, fixture, info, *abort, keys);
            });

        } catch (const exception_aborted&) {
            result.error = timedOut.load() ? L"probe_timeout" : L"cancelled";
        } catch (...) {
            result.error = L"unexpected_failure";
        }
        for (const auto& key : keys) {
            try {
                Invalidate(key);
                GetStore().Erase(key);
            } catch (...) {
                ++result.cleanup_errors;
            }
        }
        if (!abort->is_aborting() && !WaitNoFlights(*abort)) {
            result.cleanup_errors++;
            result.error = L"cleanup_flight_timeout";
        }
        stopWatchdog->store(true);
        watchdog.join();
    }

    result.ok =
        result.error.empty() && result.key_invariants && result.prefetch_scope &&
        result.cleanup_errors == 0 && result.cases.size() == 7 &&
        std::all_of(result.cases.begin(), result.cases.end(), [](const CaseResult& item) { return item.ok; });
    try {
        WriteResult(paths.result, result);
    } catch (...) {
    }
    try {
        winrt::uninit_apartment();
    } catch (...) {
    }
}

class AudioProbeLifecycle : public initquit {
  public:
    ~AudioProbeLifecycle() { Stop(); }

    void on_init() override {
        const auto paths = ProbeFiles();
        if (!paths) return;
        std::error_code error;
        if (!fs::is_regular_file(paths->request, error) || error) return;

        DeleteFileW(paths->result.c_str());
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;

        auto abort = std::make_shared<abort_callback_impl>();
        std::lock_guard lock(m_mutex);
        if (m_worker.joinable()) {
            if (m_abort) m_abort->abort();
            m_worker.join();
        }
        m_abort = abort;
        m_worker = std::thread([paths = *paths, abort = std::move(abort)] { RunProbe(paths, abort); });
    }

    void on_quit() override { Stop(); }

  private:
    void Stop() noexcept {
        std::lock_guard lock(m_mutex);
        if (m_abort) m_abort->abort();
        if (m_worker.joinable()) m_worker.join();
        m_abort.reset();
    }

    std::mutex m_mutex;
    std::shared_ptr<abort_callback_impl> m_abort;
    std::thread m_worker;
};

FB2K_SERVICE_FACTORY(AudioProbeLifecycle);

} // namespace
} // namespace glasswave::cache::audio::probe
