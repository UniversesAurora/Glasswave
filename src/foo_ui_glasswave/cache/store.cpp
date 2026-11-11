#ifndef GLASSWAVE_CACHE_STANDALONE
#include "pch.h"
#endif
#include "cache/store.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace glasswave::cache {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t kMiB = 1024ull * 1024;
constexpr size_t kMetadataLimit = 64 * 1024;
constexpr int kIndexSchemaVersion = 2;

void Check(int result) {
    if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
        throw std::runtime_error("Cache index operation failed.");
}
struct Statement {
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* db, const char* sql) { Check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr)); }
    ~Statement() { sqlite3_finalize(value); }
    void Text(int at, const std::string& text) {
        if (text.size() > kMetadataLimit) throw std::runtime_error("Cache metadata is too large.");
        Check(sqlite3_bind_text(value, at, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT));
    }
    void Int(int at, int64_t number) { Check(sqlite3_bind_int64(value, at, number)); }
    bool Step() {
        const int result = sqlite3_step(value);
        Check(result);
        return result == SQLITE_ROW;
    }
    std::string Text(int at) const {
        const auto* data = sqlite3_column_text(value, at);
        const int size = sqlite3_column_bytes(value, at);
        return data ? std::string(reinterpret_cast<const char*>(data), static_cast<size_t>(size)) : "";
    }
    int64_t Int(int at) const { return sqlite3_column_int64(value, at); }
};
void Execute(sqlite3* db, const char* sql) {
    Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
struct Transaction {
    sqlite3* db;
    bool committed = false;
    explicit Transaction(sqlite3* value) : db(value) { Execute(db, "BEGIN IMMEDIATE"); }
    void Commit() {
        Execute(db, "COMMIT");
        committed = true;
    }
    ~Transaction() {
        if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};

std::string Hex(const unsigned char* data, size_t size) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        text += hex[data[i] >> 4];
        text += hex[data[i] & 15];
    }
    return text;
}
std::string Random() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Cannot create a cache generation.");
    return Hex(bytes.data(), bytes.size());
}
bool PayloadName(const std::string& name) {
    return name.size() == 37 && name.substr(32) == ".blob" &&
           std::all_of(name.begin(), name.begin() + 32,
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
void Remove(const fs::path& path) noexcept {
    std::error_code ec;
    fs::remove(path, ec);
}

void ProtectDirectory(const fs::path& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Cannot protect the cache directory.");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    const BOOL read = GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!read || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
        throw std::runtime_error("Cannot identify the cache owner.");
    const std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                              nullptr))
        throw std::runtime_error("Cannot prepare cache permissions.");
    const BOOL applied = SetFileSecurityW(
        path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    if (!applied) throw std::runtime_error("Cannot protect cached media.");
}
} // namespace

struct Snapshot::Blob {
    fs::path path;
    Kind kind = Kind::Data;
    uint64_t size = 0, revision = 0;
    Metadata metadata;
    std::atomic_bool retired = false;
    ~Blob() {
        if (retired) Remove(path);
    }
};

const fs::path& Snapshot::Path() const {
    return m_blob->path;
}
uint64_t Snapshot::Size() const {
    return m_blob->size;
}
uint64_t Snapshot::Revision() const {
    return m_blob->revision;
}
const Metadata& Snapshot::Info() const {
    return m_blob->metadata;
}

