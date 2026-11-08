#include "pch.h"
#include "netease/config.h"
#include "netease/cover_cache.h"
#include "netease/log.h"
#include "netease/profile.h"

#include <algorithm>
#include <bcrypt.h>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <vector>

namespace glasswave::netease {

namespace {

constexpr uint64_t kMb = 1024ull * 1024ull;

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_wide_from_utf8(text.c_str()).get_ptr();
}

std::wstring Hash(const std::wstring& text) {
    const std::string utf8 = Utf8(text);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    unsigned char digest[32]{};
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(utf8.data())),
                   static_cast<ULONG>(utf8.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(64);
    for (unsigned char byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0f]);
    }
    return out;
}

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string Clean(std::wstring_view text) {
    std::wstring copy(text);
    for (auto& c : copy)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return Utf8(copy);
}

}  // namespace

CoverCache& CoverCache::Get() {
    static CoverCache cache;
    return cache;
}

CoverCache::CoverCache() {
    m_dir = CoverDir();
    std::lock_guard lock(m_mutex);
    LoadLocked();
}

CoverCache::~CoverCache() {
    std::lock_guard lock(m_mutex);
    SaveLocked();
}

std::filesystem::path CoverCache::FileFor(const std::wstring& hash) const {
    return m_dir.empty() ? std::filesystem::path() : m_dir / (hash + L".img");
}

void CoverCache::LoadLocked() {
    if (m_loaded) return;
    m_loaded = true;
    if (m_dir.empty() || !EnsureDir(m_dir)) return;

    const auto indexPath = m_dir / L"index.tsv";
    std::ifstream index(indexPath, std::ios::binary);
    std::string line;
    while (std::getline(index, line)) {
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= line.size() && fields.size() < 5) {
            const size_t end = line.find('\t', start);
            fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (fields.size() < 5) continue;
        Entry entry;
        entry.file = Wide(fields[0]);
        entry.size = std::strtoull(fields[1].c_str(), nullptr, 10);
        entry.hits = static_cast<uint32_t>(std::strtoul(fields[2].c_str(), nullptr, 10));
        entry.lastUsed = _strtoi64(fields[3].c_str(), nullptr, 10);
        const std::wstring url = Wide(fields[4]);
        const auto path = FileFor(entry.file);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) continue;
        entry.size = std::filesystem::file_size(path, ec);
        if (ec) continue;
        m_total += entry.size;
        m_entries.emplace(url, std::move(entry));
    }

    // Remove files that are not referenced by the index (including caches written by
    // earlier versions with a different naming scheme).
    std::unordered_map<std::wstring, bool> known;
    for (const auto& [url, entry] : m_entries) known.emplace(entry.file + L".img", true);
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(m_dir, ec)) {
        if (ec) break;
        if (!item.is_regular_file()) continue;
        const std::wstring name = item.path().filename().wstring();
        if (name == L"index.tsv" || name.size() < 4) continue;
        if (name.substr(name.size() - 4) != L".img") continue;
        if (!known.count(name)) std::filesystem::remove(item.path(), ec);
    }
}

void CoverCache::SaveLocked() {
    if (m_dir.empty()) return;
    const auto path = m_dir / L"index.tsv";
    auto tmp = path;
    tmp += L".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& [url, entry] : m_entries)
        out << Clean(entry.file) << '\t' << entry.size << '\t' << entry.hits << '\t' << entry.lastUsed << '\t'
            << Clean(url) << '\n';
    out.close();
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool CoverCache::Read(const std::wstring& url, std::vector<unsigned char>& bytes) {
    std::lock_guard lock(m_mutex);
    if (!m_loaded) LoadLocked();
    const auto it = m_entries.find(url);
    if (it == m_entries.end()) return false;
    const auto path = FileFor(it->second.file);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        m_total -= std::min<uint64_t>(m_total, it->second.size);
        m_entries.erase(it);
        SaveLocked();
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    it->second.hits = std::min<uint32_t>(it->second.hits + 1, 1000000);
    it->second.lastUsed = Now();
    if (++m_touches % 64 == 0) SaveLocked();
    return true;
}

void CoverCache::Write(const std::wstring& url, const std::string& bytes) {
    if (bytes.empty() || m_dir.empty()) return;
    const std::wstring hash = Hash(url);
    if (hash.empty() || !EnsureDir(m_dir)) return;
    const auto path = FileFor(hash);
    auto part = path;  // per thread: two threads may fetch the same cover at once
    part += L"." + std::to_wstring(GetCurrentThreadId()) + L".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(part, ec);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(part, path, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return;
    }

    std::lock_guard lock(m_mutex);
    if (!m_loaded) LoadLocked();
    auto [it, inserted] = m_entries.try_emplace(url);
    if (!inserted) m_total -= std::min<uint64_t>(m_total, it->second.size);
    it->second = {hash, static_cast<uint64_t>(bytes.size()), inserted ? 1u : std::max<uint32_t>(1u, it->second.hits),
                  Now()};
    m_total += it->second.size;
    if (++m_inserts % 256 == 0) AgeLocked();
    EvictLocked(url);
    SaveLocked();
}

CoverCache::Usage CoverCache::CurrentUsage() {
    std::lock_guard lock(m_mutex);
    if (!m_loaded) LoadLocked();
    return {m_total, m_entries.size()};
}

void CoverCache::Clear() {
    std::lock_guard lock(m_mutex);
    if (!m_loaded) LoadLocked();
    std::error_code ec;
    for (const auto& [url, entry] : m_entries) std::filesystem::remove(FileFor(entry.file), ec);
    m_entries.clear();
    m_total = 0;
    SaveLocked();
}

void CoverCache::Trim() {
    std::lock_guard lock(m_mutex);
    if (!m_loaded) LoadLocked();
    EvictLocked({});
    SaveLocked();
}

void CoverCache::AgeLocked() {
    // Keep the cache responsive to new listening habits instead of letting historical
    // popularity pin an entry forever.
    for (auto& [url, entry] : m_entries) entry.hits = std::max<uint32_t>(1u, (entry.hits + 1) / 2);
}

// Least frequently used first, the oldest among equals; never the cover just written,
// which would otherwise always lose (it has been used once) and be fetched again.
void CoverCache::EvictLocked(const std::wstring& keep) {
    const uint64_t limit = static_cast<uint64_t>(CoverCacheLimitMb()) * kMb;
    if (m_total <= limit || m_entries.empty()) return;
    const uint64_t target = limit * 9 / 10;

    struct Candidate {
        std::wstring url;
        Entry* entry;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(m_entries.size());
    for (auto& [url, entry] : m_entries)
        if (url != keep) candidates.push_back({url, &entry});
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.entry->hits != b.entry->hits) return a.entry->hits < b.entry->hits;
        return a.entry->lastUsed < b.entry->lastUsed;
    });

    size_t removed = 0;
    uint64_t freed = 0;
    for (const auto& candidate : candidates) {
        if (m_total <= target) break;
        const auto path = FileFor(candidate.entry->file);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        const uint64_t size = candidate.entry->size;
        m_total -= std::min<uint64_t>(m_total, size);
        m_entries.erase(candidate.url);
        freed += size;
        ++removed;
    }
    if (removed) {
        Log(L"cover cache: evicted " + std::to_wstring(removed) + L" files, " +
            std::to_wstring(freed / 1024) + L" KiB");
    }
}

}  // namespace glasswave::netease
