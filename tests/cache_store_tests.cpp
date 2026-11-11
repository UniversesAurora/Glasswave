#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cache/store.h"
#include <winsqlite/winsqlite3.h>

namespace {

namespace fs = std::filesystem;
using glasswave::cache::Key;
using glasswave::cache::Kind;
using glasswave::cache::Metadata;
using glasswave::cache::Snapshot;
using glasswave::cache::Store;

std::atomic_size_t checkCount = 0;

class TestFailure final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

void Check(bool condition, std::string_view message) {
    ++checkCount;
    if (condition) return;
    throw TestFailure(std::string(message));
}

[[noreturn]] void Fail(std::string_view message) {
    throw TestFailure(std::string(message));
}

fs::path Normalized(const fs::path& path) {
    std::error_code error;
    auto absolute = fs::absolute(path, error);
    if (error) Fail("Unable to make a test path absolute");
    auto canonical = fs::weakly_canonical(absolute, error);
    if (error) Fail("Unable to resolve a test path");
    return canonical.lexically_normal();
}

bool IsWithinOrEqual(const fs::path& base, const fs::path& candidate) {
    const auto relative = Normalized(candidate).lexically_relative(Normalized(base));
    if (relative.empty()) return false;
    if (relative == fs::path(L".")) return true;
    const auto first = relative.begin();
    return first != relative.end() && *first != fs::path(L"..") && *first != fs::path(L".");
}

fs::path ValidateRootArgument(const fs::path& argument, const fs::path& expectedBase) {
    const auto base = Normalized(expectedBase);
    const auto root = Normalized(argument);
    Check(root.parent_path() == base, "Cache test root must be a direct child of .local/tests");
    const auto name = root.filename().wstring();
    constexpr std::wstring_view prefix = L"cache-store-";
    Check(name.size() > prefix.size() && name.starts_with(prefix),
          "Cache test root must use the cache-store- prefix");
    return root;
}

void SafeRemove(const fs::path& intendedRoot, const fs::path& target, bool allowEqual) {
    const auto root = Normalized(intendedRoot);
    const auto path = Normalized(target);
    Check(IsWithinOrEqual(root, path), "Refusing recursive removal outside the cache test root");
    if (!allowEqual) Check(path != root, "Refusing to remove the cache test root as a child");
    if (!fs::exists(path)) return;

    std::error_code error;
    fs::remove_all(path, error);
    if (error) Fail("Unable to remove a cache test directory");
}

std::string ExceptionMessage(std::exception_ptr failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Unknown test failure";
    }
}

template <typename Function>
std::string RunCase(const fs::path& root, std::string_view name, Function&& function) {
    const auto directory = Normalized(root / name);
    if (!IsWithinOrEqual(root, directory)) return "Cache test case escaped its root";
    std::string failure;
    try {
        fs::create_directories(directory);
        function(directory);
    } catch (...) {
        failure = ExceptionMessage(std::current_exception());
    }
    try {
        SafeRemove(root, directory, false);
    } catch (...) {
        const auto cleanup = ExceptionMessage(std::current_exception());
        if (!failure.empty()) failure += "\n";
        failure += "Cleanup failed: " + cleanup;
    }
    return failure;
}

Key MakeKey(Store& store, std::string provider, std::string identity, std::string variant = "v1",
            Kind kind = Kind::Data) {
    auto scope = store.Scope(provider);
    return Key{kind, std::move(provider), std::move(scope), std::move(identity), std::move(variant)};
}

Metadata MakeMetadata(std::string tag, int64_t validatedAt = 1'760'000'000) {
    Metadata metadata;
    metadata.contentType = "application/octet-stream";
    metadata.etag = "\"etag-" + tag + "\"";
    metadata.lastModified = "Tue, 10 Nov 2026 00:00:00 GMT";
    metadata.extra = "schema=1;tag=" + tag;
    metadata.validatedAt = validatedAt;
    return metadata;
}

bool SameMetadata(const Metadata& left, const Metadata& right) {
    return left.contentType == right.contentType && left.etag == right.etag &&
           left.lastModified == right.lastModified && left.extra == right.extra &&
           left.validatedAt == right.validatedAt;
}

std::string Payload(std::string_view value, std::size_t size) {
    std::string result(size, ' ');
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = value[i % value.size()];
    return result;
}

