#include "pch.h"
#include "media/play_stats.h"
#include "core/log.h"
#include "core/file.h"
#include "core/text.h"
#include "core/paths.h"
#include "stream/provider.h"
#include "app/settings.h"
#include "cd/disc.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace glasswave::media {

namespace {

constexpr const char* kHeader = "# glasswave play stats 2";

size_t MaxRecords() { return static_cast<size_t>(settings::PlayHistoryLimit()); }

std::filesystem::path StatsFile() {
    const auto root = core::ProfileRoot();
    return root.empty() ? root : root / L"play-stats.txt";
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

using core::Utf8;

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

using core::Wide;

// The stable identity of a track, shared by Record and CountFor. Returns false for
// tracks that should not be tracked (empty paths, internet streams).
bool RecordKey(const metadb_handle_ptr& track, std::string& key, stream::Provider** provider = nullptr) {
    const char* rawPath = track->get_path();
    if (!rawPath || !*rawPath) return false;
    if (auto* owner = stream::ProviderOf(track)) {
        key = Utf8(std::wstring(owner->Key()) + L':' + owner->TrackId(track));
        if (provider) *provider = owner;
        return true;
    }
    const std::string_view path(rawPath);
    if (path.starts_with("http://") || path.starts_with("https://")) return false;
    key = std::string(path) + '\x1f' + std::to_string(track->get_subsong_index());
    return true;
}

}  // namespace

PlayStats& PlayStats::Get() {
    // Loaded once, on first use: CountFor is const and runs from paint code.
    static PlayStats stats = [] {
        PlayStats value;
        value.Load();
        return value;
    }();
    return stats;
}

void PlayStats::Load() {
    m_loaded = true;
    std::ifstream in(StatsFile(), std::ios::binary);
    if (!in) return;
    std::string line;
    if (!std::getline(in, line) || line != kHeader) return;  // another format: start over
    while (std::getline(in, line)) {
        std::vector<std::string_view> fields;
        size_t start = 0;
        while (true) {
            const size_t tab = line.find('\t', start);
            fields.emplace_back(line.data() + start, (tab == std::string::npos ? line.size() : tab) - start);
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (fields.size() < 13) continue;
        auto text = [](std::string_view field) { return Wide(Unescape(field)); };
        PlayRecord record;
        record.count = static_cast<uint32_t>(ToInt(fields[0]));
        record.lastPlayed = ToInt(fields[1]);
        record.provider = text(fields[2]);
        record.trackId = text(fields[3]);
        record.albumId = text(fields[4]);
        record.artistId = text(fields[5]);
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
    std::ostringstream out;
    auto text = [](const std::wstring& value) { return Escape(Utf8(value)); };
    out << kHeader << '\n';
    for (const auto& record : m_records) {
        out << record.count << '\t' << record.lastPlayed << '\t' << text(record.provider) << '\t'
            << text(record.trackId) << '\t' << text(record.albumId) << '\t' << text(record.artistId) << '\t'
            << record.subsong << '\t' << record.durationMs << '\t' << Escape(record.key) << '\t'
            << Escape(record.path) << '\t' << Escape(record.title) << '\t' << Escape(record.artist) << '\t'
            << Escape(record.album) << '\n';
    }
    try {
        core::WriteFileAtomic(file, out.str());
    } catch (const std::exception& e) {  // disk full or similar: the previous file stays
        core::Log(L"play stats: cannot save: " + core::Wide(e.what()));
    }
}

namespace {
constexpr double kSaveDelaySeconds = 5.0;
bool g_saveDirty = false;  // main thread
bool g_savePosted = false;
}  // namespace

void PlayStats::QueueSave() {
    g_saveDirty = true;
    if (g_savePosted) return;
    g_savePosted = true;
    fb2k::callLater(kSaveDelaySeconds, [] {
        g_savePosted = false;
        Flush();
    });
}

void PlayStats::Flush() {
    if (!g_saveDirty) return;
    g_saveDirty = false;
    Get().Save();
}

namespace {
class SaveAtQuit : public initquit {
public:
    void on_quit() override { PlayStats::Flush(); }
};
FB2K_SERVICE_FACTORY(SaveAtQuit);
}  // namespace

void PlayStats::Prune() {
    const size_t limit = MaxRecords();
    if (m_records.size() <= limit) return;
    std::stable_sort(m_records.begin(), m_records.end(),
                     [](const PlayRecord& a, const PlayRecord& b) { return a.lastPlayed < b.lastPlayed; });
    m_records.erase(m_records.begin(), m_records.begin() + static_cast<std::ptrdiff_t>(m_records.size() - limit));
}

void PlayStats::RebuildIndex() {
    m_index.clear();
    for (size_t i = 0; i < m_records.size(); ++i) m_index[m_records[i].key] = i;
}

void PlayStats::Record(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return;
    if (!m_loaded) Load();
    std::string key;
    stream::Provider* provider = nullptr;
    if (!RecordKey(track, key, &provider)) return;
    PlayRecord* record = nullptr;
    if (auto it = m_index.find(key); it != m_index.end()) record = &m_records[it->second];
    if (!record) {
        m_index[key] = m_records.size();
        m_records.push_back({});
        record = &m_records.back();
        record->key = key;
    }
    record->path = track->get_path();
    record->subsong = track->get_subsong_index();
    record->provider = provider ? std::wstring(provider->Key()) : std::wstring();
    record->trackId = provider ? provider->TrackId(track) : std::wstring();
    record->albumId.clear();
    record->artistId.clear();
    if (provider) {
        if (const auto song = provider->FindTrack(record->trackId)) {
            record->title = Utf8(song->title);
            record->artist = Utf8(stream::JoinArtists(song->artists));
            record->album = Utf8(song->album);
            record->durationMs = song->durationMs;
            record->albumId = song->albumId;
            for (const auto& artist : song->artists)
                if (!artist.id.empty()) {
                    record->artistId = artist.id;
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
    QueueSave();
}

uint32_t PlayStats::CountFor(const metadb_handle_ptr& track) const {
    if (!track.is_valid()) return 0;
    std::string key;
    if (!RecordKey(track, key)) return 0;
    auto it = m_index.find(key);
    return it != m_index.end() ? m_records[it->second].count : 0;
}

void PlayStats::ForgetRecent(metadb_handle_list_cref items) {
    if (!m_loaded) Load();
    bool changed = false;
    for (size_t i = 0; i < items.get_count(); ++i) {
        const metadb_handle_ptr& track = items[i];
        if (!track.is_valid()) continue;
        std::string key;
        if (!RecordKey(track, key)) continue;
        auto it = m_index.find(key);
        if (it == m_index.end()) continue;
        PlayRecord& record = m_records[it->second];
        if (record.lastPlayed) {
            record.lastPlayed = 0;
            changed = true;
        }
    }
    if (changed) QueueSave();
}

void PlayStats::ClearCounts(metadb_handle_list_cref items) {
    if (!m_loaded) Load();
    bool changed = false;
    for (size_t i = 0; i < items.get_count(); ++i) {
        const metadb_handle_ptr& track = items[i];
        if (!track.is_valid()) continue;
        std::string key;
        if (!RecordKey(track, key)) continue;
        auto it = m_index.find(key);
        if (it == m_index.end()) continue;
        PlayRecord& record = m_records[it->second];
        if (record.count) {
            record.count = 0;
            changed = true;
        }
    }
    if (changed) QueueSave();
}

void PlayStats::ForgetAllRecent() {
    if (!m_loaded) Load();
    bool changed = false;
    for (auto& record : m_records)
        if (record.lastPlayed) {
            record.lastPlayed = 0;
            changed = true;
        }
    if (changed) QueueSave();
}

void PlayStats::ClearAllCounts() {
    if (!m_loaded) Load();
    bool changed = false;
    for (auto& record : m_records)
        if (record.count) {
            record.count = 0;
            changed = true;
        }
    if (changed) QueueSave();
}

void PlayStats::Refresh(metadb_handle_list_cref items) {
    if (!m_loaded) Load();
    bool changed = false;
    for (const auto& track : items) {
        std::string key;
        stream::Provider* provider = nullptr;
        if (!track.is_valid() || !RecordKey(track, key, &provider) || provider) continue;
        const auto it = m_index.find(key);
        if (it == m_index.end()) continue;
        PlayRecord& record = m_records[it->second];
        std::string title = LocalField(track, "%title%");
        if (title.empty()) title = LocalField(track, "%filename%");
        std::string artist = LocalField(track, "%artist%"), album = LocalField(track, "%album%");
        if (title == record.title && artist == record.artist && album == record.album) continue;
        record.title = std::move(title);
        record.artist = std::move(artist);
        record.album = std::move(album);
        changed = true;
    }
    if (changed) QueueSave();
}

namespace {
// A CD's tracks play only while it is in a drive.
bool Playable(const PlayRecord& record) {
    return !cd::IsCdPath(record.path.c_str()) || cd::DiscPresent(cd::DiscKey(record.path.c_str()));
}
} // namespace

std::vector<PlayRecord> PlayStats::Recent(size_t limit) {
    if (!m_loaded) Load();
    std::vector<PlayRecord> out;
    out.reserve(m_records.size());
    for (const auto& record : m_records)
        if (record.lastPlayed > 0 && Playable(record)) out.push_back(record);  // ForgetRecent clears this
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
        if (record.count > 0 && Playable(record)) out.push_back(record);  // ClearCounts zeroes this
    std::sort(out.begin(), out.end(), [](const PlayRecord& a, const PlayRecord& b) {
        return a.count != b.count ? a.count > b.count : a.lastPlayed > b.lastPlayed;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace glasswave::media
