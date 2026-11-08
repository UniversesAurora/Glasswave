#include "pch.h"
#include "media/play_stats.h"
#include "netease/catalog.h"
#include "netease/handle.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace glasswave::media {

namespace {

constexpr size_t kMaxRecords = 2000;

std::filesystem::path StatsFile() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return {};
    } catch (...) {
        return {};
    }
    std::wstring base = pfc::stringcvt::string_wide_from_utf8(native.get_ptr()).get_ptr();
    if (base.empty()) return {};
    if (base.back() != L'\\') base += L'\\';
    return std::filesystem::path(base + L"glasswave") / L"play-stats.txt";
}

std::string Escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '%': out += "%25"; break;
        case '\t': out += "%09"; break;
        case '\n': out += "%0A"; break;
        case '\r': out += "%0D"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string Unescape(std::string_view text) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex(text[i + 1]), lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string Utf8(const std::wstring& text) {
    return pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr();
}

// A titleformat field of a local track, with the scripts compiled once.
std::string LocalField(const metadb_handle_ptr& track, const char* script) {
    static std::map<std::string, titleformat_object::ptr> scripts;
    auto it = scripts.find(script);
    if (it == scripts.end()) {
        titleformat_object::ptr compiled;
        titleformat_compiler::get()->compile_safe(compiled, script);
        it = scripts.emplace(script, compiled).first;
    }
    pfc::string8 out;
    if (it->second.is_valid()) track->format_title(nullptr, out, it->second, nullptr);
    return out.get_ptr();
}

int64_t ToInt(std::string_view text) { return std::strtoll(std::string(text).c_str(), nullptr, 10); }

// The stable identity of a track, shared by Record and CountFor. Returns false for
// tracks that should not be tracked (empty paths, internet streams).
bool RecordKey(const metadb_handle_ptr& track, std::string& key, std::string& path, bool& remote, int64_t& songId) {
    const char* rawPath = track->get_path();
    if (!rawPath || !*rawPath) return false;
    path = rawPath;
    songId = 0;
    remote = netease::ParseSongPath(rawPath, songId);
    if (remote) {
        key = "ncm:" + std::to_string(songId);
        return true;
    }
    if (path.starts_with("http://") || path.starts_with("https://")) return false;
    key = path + '\x1f' + std::to_string(track->get_subsong_index());
    return true;
}

}  // namespace

PlayStats& PlayStats::Get() {
    static PlayStats stats;
    return stats;
}