void WriteBytes(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) Fail("Unable to open a writer payload");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Check(output.good(), "Unable to close a writer payload");
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) Fail("Unable to read a cached payload");
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    Check(end >= 0, "Unable to measure a cached payload");
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(end), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(input.gcount() == static_cast<std::streamsize>(bytes.size()),
          "Cached payload ended before its declared size");
    return bytes;
}

void ExecuteDatabaseSql(const fs::path& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open16(path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        Fail("Unable to open a cache index for test setup");
    }
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    std::string failure = message ? message : "";
    sqlite3_free(message);
    sqlite3_close(db);
    if (result != SQLITE_OK) Fail("Unable to modify a cache index for test setup: " + failure);
}

std::optional<Snapshot> CommitPayload(Store& store, const Key& key, std::string_view bytes,
                                      const Metadata& metadata, std::string_view label) {
    auto writer = store.Begin(key);
    Check(writer != nullptr, std::string(label) + " did not begin a writer");
    Check(fs::exists(writer->Path()), std::string(label) + " did not create an empty payload file");
    Check(fs::file_size(writer->Path()) == 0, std::string(label) + " created a non-empty writer payload");

    WriteBytes(writer->Path(), bytes);
    auto committed = writer->Commit(metadata);
    Check(committed.has_value(), std::string(label) + " did not commit");
    Check(committed->Size() == bytes.size(), std::string(label) + " reported the wrong size");
    Check(ReadBytes(committed->Path()) == bytes, std::string(label) + " returned the wrong bytes");
    Check(SameMetadata(committed->Info(), metadata), std::string(label) + " did not preserve metadata");
    return committed;
}

void CommitUnpinned(Store& store, const Key& key, std::string_view bytes, const Metadata& metadata,
                    std::string_view label) {
    auto committed = CommitPayload(store, key, bytes, metadata, label);
    Check(committed.has_value(), "Committed snapshot unexpectedly disappeared");
}

Snapshot RequireSnapshot(Store& store, const Key& key, std::string_view label) {
    auto snapshot = store.Lookup(key);
    Check(snapshot.has_value(), std::string(label) + " unexpectedly missed");
    return *snapshot;
}

void CheckSnapshot(const Snapshot& snapshot, std::string_view bytes, const Metadata& metadata,
                   std::string_view label) {
    Check(static_cast<bool>(snapshot), std::string(label) + " is not usable");
    Check(snapshot.Size() == bytes.size(), std::string(label) + " has the wrong size");
    Check(ReadBytes(snapshot.Path()) == bytes, std::string(label) + " has the wrong content");
    Check(SameMetadata(snapshot.Info(), metadata), std::string(label) + " lost metadata");
}

void ScopePersistenceAndRotation(const fs::path& root) {
    const std::string provider = "provider.scope";
    const auto metadata = MakeMetadata("scope");
    const std::string original = Payload("scope-original", 151);
    std::string oldScope;

    {
        Store store(root);
        auto key = MakeKey(store, provider, "identity-1");
        oldScope = key.scope;
        Check(!oldScope.empty(), "Scope returned an empty account generation");
        auto committed = CommitPayload(store, key, original, metadata, "initial scope write");
        CheckSnapshot(*committed, original, metadata, "initial scope write");
    }

    std::string newScope;
    {
        Store store(root);
        Check(store.Scope(provider) == oldScope, "Account scope did not survive restart");

        auto oldKey = MakeKey(store, provider, "identity-1");
        auto oldSnapshot = RequireSnapshot(store, oldKey, "persisted old-scope entry");
        CheckSnapshot(oldSnapshot, original, metadata, "persisted old-scope entry");

        auto staleWriter = store.Begin(oldKey);
        Check(staleWriter != nullptr, "Old-scope writer did not start");
        WriteBytes(staleWriter->Path(), Payload("stale-write", 173));

        store.RotateScope(provider);
        newScope = store.Scope(provider);
        Check(!newScope.empty() && newScope != oldScope, "Rotation did not issue a new account generation");
        Check(!staleWriter->Commit(MakeMetadata("stale")).has_value(),
              "Writer from the old account generation published after rotation");
        Check(!store.Lookup(oldKey).has_value(), "Old account generation remained visible after rotation");

        auto currentKey = MakeKey(store, provider, "identity-1");
        const auto currentMetadata = MakeMetadata("current");
        const auto current = Payload("current-scope", 137);
        CommitUnpinned(store, currentKey, current, currentMetadata, "rotated-scope write");
        auto currentSnapshot = RequireSnapshot(store, currentKey, "rotated-scope entry");
        CheckSnapshot(currentSnapshot, current, currentMetadata, "rotated-scope entry");
    }

    {
        Store store(root);
        Check(store.Scope(provider) == newScope, "Rotated scope did not survive restart");
        auto oldKey = MakeKey(store, provider, "identity-1");
        oldKey.scope = oldScope;
        Check(!store.Lookup(oldKey).has_value(), "Restart resurrected an old account generation");
    }
}

