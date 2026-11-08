#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace glasswave::netease {

// Disk cache for downloaded covers. The index is persisted and entries are evicted by
// frequency (LFU) with last-use as the tie-breaker; frequencies are halved periodically so
// an old popular image cannot hold a slot forever. The total size is bounded by
// CoverCacheLimitMb().
class CoverCache {
public:
    static CoverCache& Get();

    bool Read(const std::wstring& url, std::vector<unsigned char>& bytes);
    void Write(const std::wstring& url, const std::string& bytes);

private:
    CoverCache();
    ~CoverCache();
    CoverCache(const CoverCache&) = delete;
    CoverCache& operator=(const CoverCache&) = delete;

    struct Entry {
        std::wstring file;
        uint64_t size = 0;
        uint32_t hits = 1;
        int64_t lastUsed = 0;
    };

    void LoadLocked();
    void SaveLocked();
    void EvictLocked(const std::wstring& keep);
    void AgeLocked();
    std::filesystem::path FileFor(const std::wstring& hash) const;

    std::mutex m_mutex;
    std::filesystem::path m_dir;
    std::unordered_map<std::wstring, Entry> m_entries;
    uint64_t m_total = 0;
    uint64_t m_touches = 0;
    uint32_t m_inserts = 0;
    bool m_loaded = false;
};

}  // namespace glasswave::netease
