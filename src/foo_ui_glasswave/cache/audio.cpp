#include "pch.h"
#include "cache/audio.h"

#include <atomic>
#include <filesystem>
#include <limits>
#include <unordered_map>

namespace glasswave::cache::audio {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kMaximumAudioBytes = 8ull * 1024 * 1024 * 1024;
constexpr auto kWaitSlice = std::chrono::milliseconds(20);

thread_local unsigned g_prefetchDepth = 0;

class WinHandle {
  public:
    WinHandle() = default;
    explicit WinHandle(HANDLE value) : m_value(value) {}
    ~WinHandle() { reset(); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : m_value(other.release()) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_value = other.release();
        }
        return *this;
    }

    HANDLE get() const noexcept { return m_value; }
    explicit operator bool() const noexcept { return m_value != INVALID_HANDLE_VALUE; }

    HANDLE release() noexcept {
        const HANDLE value = m_value;
        m_value = INVALID_HANDLE_VALUE;
        return value;
    }

    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value);
        m_value = value;
    }

  private:
    HANDLE m_value = INVALID_HANDLE_VALUE;
};

WinHandle OpenPath(const fs::path& path, DWORD access, DWORD disposition, DWORD flags) {
    const HANDLE value =
        CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    disposition, FILE_ATTRIBUTE_NORMAL | flags, nullptr);
    if (value == INVALID_HANDLE_VALUE) throw exception_io_data("Cannot open audio cache staging payload");
    return WinHandle(value);
}

WinHandle Duplicate(HANDLE source) {
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        throw exception_io_data("Cannot duplicate the audio cache staging handle");
    }
    return WinHandle(duplicate);
}

WinHandle CreateWaitEvent() {
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) throw exception_io_data("Cannot create the audio cache read event");
    return WinHandle(event);
}

void Truncate(HANDLE file) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(file))
        throw exception_io_data("Cannot prepare the audio cache staging payload");
}