void StableKeyEncodingAndQuality(const fs::path& root) {
    const std::string provider = "provider.keys";
    const auto low = Payload("cover-600", 211);
    const auto high = Payload("cover-1200", 307);
    const auto lowMetadata = MakeMetadata("quality-600", 100);
    const auto highMetadata = MakeMetadata("quality-1200", 200);
    std::string baseDigest;

    {
        Store store(root);
        auto base = MakeKey(store, provider, "track|42\nselector=?encoded", "cover-600");
        auto same = base;
        auto quality = base;
        quality.variant = "cover-1200";
        auto kind = base;
        kind.kind = Kind::Image;
        auto delimiterSplit = base;
        delimiterSplit.identity = "track";
        delimiterSplit.variant = "42\nselector=?encoded|cover-600";
        auto identity = base;
        identity.identity = "track|43\nselector=?encoded";

        baseDigest = Store::Digest(base);
        Check(baseDigest == Store::Digest(same), "Identical keys have unstable digests");
        Check(baseDigest != Store::Digest(quality), "Source quality variants collided");
        Check(baseDigest != Store::Digest(kind), "Cache kinds collided");
        Check(baseDigest != Store::Digest(identity), "Stable identities collided");
        Check(baseDigest != Store::Digest(delimiterSplit), "Field delimiters caused a stable-key collision");

        CommitUnpinned(store, base, low, lowMetadata, "600-pixel quality write");
        CommitUnpinned(store, quality, high, highMetadata, "1200-pixel quality write");

        auto lowSnapshot = RequireSnapshot(store, base, "600-pixel quality entry");
        auto highSnapshot = RequireSnapshot(store, quality, "1200-pixel quality entry");
        CheckSnapshot(lowSnapshot, low, lowMetadata, "600-pixel quality entry");
        CheckSnapshot(highSnapshot, high, highMetadata, "1200-pixel quality entry");
        Check(lowSnapshot.Path() != highSnapshot.Path(), "Encoding variants shared one immutable payload");
    }

    {
        Store restarted(root);
        auto restartedKey = MakeKey(restarted, provider, "track|42\nselector=?encoded", "cover-600");
        Check(Store::Digest(restartedKey) == baseDigest, "Stable key digest changed across restart");
    }
}

void OldSnapshotSurvivesReplacement(const fs::path& root) {
    Store store(root);
    auto key = MakeKey(store, "provider.replace", "asset");
    const auto oldMetadata = MakeMetadata("old", 10);
    const auto newMetadata = MakeMetadata("new", 20);
    const auto oldBytes = Payload("old-generation", 257);
    const auto newBytes = Payload("new-generation", 389);

    auto oldSnapshot = CommitPayload(store, key, oldBytes, oldMetadata, "old generation");
    CommitUnpinned(store, key, newBytes, newMetadata, "new generation");

    auto current = RequireSnapshot(store, key, "replacement entry");
    CheckSnapshot(current, newBytes, newMetadata, "replacement entry");
    CheckSnapshot(*oldSnapshot, oldBytes, oldMetadata, "old pinned generation");
    Check(oldSnapshot->Path() != current.Path(), "Replacement overwrote the immutable old payload");
}