void PlayStats::Load() {
    m_loaded = true;
    std::ifstream in(StatsFile(), std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string_view> fields;
        size_t start = 0;
        while (true) {
            const size_t tab = line.find('\t', start);
            fields.emplace_back(line.data() + start,
                                (tab == std::string::npos ? line.size() : tab) - start);
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (fields.size() < 13) continue;
        PlayRecord record;
        record.count = static_cast<uint32_t>(ToInt(fields[0]));
        record.lastPlayed = ToInt(fields[1]);
        record.remote = ToInt(fields[2]) != 0;
        record.songId = ToInt(fields[3]);
        record.albumId = ToInt(fields[4]);
        record.artistId = ToInt(fields[5]);
        record.subsong = static_cast<uint32_t>(ToInt(fields[6]));
        record.durationMs = static_cast<int>(ToInt(fields[7]));
        record.key = Unescape(fields[8]);
        record.path = Unescape(fields[9]);
        record.title = Unescape(fields[10]);
        record.artist = Unescape(fields[11]);
        record.album = Unescape(fields[12]);
        if (record.key.empty()) continue;
        m_records.push_back(std::move(record));
    }
    Prune();
    RebuildIndex();
}

void PlayStats::Save() const {
    const auto file = StatsFile();
    if (file.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return;
    auto temp = file;
    temp += L".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "# glasswave play stats 1\n";
    for (const auto& record : m_records) {
        out << record.count << '\t' << record.lastPlayed << '\t' << (record.remote ? 1 : 0) << '\t'
            << record.songId << '\t' << record.albumId << '\t' << record.artistId << '\t' << record.subsong
            << '\t' << record.durationMs << '\t' << Escape(record.key) << '\t' << Escape(record.path) << '\t'
            << Escape(record.title) << '\t' << Escape(record.artist) << '\t' << Escape(record.album) << '\n';
    }
    out.close();
    MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void PlayStats::Prune() {
    if (m_records.size() <= kMaxRecords) return;
    std::stable_sort(m_records.begin(), m_records.end(),
                     [](const PlayRecord& a, const PlayRecord& b) { return a.lastPlayed < b.lastPlayed; });
    m_records.erase(m_records.begin(),
                    m_records.begin() + static_cast<std::ptrdiff_t>(m_records.size() - kMaxRecords));
}

void PlayStats::RebuildIndex() {
    m_index.clear();
    for (size_t i = 0; i < m_records.size(); ++i) m_index[m_records[i].key] = i;
}

void PlayStats::Record(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return;
    if (!m_loaded) Load();
    std::string key, path;
    bool remote = false;
    int64_t songId = 0;
    if (!RecordKey(track, key, path, remote, songId)) return;
    PlayRecord* record = nullptr;
    if (auto it = m_index.find(key); it != m_index.end()) record = &m_records[it->second];
    if (!record) {
        m_index[key] = m_records.size();
        m_records.push_back({});
        record = &m_records.back();
        record->key = key;
    }
    record->path = path;
    record->subsong = track->get_subsong_index();
    record->remote = remote;
    record->songId = remote ? songId : 0;
    record->albumId = 0;
    record->artistId = 0;
    if (remote) {
        if (const auto song = netease::Catalog::Get().FindSong(songId)) {
            record->title = Utf8(song->title);
            record->artist = Utf8(netease::JoinArtists(song->artists));
            record->album = Utf8(song->album);
            record->durationMs = song->durationMs;
            record->albumId = song->albumId;
            for (const auto& ref : song->artistRefs)
                if (ref.id) {
                    record->artistId = ref.id;
                    break;
                }
        }
    } else {
        record->title = LocalField(track, "%title%");
        if (record->title.empty()) record->title = LocalField(track, "%filename%");
        record->artist = LocalField(track, "%artist%");
        record->album = LocalField(track, "%album%");
        record->durationMs = static_cast<int>(ToInt(LocalField(track, "%length_seconds%")) * 1000);
    }
    ++record->count;
    record->lastPlayed = static_cast<int64_t>(time(nullptr));
    const size_t before = m_records.size();
    Prune();
    if (m_records.size() != before) RebuildIndex();
    Save();
}

uint32_t PlayStats::CountFor(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return 0;
    if (!m_loaded) Load();
    std::string key, path;
    bool remote = false;
    int64_t songId = 0;
    if (!RecordKey(track, key, path, remote, songId)) return 0;
    auto it = m_index.find(key);
    return it != m_index.end() ? m_records[it->second].count : 0;
}

void PlayStats::ForgetRecent(metadb_handle_list_cref items) {
    if (!m_loaded) Load();
    bool changed = false;
    for (size_t i = 0; i < items.get_count(); ++i) {
        const metadb_handle_ptr& track = items[i];
        if (!track.is_valid()) continue;
        std::string key, path;
        bool remote = false;
        int64_t songId = 0;
        if (!RecordKey(track, key, path, remote, songId)) continue;
        auto it = m_index.find(key);
        if (it == m_index.end()) continue;
        PlayRecord& record = m_records[it->second];
        if (record.lastPlayed) {
            record.lastPlayed = 0;
            changed = true;
        }
    }
    if (changed) Save();
}

void PlayStats::ClearCounts(metadb_handle_list_cref items) {
    if (!m_loaded) Load();
    bool changed = false;
    for (size_t i = 0; i < items.get_count(); ++i) {
        const metadb_handle_ptr& track = items[i];
        if (!track.is_valid()) continue;
        std::string key, path;
        bool remote = false;
        int64_t songId = 0;
        if (!RecordKey(track, key, path, remote, songId)) continue;
        auto it = m_index.find(key);
        if (it == m_index.end()) continue;
        PlayRecord& record = m_records[it->second];
        if (record.count) {
            record.count = 0;
            changed = true;
        }
    }
    if (changed) Save();
}

std::vector<PlayRecord> PlayStats::Recent(size_t limit) {
    if (!m_loaded) Load();
    std::vector<PlayRecord> out;
    out.reserve(m_records.size());
    for (const auto& record : m_records)
        if (record.lastPlayed > 0) out.push_back(record);  // ForgetRecent clears this
    std::sort(out.begin(), out.end(), [](const PlayRecord& a, const PlayRecord& b) {
        return a.lastPlayed != b.lastPlayed ? a.lastPlayed > b.lastPlayed : a.count > b.count;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<PlayRecord> PlayStats::MostPlayed(size_t limit) {
    if (!m_loaded) Load();
    std::vector<PlayRecord> out;
    out.reserve(m_records.size());
    for (const auto& record : m_records)
        if (record.count > 0) out.push_back(record);  // ClearCounts zeroes this
    std::sort(out.begin(), out.end(), [](const PlayRecord& a, const PlayRecord& b) {
        return a.count != b.count ? a.count > b.count : a.lastPlayed > b.lastPlayed;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace glasswave::media