struct Store::Impl {
    std::mutex mutex;
    sqlite3* db = nullptr;
    HANDLE owner = INVALID_HANDLE_VALUE;
    fs::path root;
    std::map<std::string, std::weak_ptr<Snapshot::Blob>> pins;
    std::array<uint64_t, 3> limits{10'240 * kMiB, 512 * kMiB, 128 * kMiB};
    int64_t access = 0;

    ~Impl() {
        if (db) sqlite3_close_v2(db);
        if (owner != INVALID_HANDLE_VALUE) CloseHandle(owner);
    }
    std::string Scope(const std::string& provider) {
        Statement read(db, "SELECT token FROM scopes WHERE provider=?");
        read.Text(1, provider);
        if (read.Step()) return read.Text(0);
        const auto token = Random();
        Statement insert(db, "INSERT INTO scopes(provider,token) VALUES(?,?)");
        insert.Text(1, provider);
        insert.Text(2, token);
        insert.Step();
        return token;
    }
    bool Current(const Key& key) { return key.scope.empty() || Scope(key.provider) == key.scope; }
    void Retire(const std::string& name) {
        if (!PayloadName(name)) return;
        const auto found = pins.find(name);
        if (found != pins.end()) {
            if (auto blob = found->second.lock()) {
                blob->retired = true;
                return;
            }
            pins.erase(found);
        }
        Remove(root / L"objects" / name);
    }
    bool Pinned(const std::string& name) {
        const auto found = pins.find(name);
        return found != pins.end() && !found->second.expired();
    }
    void Trim() {
        for (int kind = 0; kind < 3; ++kind) {
            Statement total(db, "SELECT COALESCE(SUM(size),0) FROM entries WHERE kind=?");
            total.Int(1, kind);
            total.Step();
            uint64_t bytes = static_cast<uint64_t>(total.Int(0));
            const uint64_t limit = limits[static_cast<size_t>(kind)];
            if (bytes <= limit) continue;
            struct Candidate {
                std::string key, file;
                uint64_t size;
            };
            std::vector<Candidate> candidates;
            {
                Statement rows(db, "SELECT k,file,size FROM entries WHERE kind=? AND file<>'' ORDER BY used");
                rows.Int(1, kind);
                while (rows.Step())
                    candidates.push_back({rows.Text(0), rows.Text(1), static_cast<uint64_t>(rows.Int(2))});
            }
            for (const auto& candidate : candidates) {
                if (bytes <= limit * 9 / 10) break;
                if (Pinned(candidate.file)) continue;
                Statement drop(db, "DELETE FROM entries WHERE k=?");
                drop.Text(1, candidate.key);
                drop.Step();
                Retire(candidate.file);
                bytes -= std::min(bytes, candidate.size);
            }
        }
        for (auto it = pins.begin(); it != pins.end();) {
            if (it->second.expired())
                it = pins.erase(it);
            else
                ++it;
        }
    }
};

Store::Store(fs::path root) : m_impl(std::make_unique<Impl>()), m_root(fs::absolute(std::move(root))) {
    fs::create_directories(m_root);
    ProtectDirectory(m_root);
    fs::create_directories(m_root / L"objects");
    fs::create_directories(m_root / L"staging");
    m_impl->root = m_root;
    // Exclusive ownership makes startup orphan cleanup safe across portable instances.
    m_impl->owner = CreateFileW((m_root / L"owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_impl->owner == INVALID_HANDLE_VALUE) throw std::runtime_error("The cache is already in use.");
    const auto index = m_root / L"index.sqlite3";
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            Check(sqlite3_open16(index.c_str(), &m_impl->db));
            sqlite3_busy_timeout(m_impl->db, 3000);
            Execute(m_impl->db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
                                "PRAGMA wal_autocheckpoint=256; PRAGMA journal_size_limit=16777216;");
            int schemaVersion = 0;
            {
                Statement version(m_impl->db, "PRAGMA user_version");
                version.Step();
                schemaVersion = static_cast<int>(version.Int(0));
            }
            if (schemaVersion == 0) {
                Statement tables(m_impl->db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                                             "name IN ('scopes','sequence','entries')");
                tables.Step();
                if (tables.Int(0) != 0)
                    throw std::runtime_error(
                        "The cache index uses an unsupported format; clear the disposable cache.");
                Execute(m_impl->db,
                        "CREATE TABLE scopes(provider TEXT PRIMARY KEY,token TEXT NOT NULL);"
                        "CREATE TABLE sequence(n INTEGER NOT NULL);"
                        "INSERT INTO sequence VALUES(0);"
                        "CREATE TABLE entries(k TEXT PRIMARY KEY,provider TEXT NOT NULL,scope TEXT NOT NULL,"
                        "kind INTEGER NOT NULL,gen INTEGER NOT NULL,rev INTEGER NOT NULL DEFAULT 0,"
                        "file TEXT NOT NULL DEFAULT '',size INTEGER NOT NULL DEFAULT 0,"
                        "used INTEGER NOT NULL DEFAULT 0,validated INTEGER NOT NULL DEFAULT 0,"
                        "mime TEXT NOT NULL DEFAULT '',etag TEXT NOT NULL DEFAULT '',"
                        "modified TEXT NOT NULL DEFAULT '',extra TEXT NOT NULL DEFAULT '');"
                        "CREATE INDEX entries_lru ON entries(kind,used);"
                        "PRAGMA user_version=2;");
            } else if (schemaVersion != kIndexSchemaVersion) {
                throw std::runtime_error(
                    "The cache index uses an unsupported format; clear the disposable cache.");
            }
            Execute(m_impl->db, "DELETE FROM entries WHERE file='';");
            break;
        } catch (...) {
            const int code = m_impl->db ? sqlite3_errcode(m_impl->db) : SQLITE_ERROR;
            if (attempt != 0 || (code != SQLITE_CORRUPT && code != SQLITE_NOTADB)) throw;
            sqlite3_close_v2(m_impl->db);
            m_impl->db = nullptr;
            const auto backup = m_root / L"index.damaged.sqlite3";
            if (!MoveFileExW(index.c_str(), backup.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw;
            // Only our index sidecars, under the exclusively owned cache root.
            Remove(m_root / L"index.sqlite3-wal");
            Remove(m_root / L"index.sqlite3-shm");
        }
    }
    Statement clock(m_impl->db, "SELECT COALESCE(MAX(used),0) FROM entries");
    clock.Step();
    m_impl->access = clock.Int(0);
    std::set<std::string> referenced;
    {
        Statement files(m_impl->db, "SELECT file FROM entries WHERE file<>''");
        while (files.Step())
            referenced.insert(files.Text(0));
    }
    for (const auto& file : fs::directory_iterator(m_root / L"objects")) {
        const auto name = file.path().filename().string();
        if (PayloadName(name) && !referenced.contains(name)) Remove(file.path());
    }
    for (const auto& file : fs::directory_iterator(m_root / L"staging")) {
        const auto name = file.path().filename().string();
        if (name.size() == 37 && name.substr(32) == ".part" && PayloadName(name.substr(0, 32) + ".blob"))
            Remove(file.path());
    }
    m_impl->Trim();
}
Store::~Store() = default;

int64_t Store::Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string Store::Digest(const Key& key) {
    // SQLite keys stay bounded and filename-safe. This digest indexes identity only and never
    // participates in payload validation.
    std::string data = "GlasswaveCache-v1";
    const auto field = [&data](const std::string& value) {
        if (value.size() > kMetadataLimit) throw std::runtime_error("Cache identity is too large.");
        const uint64_t size = value.size();
        for (unsigned i = 0; i < 8; ++i)
            data.push_back(static_cast<char>((size >> (i * 8)) & 255));
        data += value;
    };
    field(std::to_string(static_cast<int>(key.kind)));
    field(key.provider);
    field(key.scope);
    field(key.identity);
    field(key.variant);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize the cache key digest.");
    std::array<unsigned char, 32> digest{};
    const auto result =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(data.data()),
                   static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) throw std::runtime_error("Cannot hash a cache identity.");
    return Hex(digest.data(), digest.size());
}

std::string Store::Scope(const std::string& provider) {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->Scope(provider);
}
void Store::RotateScope(const std::string& provider) {
    std::lock_guard lock(m_impl->mutex);
    Statement rotate(m_impl->db, "INSERT INTO scopes(provider,token) VALUES(?,?) "
                                 "ON CONFLICT(provider) DO UPDATE SET token=excluded.token");
    rotate.Text(1, provider);
    rotate.Text(2, Random());
    rotate.Step();
}

std::optional<Snapshot> Store::Lookup(const Key& key) {
    const auto digest = Digest(key);
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->Current(key)) return std::nullopt;
    std::string name;
    int64_t size = 0, revision = 0;
    Metadata metadata;
    {
        Statement read(m_impl->db, "SELECT file,size,rev,validated,mime,etag,modified,extra FROM entries "
                                   "WHERE k=?");
        read.Text(1, digest);
        if (!read.Step()) return std::nullopt;
        name = read.Text(0);
        size = read.Int(1);
        revision = read.Int(2);
        metadata = {read.Text(4), read.Text(5), read.Text(6), read.Text(7), read.Int(3)};
    }
    if (name.empty()) return std::nullopt;
    std::error_code ec;
    const auto path = m_root / L"objects" / name;
    if (!PayloadName(name) || size < 0 || revision < 0 ||
        fs::file_size(path, ec) != static_cast<uint64_t>(size) || ec) {
        Statement drop(m_impl->db, "DELETE FROM entries WHERE k=?");
        drop.Text(1, digest);
        drop.Step();
        m_impl->Retire(name);
        return std::nullopt;
    }
    Snapshot snapshot;
    snapshot.m_blob = m_impl->pins[name].lock();
    if (!snapshot.m_blob) {
        snapshot.m_blob = std::make_shared<Snapshot::Blob>();
        auto& blob = *snapshot.m_blob;
        blob.path = path;
        blob.kind = key.kind;
        blob.size = static_cast<uint64_t>(size);
        blob.revision = static_cast<uint64_t>(revision);
        blob.metadata = std::move(metadata);
        m_impl->pins[name] = snapshot.m_blob;
    }
    Statement touch(m_impl->db, "UPDATE entries SET used=? WHERE k=?");
    touch.Int(1, ++m_impl->access);
    touch.Text(2, digest);
    touch.Step();
    return snapshot;
}

Writer::Writer(Store& store, Key key, std::string digest, uint64_t ticket, fs::path path)
    : m_store(store), m_key(std::move(key)), m_digest(std::move(digest)), m_ticket(ticket),
      m_path(std::move(path)) {}
Writer::~Writer() {
    if (!m_path.empty()) m_store.Abandon(*this);
}
std::optional<Snapshot> Writer::Commit(Metadata metadata) {
    return m_store.Commit(*this, std::move(metadata));
}
void Store::Abandon(Writer& writer) noexcept {
    Remove(writer.m_path);
    try {
        std::lock_guard lock(m_impl->mutex);
        Statement drop(m_impl->db, "DELETE FROM entries WHERE k=? AND gen=? AND file=''");
        drop.Text(1, writer.m_digest);
        drop.Int(2, static_cast<int64_t>(writer.m_ticket));
        drop.Step();
    } catch (...) { /* A failed cache cleanup cannot interrupt playback shutdown. */
    }
}

std::unique_ptr<Writer> Store::Begin(const Key& key) {
    const auto digest = Digest(key);
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->Current(key)) throw std::runtime_error("The cache account scope has changed.");
    Transaction transaction(m_impl->db);
    Execute(m_impl->db, "UPDATE sequence SET n=n+1");
    Statement sequence(m_impl->db, "SELECT n FROM sequence");
    sequence.Step();
    const auto ticket = static_cast<uint64_t>(sequence.Int(0));
    Statement reserve(m_impl->db, "INSERT INTO entries(k,provider,scope,kind,gen) VALUES(?,?,?,?,?) "
                                  "ON CONFLICT(k) DO UPDATE SET gen=excluded.gen");
    reserve.Text(1, digest);
    reserve.Text(2, key.provider);
    reserve.Text(3, key.scope);
    reserve.Int(4, static_cast<int>(key.kind));
    reserve.Int(5, static_cast<int64_t>(ticket));
    reserve.Step();
    const auto path = m_root / L"staging" / (Random() + ".part");
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create a cache staging file.");
    CloseHandle(file);
    try {
        transaction.Commit();
        return std::unique_ptr<Writer>(new Writer(*this, key, digest, ticket, path));
    } catch (...) {
        Remove(path);
        throw;
    }
}