void WriteTicketsAndAbandonment(const fs::path& root) {
    const std::string provider = "provider.ticket";

    {
        Store store(root);
        auto key = MakeKey(store, provider, "asset");
        auto older = store.Begin(key);
        auto newer = store.Begin(key);
        Check(older != nullptr && newer != nullptr, "Collision writers did not start");
        const auto olderBytes = Payload("older-ticket", 131);
        const auto newerBytes = Payload("newer-ticket", 179);
        WriteBytes(older->Path(), olderBytes);
        WriteBytes(newer->Path(), newerBytes);

        const auto newerMetadata = MakeMetadata("newer");
        auto published = newer->Commit(newerMetadata);
        Check(published.has_value(), "Newest write ticket did not publish");
        Check(!older->Commit(MakeMetadata("older")).has_value(),
              "Superseded writer published after the newest ticket");

        auto visible = RequireSnapshot(store, key, "newest-ticket entry");
        CheckSnapshot(visible, newerBytes, newerMetadata, "newest-ticket entry");
    }

    {
        Store store(root);
        const auto baseline = store.CurrentUsage(Kind::Data);
        fs::path abandonedPath;
        {
            auto abandoned = store.Begin(MakeKey(store, provider, "abandoned"));
            Check(abandoned != nullptr, "Aborted writer did not start");
            abandonedPath = abandoned->Path();
            WriteBytes(abandonedPath, Payload("never-committed", 193));
        }
        const auto abandonedKey = MakeKey(store, provider, "abandoned");
        Check(!store.Lookup(abandonedKey).has_value(), "Writer destruction published an uncommitted payload");
        const auto after = store.CurrentUsage(Kind::Data);
        Check(after.entries == baseline.entries && after.bytes == baseline.bytes,
              "Aborted writer changed cache usage");
    }

    {
        Store store(root);
        auto abandonedKey = MakeKey(store, provider, "abandoned");
        Check(!store.Lookup(abandonedKey).has_value(), "Restart exposed an uncommitted payload");
    }
}

void RestartPersistence(const fs::path& root) {
    const auto audioMetadata = MakeMetadata("audio-restart", 300);
    const auto imageMetadata = MakeMetadata("image-restart", 400);
    const auto audioBytes = Payload("audio-persisted", 233);
    const auto imageBytes = Payload("image-persisted", 319);
    std::string scope;

    {
        Store store(root);
        auto audio = MakeKey(store, "provider.restart", "song", "flac-16-44", Kind::Audio);
        auto image = MakeKey(store, "provider.restart", "cover", "cover-1200", Kind::Image);
        scope = audio.scope;
        CommitUnpinned(store, audio, audioBytes, audioMetadata, "restart audio");
        CommitUnpinned(store, image, imageBytes, imageMetadata, "restart image");
        const auto audioUsage = store.CurrentUsage(Kind::Audio);
        const auto imageUsage = store.CurrentUsage(Kind::Image);
        Check(audioUsage.bytes == audioBytes.size() && audioUsage.entries == 1,
              "Audio usage did not reflect the committed payload");
        Check(imageUsage.bytes == imageBytes.size() && imageUsage.entries == 1,
              "Image usage did not reflect the committed payload");
    }

    {
        Store store(root);
        Check(store.Scope("provider.restart") == scope, "Scope changed across restart");
        auto audio = MakeKey(store, "provider.restart", "song", "flac-16-44", Kind::Audio);
        auto image = MakeKey(store, "provider.restart", "cover", "cover-1200", Kind::Image);
        auto audioSnapshot = RequireSnapshot(store, audio, "restarted audio entry");
        auto imageSnapshot = RequireSnapshot(store, image, "restarted image entry");
        CheckSnapshot(audioSnapshot, audioBytes, audioMetadata, "restarted audio entry");
        CheckSnapshot(imageSnapshot, imageBytes, imageMetadata, "restarted image entry");
        Check(store.CurrentUsage(Kind::Audio).bytes == audioBytes.size(),
              "Audio usage was lost across restart");
        Check(store.CurrentUsage(Kind::Image).bytes == imageBytes.size(),
              "Image usage was lost across restart");
    }
}

