#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct sqlite3;

namespace glasswave::cache {

enum class Kind {
    Audio = 0,
    Image = 1,
    Data = 2
};

// Stable provider identity, never a signed URL. scope is an opaque account generation.
// variant distinguishes actual source encoding/quality and producer schema versions.
struct Key {
    Kind kind = Kind::Data;
    std::string provider;
    std::string scope;
    std::string identity;
    std::string variant;
};

struct Metadata {
    std::string contentType;
    std::string etag;
    std::string lastModified;
    std::string extra; // Small, versioned consumer metadata; no credentials or signed URLs.
    int64_t validatedAt = 0;
};

class Store;
class Snapshot {
  public:
    const std::filesystem::path& Path() const;
    uint64_t Size() const;
    uint64_t Revision() const;
    const Metadata& Info() const;
    explicit operator bool() const { return static_cast<bool>(m_blob); }

  private:
    struct Blob;
    std::shared_ptr<Blob> m_blob; // Pins this immutable generation against eviction.
    Metadata m_info;              // As read; a later Revalidate does not change this copy.
    friend class Store;
};

class Writer {
  public:
    ~Writer();
    const std::filesystem::path& Path() const { return m_path; }
    // Close/flush writer handles first, or open them with FILE_SHARE_DELETE.
    // Only the latest write ticket in the still-current account scope can publish.
    // `flushed`: the caller has already flushed the staging file (outside the store's lock), so
    // Commit does not flush it again while holding that lock.
    std::optional<Snapshot> Commit(Metadata metadata, bool flushed = false);

  private:
    Writer(Store& store, Key key, std::string digest, uint64_t ticket, std::filesystem::path path);
    Store& m_store;
    Key m_key;
    std::string m_digest;
    uint64_t m_ticket;
    std::filesystem::path m_path;
    friend class Store;
};

struct Usage {
    uint64_t bytes = 0, entries = 0, pinnedBytes = 0;
};

// SQLite WAL publishes metadata atomically; immutable payloads keep old readers valid.
// This class has no UI/foobar dependencies and can be exercised in standalone tests.
class Store {
  public:
    explicit Store(std::filesystem::path root);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    std::string Scope(const std::string& provider);
    void RotateScope(const std::string& provider);
    std::optional<Snapshot> Lookup(const Key& key);
    std::unique_ptr<Writer> Begin(const Key& key);
    void Erase(const Key& key);
    // Records that the published generation `snapshot` was confirmed current (a 304): only
    // its validators and validation time change, the payload is neither copied nor replaced.
    // False when another generation has been published since.
    bool Revalidate(const Key& key, const Snapshot& snapshot, const Metadata& metadata);
    void SetLimit(Kind kind, uint64_t bytes);
    Usage CurrentUsage(Kind kind);
    void Trim();
    void Clear(Kind kind); // Pinned readers stay usable; future lookups miss.
    const std::filesystem::path& Root() const { return m_root; }
    // A bounded, filename-safe index key prevents unbounded composite keys. This is an
    // identity index only; it is never used to verify payload bytes.
    static std::string Digest(const Key& key);
    static int64_t Now();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::filesystem::path m_root;
    std::optional<Snapshot> Commit(Writer& writer, Metadata metadata, bool flushed);
    void Abandon(Writer& writer) noexcept;
    friend class Writer;
};

// Component-owned instance, rooted in <profile>/glasswave/cache.
// GetStore() is initialized by provider workers, never by painting.
Store& GetStore();
std::string AccountScope(const std::string& provider);
void RotateAccountScope(const std::string& provider);
// In-memory account generation for UI keys; never opens the index or touches disk.
uint64_t AccountRevision(const std::string& provider);

} // namespace glasswave::cache