size_t ReadSome(HANDLE file, uint64_t offset, void* bytes, size_t size, abort_callback& abort) {
    abort.check();
    if (!size) return 0;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
        throw exception_io_data("Cannot seek the progressive audio cache");
    const auto count =
        static_cast<DWORD>(std::min<size_t>(size, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    DWORD read = 0;
    if (!ReadFile(file, bytes, count, &read, nullptr))
        throw exception_io_data("Cannot read the progressive audio cache");
    return read;
}

size_t ReadSomeAt(HANDLE file, HANDLE event, uint64_t offset, void* bytes, size_t size,
                  abort_callback& abort) {
    abort.check();
    if (!size) return 0;

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    OVERLAPPED overlapped{};
    overlapped.Offset = position.LowPart;
    overlapped.OffsetHigh = position.HighPart;
    overlapped.hEvent = event;
    ResetEvent(event);

    const auto count =
        static_cast<DWORD>(std::min<size_t>(size, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    DWORD read = 0;
    SetLastError(ERROR_SUCCESS);
    if (ReadFile(file, bytes, count, &read, &overlapped)) return read;

    const DWORD immediate = GetLastError();
    if (immediate == ERROR_HANDLE_EOF || immediate == ERROR_BROKEN_PIPE) return 0;
    if (immediate != ERROR_IO_PENDING) exception_io_from_win32(immediate);

    try {
        while (!abort.waitForEvent(event, std::chrono::duration<double>(kWaitSlice).count())) {
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        CancelIoEx(file, &overlapped);
        GetOverlappedResult(file, &overlapped, &read, TRUE);
        std::rethrow_exception(failure);
    }
    abort.check();
    if (!GetOverlappedResult(file, &overlapped, &read, FALSE)) {
        const DWORD code = GetLastError();
        if (code == ERROR_HANDLE_EOF || code == ERROR_BROKEN_PIPE) return 0;
        exception_io_from_win32(code);
    }
    return read;
}

void WriteAt(HANDLE file, uint64_t offset, const void* bytes, size_t size, abort_callback& abort) {
    const auto* cursor = static_cast<const uint8_t*>(bytes);
    while (size) {
        abort.check();
        const auto count = static_cast<DWORD>(
            std::min<size_t>(size, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
            throw exception_io_data("Cannot seek the audio cache staging payload");
        DWORD written = 0;
        if (!WriteFile(file, cursor, count, &written, nullptr) || !written)
            throw exception_io_data("Cannot write the audio cache staging payload");
        cursor += written;
        offset += written;
        size -= written;
    }
}

fs::path MakeTemporaryPath() {
    std::vector<wchar_t> directory(MAX_PATH + 1);
    const DWORD length = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
    if (!length || static_cast<size_t>(length) >= directory.size())
        throw exception_io_data("Cannot locate the temporary audio cache directory");
    std::vector<wchar_t> name(MAX_PATH + 1);
    if (!GetTempFileNameW(directory.data(), L"gwa", 0, name.data()))
        throw exception_io_data("Cannot create a temporary audio cache payload");
    return fs::path(name.data());
}

void DeletePath(const fs::path& path) noexcept {
    if (!path.empty()) DeleteFileW(path.c_str());
}

void AppendPart(std::string& output, std::string_view value) {
    output += std::to_string(value.size());
    output.push_back(':');
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    output.push_back(';');
}

class StoredFile : public file {
  public:
    StoredFile(Snapshot snapshot, WinHandle handle)
        : m_snapshot(std::move(snapshot)), m_handle(std::move(handle)),
          m_contentType(m_snapshot.Info().contentType) {}

    t_size read(void* buffer, t_size bytes, abort_callback& abort) override {
        std::lock_guard ioLock(m_io);
        abort.check();
        if (m_position >= m_snapshot.Size()) return 0;
        const auto wanted = static_cast<size_t>(std::min<uint64_t>(bytes, m_snapshot.Size() - m_position));
        const size_t read = ReadSome(m_handle.get(), m_position, buffer, wanted, abort);
        if (read != wanted) throw exception_io_data("Stored audio payload ended before its declared size");
        m_position += read;
        return read;
    }

    void write(const void*, t_size, abort_callback&) override { throw exception_io_denied(); }
    void resize(t_filesize, abort_callback&) override { throw exception_io_denied(); }
    t_filesize get_size(abort_callback& abort) override {
        abort.check();
        return m_snapshot.Size();
    }
    t_filesize get_position(abort_callback&) override {
        std::lock_guard ioLock(m_io);
        return m_position;
    }
    void seek(t_filesize position, abort_callback& abort) override {
        std::lock_guard ioLock(m_io);
        abort.check();
        if (position > m_snapshot.Size()) throw exception_io_seek_out_of_range();
        m_position = position;
    }
    bool can_seek() override { return true; }
    bool get_content_type(pfc::string_base& out) override {
        out = m_contentType.c_str();
        return true;
    }
    void reopen(abort_callback& abort) override { seek(0, abort); }
    bool is_remote() override { return false; }
    void on_idle(abort_callback& abort) override { abort.check(); }
    t_filetimestamp get_timestamp(abort_callback&) override { return filetimestamp_invalid; }

  private:
    Snapshot m_snapshot;
    WinHandle m_handle;
    std::string m_contentType;
    std::mutex m_io;
    uint64_t m_position = 0;
};

Workload ExplicitWorkload(Workload workload) {
    if (workload != Workload::Inherit) return workload;
    return g_prefetchDepth ? Workload::Prefetch : Workload::Playback;
}

template <typename Function> void NoThrow(Function&& function) noexcept {
    try {
        function();
    } catch (...) {
    }
}

} // namespace

PrefetchScope::PrefetchScope() {
    ++g_prefetchDepth;
}
PrefetchScope::~PrefetchScope() {
    if (g_prefetchDepth) --g_prefetchDepth;
}
bool IsPrefetchThread() noexcept {
    return g_prefetchDepth != 0;
}

Key MakeKey(Source source) {
    std::string variant;
    AppendPart(variant, kProducerSalt);
    AppendPart(variant, source.quality);
    AppendPart(variant, source.codec);
    AppendPart(variant, source.sourceVersion);
    return Key{Kind::Audio, std::move(source.provider), std::move(source.accountScope),
               std::move(source.identity), std::move(variant)};
}

struct Flight::Impl {
    class Sink final : public audio::Sink {
      public:
        explicit Sink(Impl& owner) : m_owner(owner) {}

        void Write(const void* bytes, size_t size, abort_callback& abort) override {
            m_owner.Write(bytes, size, abort);
        }

        uint64_t Size() const noexcept override { return m_owner.Available(); }

      private:
        Impl& m_owner;
    };

    Impl(Key sourceKey, Metadata sourceMetadata, Fetcher sourceFetch, Workload sourceWorkload,
         std::unique_ptr<Writer> sourceWriter, fs::path fallback, WinHandle write, WinHandle read)
        : key(std::move(sourceKey)), metadata(std::move(sourceMetadata)), fetch(std::move(sourceFetch)),
          workload(sourceWorkload), writer(std::move(sourceWriter)), fallbackPath(std::move(fallback)),
          writeHandle(std::move(write)), readHandle(std::move(read)) {}

    ~Impl() {
        Cancel();
        writeHandle.reset();
        readHandle.reset();
        writer.reset();
        DeletePath(fallbackPath);
    }

    void Acquire() {
        {
            std::lock_guard lock(mutex);
            ++participants;
        }
        changed.notify_all();
    }

    void Release() noexcept {
        bool cancel = false;
        {
            std::lock_guard lock(mutex);
            if (participants > 0) --participants;
            cancel = participants == 0 && !done;
        }
        if (cancel) Cancel();
    }

    void Cancel() noexcept {
        std::lock_guard publishLock(commitMutex);
        Fetcher abandoned;
        {
            std::lock_guard lock(mutex);
            cancelled = true;
            if (!fetchTaken) {
                abandoned = std::move(fetch);
                fetchTaken = true;
            }
        }
        abort.abort();
        changed.notify_all();
    }

    void Invalidate() noexcept { Cancel(); }

    bool CanAdopt() const {
        std::lock_guard lock(mutex);
        return !cancelled && (!done || !error);
    }

    Workload Promote() { return workload.exchange(Workload::Playback); }

    Workload CurrentWorkload() const noexcept { return workload.load(); }

    bool Done() const noexcept {
        std::lock_guard lock(mutex);
        return done;
    }

    bool Failed() const noexcept {
        std::lock_guard lock(mutex);
        return cancelled || static_cast<bool>(error);
    }

    uint64_t Available() const noexcept {
        std::lock_guard lock(mutex);
        return available;
    }

    void Write(const void* bytes, size_t size, abort_callback& readerAbort) {
        readerAbort.check();
        {
            std::lock_guard lock(mutex);
            if (cancelled) throw exception_aborted();
            if (available + size > kMaximumAudioBytes)
                throw exception_io_data("Encoded audio exceeds the shared cache limit");
        }
        WriteAt(writeHandle.get(), Available(), bytes, size, readerAbort);
        {
            std::lock_guard lock(mutex);
            available += size;
        }
        changed.notify_all();
    }

    Fetcher BeginFetch() {
        std::lock_guard lock(mutex);
        if (cancelled) throw exception_aborted();
        if (fetchTaken) throw exception_io_data("Audio cache transfer started twice");
        fetchTaken = true;
        return std::move(fetch);
    }

    void Run() noexcept {
        try {
            abort.check();
            Fetcher producer = BeginFetch();
            if (!producer) throw exception_io_data("Audio cache transfer has no producer");
            Sink sink(*this);
            producer(sink, abort);
            abort.check();
            Complete();
        } catch (...) {
            Fail(std::current_exception());
        }
    }

    void Complete() {
        std::lock_guard publishLock(commitMutex);
        {
            std::lock_guard lock(mutex);
            if (cancelled) throw exception_aborted();
        }
        if (!Available()) throw exception_io_data("Audio source returned no encoded bytes");
        NoThrow([this] { FlushFileBuffers(writeHandle.get()); });
        writeHandle.reset();

        std::optional<Snapshot> published;
        if (writer) {
            try {
                if (!metadata.validatedAt) metadata.validatedAt = Store::Now();
                published = writer->Commit(metadata);
            } catch (...) {
                // Cache publication is optional. The open staging handle remains usable.
            }
            writer.reset();
        }

        {
            std::lock_guard lock(mutex);
            snapshot = std::move(published);
            done = true;
            error = {};
        }
        changed.notify_all();
    }

    void Fail(std::exception_ptr failure) noexcept {
        std::lock_guard publishLock(commitMutex);
        writeHandle.reset();
        writer.reset();
        {
            std::lock_guard lock(mutex);
            error = std::move(failure);
            done = true;
        }
        changed.notify_all();
    }

    Key key;
    Metadata metadata;
    Fetcher fetch;
    std::atomic<Workload> workload;
    std::unique_ptr<Writer> writer;
    fs::path fallbackPath;
    WinHandle writeHandle;
    WinHandle readHandle;
    abort_callback_impl abort;
    std::mutex commitMutex;
    mutable std::mutex mutex;
    std::condition_variable changed;
    uint64_t available = 0;
    int participants = 0;
    bool done = false;
    bool cancelled = false;
    bool fetchTaken = false;
    std::exception_ptr error;
    std::optional<Snapshot> snapshot;
};

class Flight::View : public file {
  public:
    View(std::shared_ptr<Impl> owner, bool seekable)
        : m_owner(std::move(owner)), m_handle(Duplicate(m_owner->readHandle.get())),
          m_event(CreateWaitEvent()), m_seekable(seekable) {
        m_owner->Acquire();
        m_contentType = m_owner->metadata.contentType;
    }

    ~View() { m_owner->Release(); }

    t_size read(void* buffer, t_size bytes, abort_callback& abort) override {
        std::lock_guard ioLock(m_io);
        t_size total = 0;
        while (total < bytes) {
            uint64_t available = 0;
            bool done = false;
            bool cancelled = false;
            bool wait = false;
            std::exception_ptr error;
            {
                std::unique_lock lock(m_owner->mutex);
                abort.check();
                cancelled = m_owner->cancelled;
                available = m_owner->available;
                done = m_owner->done;
                error = m_owner->error;
                if (!cancelled && m_position < available) {
                    const auto count =
                        static_cast<size_t>(std::min<uint64_t>(bytes - total, available - m_position));
                    lock.unlock();
                    const size_t read = ReadSomeAt(m_handle.get(), m_event.get(), m_position,
                                                   static_cast<uint8_t*>(buffer) + total, count, abort);
                    if (!read) throw exception_io_data("Audio cache read ended unexpectedly");
                    m_position += read;
                    total += read;
                    continue;
                }
                wait = !cancelled && !done;
                if (wait) m_owner->changed.wait_for(lock, kWaitSlice);
            }
            if (wait) continue;
            if (cancelled) throw exception_aborted();
            if (error) std::rethrow_exception(error);
            if (done) break;
        }
        return total;
    }

    void write(const void*, t_size, abort_callback&) override { throw exception_io_denied(); }
    void resize(t_filesize, abort_callback&) override { throw exception_io_denied(); }

    t_filesize get_size(abort_callback& abort) override {
        abort.check();
        std::lock_guard lock(m_owner->mutex);
        if (m_owner->cancelled) throw exception_aborted();
        if (m_owner->error) std::rethrow_exception(m_owner->error);
        return m_seekable && m_owner->done ? m_owner->available : filesize_invalid;
    }

    t_filesize get_position(abort_callback&) override {
        std::lock_guard ioLock(m_io);
        return m_position;
    }

    void seek(t_filesize position, abort_callback& abort) override {
        std::lock_guard ioLock(m_io);
        std::unique_lock lock(m_owner->mutex);
        while (position > m_owner->available && !m_owner->done && !m_owner->cancelled) {
            abort.check();
            m_owner->changed.wait_for(lock, kWaitSlice);
        }
        abort.check();
        if (m_owner->cancelled) throw exception_aborted();
        if (m_owner->error) std::rethrow_exception(m_owner->error);
        if (position > m_owner->available) throw exception_io_seek_out_of_range();
        m_position = position;
    }

    bool can_seek() override { return m_seekable; }

    bool get_content_type(pfc::string_base& out) override {
        out = m_contentType.c_str();
        return true;
    }

    void reopen(abort_callback& abort) override { seek(0, abort); }
    bool is_remote() override { return !m_seekable; }
    void on_idle(abort_callback& abort) override { abort.check(); }
    t_filetimestamp get_timestamp(abort_callback&) override { return filetimestamp_invalid; }

  private:
    std::shared_ptr<Impl> m_owner;
    WinHandle m_handle;
    WinHandle m_event;
    std::string m_contentType;
    std::mutex m_io;
    uint64_t m_position = 0;
    bool m_seekable;
};

class Manager {
  public:
    Manager() {
        for (size_t index = 0; index < kMaxWorkers; ++index)
            workers.emplace_back([this] { Worker(); });
    }

    ~Manager() { Stop(); }

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    Opened Open(Request request, abort_callback& abort) {
        abort.check();
        const Workload requested = ExplicitWorkload(request.workload);
        const std::string digest = Store::Digest(request.key);

        std::unique_lock lock(mutex);
        if (stopping) throw exception_aborted();

        const auto adoptLocked = [&]() -> std::optional<Opened> {
            if (const auto existing = FindLocked(digest)) {
                if (existing->CanAdopt()) {
                    if (requested == Workload::Playback && existing->Promote() == Workload::Prefetch) {
                        if (prefetchFlights) --prefetchFlights;
                    }
                    auto flight = std::shared_ptr<Flight>(new Flight(existing));
                    return Opened{flight->Sequential(), std::move(flight), false, true};
                }
                EraseLocked(digest);
            }
            return {};
        };
        if (auto opened = adoptLocked()) return std::move(*opened);

        // Payload verification can read a large lossless song. Keep unrelated playback,
        // promotion and cancellation free to use the scheduler while disk IO runs.
        lock.unlock();
        std::optional<Opened> cached;
        if (request.persistent) {
            if (auto snapshot = Lookup(request.key, request.metadata.contentType)) {
                try {
                    const fs::path path = snapshot->Path();
                    auto handle = OpenPath(path, GENERIC_READ, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS);
                    auto stream = fb2k::service_new<StoredFile>(std::move(*snapshot), std::move(handle));
                    cached = Opened{std::move(stream), {}, true, true};
                } catch (...) {
                    NoThrow([&] { GetStore().Erase(request.key); });
                }
            }
        }

        abort.check();
        lock.lock();
        if (stopping) throw exception_aborted();
        // Another opener may have installed the shared flight while this lookup ran.
        if (auto opened = adoptLocked()) return std::move(*opened);
        if (cached) return std::move(*cached);

        if (requested == Workload::Prefetch && prefetchFlights >= kMaxPrefetchFlights)
            return Opened{{}, {}, false, false};

        std::unique_ptr<Writer> writer;
        fs::path fallback;
        WinHandle write;
        WinHandle read;
        std::exception_ptr storeFailure;
        if (request.persistent) {
            try {
                writer = GetStore().Begin(request.key);
                if (writer) {
                    write = OpenPath(writer->Path(), GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS,
                                     FILE_FLAG_SEQUENTIAL_SCAN);
                    Truncate(write.get());
                    read = OpenPath(writer->Path(), GENERIC_READ, OPEN_EXISTING,
                                    FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED);
                }
            } catch (...) {
                storeFailure = std::current_exception();
                writer.reset();
                write.reset();
                read.reset();
            }
        }

        if (!read) {
            fallback = MakeTemporaryPath();
            try {
                write = OpenPath(fallback, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING,
                                 FILE_FLAG_SEQUENTIAL_SCAN);
                Truncate(write.get());
                read = OpenPath(fallback, GENERIC_READ, OPEN_EXISTING,
                                FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED);
            } catch (...) {
                DeletePath(fallback);
                if (storeFailure) std::rethrow_exception(storeFailure);
                throw;
            }
        }

        auto state = std::make_shared<Flight::Impl>(request.key, std::move(request.metadata),
                                                    std::move(request.fetch), requested, std::move(writer),
                                                    std::move(fallback), std::move(write), std::move(read));
        auto flight = std::shared_ptr<Flight>(new Flight(state));
        file::ptr stream;
        try {
            stream = fb2k::service_new<Flight::View>(state, false);
        } catch (...) {
            state->Cancel();
            throw;
        }

        flights[digest] = state;
        if (requested == Workload::Prefetch) ++prefetchFlights;
        queue.push_back(std::move(state));
        changed.notify_all();
        return Opened{std::move(stream), std::move(flight), false, true};
    }

    std::shared_ptr<Flight> Adopt(const Key& key, Workload requested) {
        const std::string digest = Store::Digest(key);
        std::lock_guard lock(mutex);
        if (stopping) return {};
        auto existing = FindLocked(digest);
        if (!existing || !existing->CanAdopt()) return {};
        if (requested == Workload::Playback && existing->Promote() == Workload::Prefetch) {
            if (prefetchFlights) --prefetchFlights;
        }
        return std::shared_ptr<Flight>(new Flight(existing));
    }

    void Promote(const std::shared_ptr<Flight::Impl>& state) {
        std::lock_guard lock(mutex);
        const auto found = flights.find(Store::Digest(state->key));
        const bool tracked = found != flights.end() && found->second.lock() == state;
        if (state->Promote() == Workload::Prefetch && tracked && prefetchFlights) --prefetchFlights;
    }

    void Invalidate(const Key& key) noexcept {
        std::shared_ptr<Flight::Impl> active;
        {
            std::lock_guard lock(mutex);
            active = FindLocked(Store::Digest(key));
        }
        if (active) active->Cancel();
        NoThrow([&] { GetStore().Erase(key); });
    }

    size_t Active() const {
        std::lock_guard lock(mutex);
        return flights.size();
    }

    size_t ActivePrefetch() const {
        std::lock_guard lock(mutex);
        return prefetchFlights;
    }

    void Stop() noexcept {
        std::deque<std::shared_ptr<Flight::Impl>> dropped;
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            stopping = true;
            for (auto& entry : flights)
                if (auto active = entry.second.lock()) active->Cancel();
            dropped.swap(queue);
        }
        changed.notify_all();
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
        workers.clear();
        {
            std::lock_guard lock(mutex);
            flights.clear();
            prefetchFlights = 0;
            activePrefetch = 0;
        }
    }

  private:
    std::optional<Snapshot> Lookup(const Key& key, std::string_view expectedType) {
        try {
            auto snapshot = GetStore().Lookup(key);
            if (!snapshot) return {};
            if (!expectedType.empty() && !snapshot->Info().contentType.empty() &&
                snapshot->Info().contentType != expectedType) {
                NoThrow([&] { GetStore().Erase(key); });
                return {};
            }
            return snapshot;
        } catch (...) {
            return {};
        }
    }

    std::shared_ptr<Flight::Impl> FindLocked(const std::string& digest) const {
        const auto found = flights.find(digest);
        return found == flights.end() ? std::shared_ptr<Flight::Impl>() : found->second.lock();
    }

    void EraseLocked(const std::string& digest) {
        const auto found = flights.find(digest);
        if (found == flights.end()) return;
        if (auto active = found->second.lock();
            active && active->CurrentWorkload() == Workload::Prefetch && prefetchFlights) {
            --prefetchFlights;
        }
        flights.erase(found);
    }

    bool HasPlaybackLocked() const {
        return std::any_of(queue.begin(), queue.end(),
                           [](const auto& state) { return state->CurrentWorkload() == Workload::Playback; });
    }

    bool HasPrefetchLocked() const {
        return activePrefetch < kMaxPrefetchFlights &&
               std::any_of(
                   queue.begin(), queue.end(),
                   [](const auto& state) { return state->CurrentWorkload() == Workload::Prefetch; });
    }

    std::shared_ptr<Flight::Impl> TakeNextLocked(bool& prefetch) {
        auto selected = std::find_if(queue.begin(), queue.end(), [](const auto& state) {
            return state->CurrentWorkload() == Workload::Playback;
        });
        if (selected == queue.end()) {
            selected = std::find_if(queue.begin(), queue.end(), [](const auto& state) {
                return state->CurrentWorkload() == Workload::Prefetch;
            });
            prefetch = true;
        }
        auto state = std::move(*selected);
        queue.erase(selected);
        return state;
    }

    void Finish(const std::shared_ptr<Flight::Impl>& state) {
        std::lock_guard lock(mutex);
        if (state->CurrentWorkload() == Workload::Prefetch && prefetchFlights) --prefetchFlights;
        const std::string digest = Store::Digest(state->key);
        const auto found = flights.find(digest);
        if (found != flights.end() && found->second.lock() == state) flights.erase(found);
        changed.notify_all();
    }

    void Worker() {
        for (;;) {
            std::shared_ptr<Flight::Impl> state;
            bool scheduledPrefetch = false;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopping || HasPlaybackLocked() || HasPrefetchLocked(); });
                if (stopping) return;
                state = TakeNextLocked(scheduledPrefetch);
                if (scheduledPrefetch) ++activePrefetch;
            }

            state->Run();

            {
                std::lock_guard lock(mutex);
                if (scheduledPrefetch && activePrefetch) --activePrefetch;
            }
            Finish(state);
            changed.notify_all();
        }
    }

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Flight::Impl>> queue;
    std::unordered_map<std::string, std::weak_ptr<Flight::Impl>> flights;
    std::vector<std::thread> workers;
    size_t prefetchFlights = 0;
    size_t activePrefetch = 0;
    bool stopping = false;
};

std::mutex g_managerMutex;
std::unique_ptr<Manager> g_manager;
bool g_managerStopping = false;

Manager& SharedManager() {
    std::lock_guard lock(g_managerMutex);
    if (g_managerStopping) throw exception_aborted();
    if (!g_manager) g_manager = std::make_unique<Manager>();
    return *g_manager;
}

Manager* ExistingManager() {
    std::lock_guard lock(g_managerMutex);
    return g_manager.get();
}

Flight::Flight(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {
    m_impl->Acquire();
}

Flight::~Flight() {
    m_impl->Release();
}

file::ptr Flight::Sequential() const {
    std::optional<Snapshot> snapshot;
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->cancelled) throw exception_aborted();
        if (m_impl->error) std::rethrow_exception(m_impl->error);
        snapshot = m_impl->snapshot;
    }
    if (snapshot) {
        const fs::path path = snapshot->Path();
        auto handle = OpenPath(path, GENERIC_READ, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS);
        return fb2k::service_new<StoredFile>(std::move(*snapshot), std::move(handle));
    }
    return fb2k::service_new<View>(m_impl, false);
}

file::ptr Flight::Completed(abort_callback& abort) const {
    std::optional<Snapshot> snapshot;
    {
        std::unique_lock lock(m_impl->mutex);
        while (!m_impl->done && !m_impl->cancelled) {
            abort.check();
            m_impl->changed.wait_for(lock, kWaitSlice);
        }
        abort.check();
        if (m_impl->cancelled) throw exception_aborted();
        if (m_impl->error) std::rethrow_exception(m_impl->error);
        snapshot = m_impl->snapshot;
    }
    if (snapshot) {
        const fs::path path = snapshot->Path();
        auto handle = OpenPath(path, GENERIC_READ, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS);
        return fb2k::service_new<StoredFile>(std::move(*snapshot), std::move(handle));
    }
    return fb2k::service_new<View>(m_impl, true);
}

bool Flight::Done() const noexcept {
    return m_impl->Done();
}
bool Flight::Failed() const noexcept {
    return m_impl->Failed();
}
uint64_t Flight::Available() const noexcept {
    return m_impl->Available();
}

void Flight::Promote() {
    SharedManager().Promote(m_impl);
}

void Flight::Invalidate() noexcept {
    if (Manager* manager = ExistingManager()) manager->Invalidate(m_impl->key);
    m_impl->Cancel();
}

Opened OpenOrStart(Request request, abort_callback& abort) {
    return SharedManager().Open(std::move(request), abort);
}

std::shared_ptr<Flight> Adopt(const Key& key, Workload workload) {
    return SharedManager().Adopt(key, ExplicitWorkload(workload));
}

void Invalidate(const Key& key) noexcept {
    if (Manager* manager = ExistingManager()) manager->Invalidate(key);
}

size_t ActiveFlights() {
    Manager* manager = ExistingManager();
    return manager ? manager->Active() : 0;
}
size_t ActivePrefetchFlights() {
    Manager* manager = ExistingManager();
    return manager ? manager->ActivePrefetch() : 0;
}
void Shutdown() {
    Manager* manager = nullptr;
    {
        std::lock_guard lock(g_managerMutex);
        g_managerStopping = true;
        manager = g_manager.get();
    }
    if (manager) manager->Stop();
}

} // namespace glasswave::cache::audio

namespace {

class AudioCacheLifecycle : public initquit {
  public:
    void on_init() override {}
    void on_quit() override { glasswave::cache::audio::Shutdown(); }
};

FB2K_SERVICE_FACTORY(AudioCacheLifecycle);

} // namespace