void CorruptIndexRecoveryKeepsStoreOperational(const fs::path& root) {
    const std::string provider = "provider.corrupt-index";
    const auto beforeMetadata = MakeMetadata("before-corruption", 700);
    const auto afterMetadata = MakeMetadata("after-recovery", 800);
    const auto beforeBytes = Payload("entry-before-corruption", 337);
    const auto afterBytes = Payload("entry-after-recovery", 347);
    Key oldKey;
    fs::path oldPayloadPath;

    {
        Store store(root);
        oldKey = MakeKey(store, provider, "before");
        auto snapshot = CommitPayload(store, oldKey, beforeBytes, beforeMetadata, "pre-corruption entry");
        oldPayloadPath = snapshot->Path();
    }

    const auto indexPath = root / L"index.sqlite3";
    const auto damagedPath = root / L"index.damaged.sqlite3";
    const auto walPath = root / L"index.sqlite3-wal";
    const auto shmPath = root / L"index.sqlite3-shm";
    const auto damagedBytes = Payload("this-is-not-a-sqlite-index", 513);
    Check(fs::exists(indexPath), "Cache did not create an index before corruption");
    WriteBytes(indexPath, damagedBytes);
    const auto damagedWal = Payload("orphaned-wal", 97);
    const auto damagedShm = Payload("orphaned-shm", 89);
    WriteBytes(walPath, damagedWal);
    WriteBytes(shmPath, damagedShm);

    {
        Store recovered(root);
        Check(fs::exists(damagedPath), "Corrupt index was not quarantined");
        Check(ReadBytes(damagedPath) == damagedBytes, "Quarantine did not preserve the damaged index");
        if (fs::exists(walPath))
            Check(ReadBytes(walPath) != damagedWal, "Recovery reused the damaged index WAL");
        if (fs::exists(shmPath))
            Check(ReadBytes(shmPath) != damagedShm, "Recovery reused the damaged index shared-memory file");
        Check(!fs::exists(oldPayloadPath), "Recovery did not remove the orphaned old payload");
        Check(!recovered.Lookup(oldKey).has_value(), "Recovery resurrected metadata from the damaged index");

        auto fresh = MakeKey(recovered, provider, "after");
        CommitUnpinned(recovered, fresh, afterBytes, afterMetadata, "post-recovery entry");
        auto visible = RequireSnapshot(recovered, fresh, "post-recovery entry");
        CheckSnapshot(visible, afterBytes, afterMetadata, "post-recovery entry");
    }

    {
        Store reopened(root);
        Check(fs::exists(damagedPath), "Reopening discarded the quarantined index");
        auto fresh = MakeKey(reopened, provider, "after");
        auto visible = RequireSnapshot(reopened, fresh, "restarted post-recovery entry");
        CheckSnapshot(visible, afterBytes, afterMetadata, "restarted post-recovery entry");
    }
}

void CanceledBeginsLeaveNoVisibleReservations(const fs::path& root) {
    const std::string provider = "provider.canceled-begin";
    const auto originalMetadata = MakeMetadata("canceled-original", 900);
    const auto finalMetadata = MakeMetadata("canceled-final", 1000);
    const auto originalBytes = Payload("original-before-cancellations", 229);
    const auto finalBytes = Payload("committed-after-cancellations", 263);
    std::string scope;

    {
        Store store(root);
        auto key = MakeKey(store, provider, "existing");
        scope = key.scope;
        auto original = CommitPayload(store, key, originalBytes, originalMetadata, "pre-cancel entry");
        const auto originalRevision = original->Revision();
        const auto baseline = store.CurrentUsage(Kind::Data);

        for (int attempt = 0; attempt < 12; ++attempt) {
            auto canceled = store.Begin(key);
            Check(canceled != nullptr, "Canceled Begin did not create a writer");
            canceled.reset();
        }
        for (int attempt = 0; attempt < 12; ++attempt) {
            auto provisional = MakeKey(store, provider, "provisional-" + std::to_string(attempt));
            auto canceled = store.Begin(provisional);
            Check(canceled != nullptr, "Canceled provisional Begin did not create a writer");
            canceled.reset();
            Check(!store.Lookup(provisional).has_value(),
                  "Canceled Begin produced a visible provisional entry");
        }

        auto retained = RequireSnapshot(store, key, "entry retained through canceled Begins");
        CheckSnapshot(retained, originalBytes, originalMetadata, "entry retained through canceled Begins");
        Check(retained.Revision() == originalRevision, "Canceled Begin changed the published revision");
        Check(store.CurrentUsage(Kind::Data).entries == baseline.entries &&
                  store.CurrentUsage(Kind::Data).bytes == baseline.bytes,
              "Canceled Begins changed observable cache usage");

        CommitUnpinned(store, key, finalBytes, finalMetadata, "commit after canceled Begins");
        auto visible = RequireSnapshot(store, key, "commit after canceled Begins");
        CheckSnapshot(visible, finalBytes, finalMetadata, "commit after canceled Begins");
    }

    {
        Store reopened(root);
        auto key = MakeKey(reopened, provider, "existing");
        Check(key.scope == scope, "Canceled Begin runtime changed the account scope across restart");
        auto visible = RequireSnapshot(reopened, key, "restarted commit after canceled Begins");
        CheckSnapshot(visible, finalBytes, finalMetadata, "restarted commit after canceled Begins");
        const auto usage = reopened.CurrentUsage(Kind::Data);
        Check(usage.entries == 1 && usage.bytes == finalBytes.size(),
              "Canceled Begin left a visible reservation after restart");
    }
}