std::optional<Snapshot> Store::Commit(Writer& writer, Metadata metadata) {
    if (writer.m_path.empty()) return std::nullopt;
    const auto withinLimit = [](const std::string& value) { return value.size() <= kMetadataLimit; };
    if (!withinLimit(metadata.contentType) || !withinLimit(metadata.etag) ||
        !withinLimit(metadata.lastModified) || !withinLimit(metadata.extra))
        return std::nullopt;
    std::error_code ec;
    const auto size = fs::file_size(writer.m_path, ec);
    if (ec || size > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->Current(writer.m_key)) return std::nullopt;
    Transaction transaction(m_impl->db);
    Statement existing(m_impl->db, "SELECT gen,file FROM entries WHERE k=?");
    existing.Text(1, writer.m_digest);
    if (!existing.Step() || static_cast<uint64_t>(existing.Int(0)) != writer.m_ticket) return std::nullopt;
    const auto previous = existing.Text(1);
    const auto name = Random() + ".blob";
    const auto destination = m_root / L"objects" / name;
    // Flush data before publishing the SQLite reference. On failure the old entry survives.
    HANDLE file = CreateFileW(writer.m_path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    const BOOL flushed = FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed || !MoveFileExW(writer.m_path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        return std::nullopt;
    try {
        Statement publish(m_impl->db, "UPDATE entries SET file=?,size=?,used=?,validated=?,mime=?,etag=?,"
                                      "modified=?,extra=?,rev=? WHERE k=? AND gen=?");
        publish.Text(1, name);
        publish.Int(2, static_cast<int64_t>(size));
        publish.Int(3, ++m_impl->access);
        publish.Int(4, metadata.validatedAt ? metadata.validatedAt : Now());
        publish.Text(5, metadata.contentType);
        publish.Text(6, metadata.etag);
        publish.Text(7, metadata.lastModified);
        publish.Text(8, metadata.extra);
        publish.Int(9, static_cast<int64_t>(writer.m_ticket));
        publish.Text(10, writer.m_digest);
        publish.Int(11, static_cast<int64_t>(writer.m_ticket));
        publish.Step();
        transaction.Commit();
    } catch (...) {
        Remove(destination);
        throw;
    }
    writer.m_path.clear();
    if (previous != name) m_impl->Retire(previous);
    Snapshot snapshot;
    snapshot.m_blob = std::make_shared<Snapshot::Blob>();
    snapshot.m_blob->path = destination;
    snapshot.m_blob->kind = writer.m_key.kind;
    snapshot.m_blob->size = size;
    snapshot.m_blob->revision = writer.m_ticket;
    snapshot.m_blob->metadata = std::move(metadata);
    if (!snapshot.m_blob->metadata.validatedAt) snapshot.m_blob->metadata.validatedAt = Now();
    m_impl->pins[name] = snapshot.m_blob;
    m_impl->Trim();
    return snapshot;
}

void Store::Erase(const Key& key) {
    std::lock_guard lock(m_impl->mutex);
    const auto digest = Digest(key);
    Statement read(m_impl->db, "SELECT file FROM entries WHERE k=?");
    read.Text(1, digest);
    const auto name = read.Step() ? read.Text(0) : "";
    Statement drop(m_impl->db, "DELETE FROM entries WHERE k=?");
    drop.Text(1, digest);
    drop.Step();
    m_impl->Retire(name);
}
void Store::SetLimit(Kind kind, uint64_t bytes) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->limits.at(static_cast<size_t>(kind)) = bytes;
    m_impl->Trim();
}
void Store::Trim() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->Trim();
}
Usage Store::CurrentUsage(Kind kind) {
    std::lock_guard lock(m_impl->mutex);
    Usage usage;
    Statement read(m_impl->db,
                   "SELECT COUNT(*),COALESCE(SUM(size),0) FROM entries WHERE kind=? AND file<>''");
    read.Int(1, static_cast<int>(kind));
    read.Step();
    usage.entries = static_cast<uint64_t>(read.Int(0));
    usage.bytes = static_cast<uint64_t>(read.Int(1));
    for (const auto& [name, weak] : m_impl->pins) {
        if (const auto blob = weak.lock()) {
            if (blob->kind != kind) continue;
            usage.pinnedBytes += blob->size;
            if (blob->retired) usage.bytes += blob->size;
        }
    }
    return usage;
}
void Store::Clear(Kind kind) {
    std::lock_guard lock(m_impl->mutex);
    std::vector<std::string> names;
    {
        Statement read(m_impl->db, "SELECT file FROM entries WHERE kind=?");
        read.Int(1, static_cast<int>(kind));
        while (read.Step())
            names.push_back(read.Text(0));
    }
    Statement drop(m_impl->db, "DELETE FROM entries WHERE kind=?");
    drop.Int(1, static_cast<int>(kind));
    drop.Step();
    for (const auto& name : names)
        m_impl->Retire(name);
}

} // namespace glasswave::cache