void InvalidFilesAndSizesMiss(const fs::path& root) {
    const auto malformedBytes = Payload("malformed-payload", 257);
    const auto truncatedBytes = Payload("truncated-payload", 271);
    const auto missingBytes = Payload("missing-payload", 283);
    const auto controlBytes = Payload("control-payload", 299);
    const auto metadata = MakeMetadata("integrity");
    fs::path malformedPath;
    fs::path truncatedPath;
    fs::path missingPath;

    {
        Store store(root);
        auto malformed = MakeKey(store, "provider.integrity", "malformed");
        auto truncated = MakeKey(store, "provider.integrity", "truncated");
        auto missing = MakeKey(store, "provider.integrity", "missing");
        auto control = MakeKey(store, "provider.integrity", "control");

        auto malformedSnapshot = CommitPayload(store, malformed, malformedBytes, metadata, "malformed seed");
        auto truncatedSnapshot = CommitPayload(store, truncated, truncatedBytes, metadata, "truncated seed");
        auto missingSnapshot = CommitPayload(store, missing, missingBytes, metadata, "missing seed");
        CommitUnpinned(store, control, controlBytes, metadata, "control seed");
        malformedPath = malformedSnapshot->Path();
        truncatedPath = truncatedSnapshot->Path();
        missingPath = missingSnapshot->Path();
    }

    auto changedBytes = malformedBytes;
    changedBytes[0] ^= 0x5a;
    changedBytes[changedBytes.size() / 2] ^= 0x3c;
    Check(changedBytes.size() == malformedBytes.size() && changedBytes != malformedBytes,
          "Malformed payload mutation did not preserve size");
    WriteBytes(malformedPath, changedBytes);
    WriteBytes(truncatedPath, std::string_view(truncatedBytes).substr(0, truncatedBytes.size() / 3));
    Check(fs::remove(missingPath), "Unable to remove the missing-payload fixture");

    Store store(root);
    auto malformed = MakeKey(store, "provider.integrity", "malformed");
    auto truncated = MakeKey(store, "provider.integrity", "truncated");
    auto missing = MakeKey(store, "provider.integrity", "missing");
    auto control = MakeKey(store, "provider.integrity", "control");
    Check(!store.Lookup(truncated).has_value(), "Truncated payload was treated as a cache hit");
    Check(!store.Lookup(missing).has_value(), "Missing payload was treated as a cache hit");
    auto controlSnapshot = RequireSnapshot(store, control, "integrity control entry");
    CheckSnapshot(controlSnapshot, controlBytes, metadata, "integrity control entry");
    auto changedSnapshot = RequireSnapshot(store, malformed, "same-size changed payload");
    CheckSnapshot(changedSnapshot, changedBytes, metadata, "same-size changed payload");
}

void UnsupportedIndexSchemaRejected(const fs::path& root) {
    {
        Store store(root);
    }
    ExecuteDatabaseSql(root / L"index.sqlite3", "PRAGMA user_version=1;");

    bool rejected = false;
    bool clearMessage = false;
    try {
        Store store(root);
    } catch (const std::exception& error) {
        rejected = true;
        clearMessage =
            std::string_view(error.what()).find("clear the disposable cache") != std::string_view::npos;
    }
    Check(rejected, "A legacy cache index schema was accepted");
    Check(clearMessage, "An unsupported cache schema did not identify the required cache reset");
}

void ByteQuotaLru(const fs::path& root) {
    Store store(root);
    constexpr std::uint64_t payloadSize = 64;
    constexpr std::size_t quotaSlots = 10;
    store.SetLimit(Kind::Data, payloadSize * quotaSlots);
    const auto metadata = MakeMetadata("lru");
    std::vector<Key> keys;
    keys.reserve(quotaSlots + 1);
    for (std::size_t index = 0; index <= quotaSlots; ++index) {
        keys.push_back(MakeKey(store, "provider.lru", "entry-" + std::to_string(index)));
        auto bytes = Payload("entry-" + std::to_string(index), payloadSize);
        if (index < quotaSlots) CommitUnpinned(store, keys.back(), bytes, metadata, "LRU initial fill");
    }

    {
        auto touched = store.Lookup(keys.front());
        Check(touched.has_value(), "LRU touch missed");
        Check(touched->Size() == payloadSize, "LRU touched entry had the wrong size");
    }
    const auto newestBytes = Payload("entry-" + std::to_string(quotaSlots), payloadSize);
    CommitUnpinned(store, keys.back(), newestBytes, metadata, "LRU overflow");
    store.Trim();

    auto touchedAfter = store.Lookup(keys.front());
    auto leastRecentAfter = store.Lookup(keys[1]);
    auto newestAfter = store.Lookup(keys.back());
    Check(touchedAfter.has_value(), "LRU ignored the access that refreshed the oldest entry");
    Check(!leastRecentAfter.has_value(), "Least recently used entry survived quota pressure");
    Check(newestAfter.has_value(), "Newest LRU entry was evicted");
    touchedAfter.reset();
    newestAfter.reset();
    const auto usage = store.CurrentUsage(Kind::Data);
    Check(usage.bytes <= payloadSize * quotaSlots, "Byte quota was exceeded");
    Check(usage.entries <= quotaSlots, "LRU kept more entries than the byte quota allows");
}

void PinnedSnapshotsSurviveEvictionAndClear(const fs::path& root) {
    Store store(root);
    constexpr std::uint64_t payloadSize = 192;
    store.SetLimit(Kind::Data, payloadSize * 2);
    const auto metadata = MakeMetadata("pins");
    auto pinnedKey = MakeKey(store, "provider.pins", "pinned");
    auto olderKey = MakeKey(store, "provider.pins", "older");
    auto newerKey = MakeKey(store, "provider.pins", "newer");
    const auto pinnedBytes = Payload("pinned", payloadSize);
    const auto olderBytes = Payload("older", payloadSize);
    const auto newerBytes = Payload("newer", payloadSize);

    auto pinned = CommitPayload(store, pinnedKey, pinnedBytes, metadata, "pinned entry");
    CommitUnpinned(store, olderKey, olderBytes, metadata, "unpinned older entry");
    CommitUnpinned(store, newerKey, newerBytes, metadata, "unpinned newer entry");
    store.Trim();

    Check(!store.Lookup(olderKey).has_value(), "Quota pressure did not evict an unpinned entry");
    CheckSnapshot(*pinned, pinnedBytes, metadata, "pinned entry after eviction");
    auto evictionUsage = store.CurrentUsage(Kind::Data);
    Check(evictionUsage.pinnedBytes >= pinned->Size(), "Usage did not report the pinned immutable payload");

    store.Clear(Kind::Data);
    const auto pinnedPath = pinned->Path();
    Check(!store.Lookup(pinnedKey).has_value(), "Clear left the cache entry visible");
    Check(!store.Lookup(newerKey).has_value(), "Clear left another cache entry visible");
    CheckSnapshot(*pinned, pinnedBytes, metadata, "pinned entry after Clear");
    Check(fs::exists(pinnedPath), "Clear removed a file still pinned by a reader");
    const auto clearedUsage = store.CurrentUsage(Kind::Data);
    Check(clearedUsage.entries == 0, "Clear left cache entries behind");
    Check(clearedUsage.bytes == pinnedBytes.size(),
          "Clear did not account for the retired pinned file still on disk");

    pinned.reset();
    store.Trim();
    Check(!fs::exists(pinnedPath), "Last snapshot destruction did not remove the retired file");
    const auto releasedUsage = store.CurrentUsage(Kind::Data);
    Check(releasedUsage.bytes == 0 && releasedUsage.entries == 0 && releasedUsage.pinnedBytes == 0,
          "Released pinned payload was not reclaimed");
    Check(clearedUsage.pinnedBytes >= pinnedBytes.size(),
          "Clear did not report the retired pinned payload in pinnedBytes");
}

void EmptyDataAndZeroLimit(const fs::path& root) {
    Store store(root);
    store.SetLimit(Kind::Data, 0);
    auto key = MakeKey(store, "provider.empty", "empty");
    const auto metadata = MakeMetadata("empty");
    auto empty = CommitPayload(store, key, {}, metadata, "empty payload");
    Check(empty->Size() == 0, "Empty payload reported a nonzero size");
    Check(ReadBytes(empty->Path()).empty(), "Empty payload file contained data");
    Check(RequireSnapshot(store, key, "empty payload").Size() == 0,
          "Empty payload did not remain cacheable at a zero byte limit");
    const auto usage = store.CurrentUsage(Kind::Data);
    Check(usage.bytes == 0 && usage.entries == 1, "Zero-byte usage did not retain the empty cache entry");
}

void ThreadedTicketCollisionDoesNotCorruptDatabase(const fs::path& root) {
    constexpr std::size_t threadCount = 8;
    constexpr std::size_t rounds = 12;
    const std::string finalBytes = Payload("final-thread-write", 401);
    const auto finalMetadata = MakeMetadata("thread-final");
    std::string provider;
    Key sharedKey;

    {
        Store store(root);
        sharedKey = MakeKey(store, "provider.thread", "collision", "shared");
        provider = sharedKey.provider;

        std::latch startGate(threadCount + 1);
        std::atomic_bool failed = false;
        std::atomic_size_t successfulCommits = 0;
        std::exception_ptr firstFailure;
        std::mutex failureMutex;
        std::vector<std::thread> workers;
        workers.reserve(threadCount);

        for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
            workers.emplace_back([&, threadIndex] {
                startGate.arrive_and_wait();
                try {
                    for (std::size_t round = 0; round < rounds && !failed.load(); ++round) {
                        auto writer = store.Begin(sharedKey);
                        if (!writer) Fail("Threaded Begin returned no writer");
                        const auto bytes = Payload("thread-" + std::to_string(threadIndex) + "-" +
                                                       std::to_string(round) + "-payload",
                                                   173);
                        WriteBytes(writer->Path(), bytes);
                        auto committed = writer->Commit(MakeMetadata("thread", 500));
                        if (committed) ++successfulCommits;
                    }
                } catch (...) {
                    failed.store(true);
                    std::lock_guard lock(failureMutex);
                    if (!firstFailure) firstFailure = std::current_exception();
                }
            });
        }

        startGate.count_down();
        for (auto& worker : workers)
            worker.join();
        if (firstFailure) std::rethrow_exception(firstFailure);
        Check(successfulCommits.load() > 0, "No colliding writer ever committed");

        auto finalWriter = store.Begin(sharedKey);
        Check(finalWriter != nullptr, "Final recovery writer did not start");
        WriteBytes(finalWriter->Path(), finalBytes);
        auto finalSnapshot = finalWriter->Commit(finalMetadata);
        Check(finalSnapshot.has_value(), "Final recovery write did not commit");
        CheckSnapshot(*finalSnapshot, finalBytes, finalMetadata, "final recovery write");
        const auto threadedUsage = store.CurrentUsage(Kind::Data);
        Check(threadedUsage.entries == 1 && threadedUsage.bytes == finalBytes.size(),
              "Threaded replacements left stale or unbounded cache entries");
    }

    {
        Store reopened(root);
        auto shared = MakeKey(reopened, provider, "collision", "shared");
        auto persisted = RequireSnapshot(reopened, shared, "threaded database reopen");
        CheckSnapshot(persisted, finalBytes, finalMetadata, "threaded database reopen");

        const auto recoveryBytes = Payload("post-reopen-recovery", 353);
        const auto recoveryMetadata = MakeMetadata("thread-reopen", 600);
        CommitUnpinned(reopened, shared, recoveryBytes, recoveryMetadata, "post-reopen recovery write");
        auto recovered = RequireSnapshot(reopened, shared, "post-reopen recovery read");
        CheckSnapshot(recovered, recoveryBytes, recoveryMetadata, "post-reopen recovery read");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "Usage: cache_store_tests.exe <unique-test-root>");
        const auto root = ValidateRootArgument(argv[1], fs::current_path() / ".local/tests");
        Check(!fs::exists(root), "Unique cache test root already exists");
        fs::create_directories(root);

        std::vector<std::string> failures;
        const auto run = [&](std::string_view name, auto function) {
            auto failure = RunCase(root, name, function);
            if (!failure.empty()) failures.push_back(std::string(name) + ": " + failure);
        };
        run("scope", ScopePersistenceAndRotation);
        run("keys", StableKeyEncodingAndQuality);
        run("replacement", OldSnapshotSurvivesReplacement);
        run("tickets", WriteTicketsAndAbandonment);
        run("restart", RestartPersistence);
        run("corrupt-index", CorruptIndexRecoveryKeepsStoreOperational);
        run("canceled-begin", CanceledBeginsLeaveNoVisibleReservations);
        run("files", InvalidFilesAndSizesMiss);
        run("schema", UnsupportedIndexSchemaRejected);
        run("lru", ByteQuotaLru);
        run("pins", PinnedSnapshotsSurviveEvictionAndClear);
        run("empty", EmptyDataAndZeroLimit);
        run("threads", ThreadedTicketCollisionDoesNotCorruptDatabase);

        if (!failures.empty()) {
            SafeRemove(root, root, true);
            for (const auto& failure : failures)
                std::cerr << failure << '\n';
            return 1;
        }

        SafeRemove(root, root, true);
        std::cout << "Cache store checks passed: " << checkCount.load() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
