#include "pch.h"
#include "netease/catalog.h"
#include "core/text.h"

#include <SDK/file_info_impl.h>

#include <cstdlib>

namespace glasswave::netease {

namespace {

using core::Utf8;
using core::Wide;

constexpr char kFieldSeparator = '\x1e';
constexpr wchar_t kFieldWide = L'\x1e';
constexpr size_t kExtraFields = 4;

// ncm://song/<decimal id>; the id is the service's number.
std::string EncodeId(const stream::Id& id) {
    const int64_t number = Num(id);
    return number > 0 ? std::to_string(number) : std::string();
}

bool DecodeId(std::string_view segment, stream::Id& id) {
    if (segment.empty()) return false;
    const std::string copy(segment);
    char* end = nullptr;
    const long long parsed = std::strtoll(copy.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed <= 0) return false;
    id = std::to_wstring(parsed);
    return true;
}

int ToInt(const std::string& text) {
    return static_cast<int>(_strtoi64(text.c_str(), nullptr, 10));
}

} // namespace

Catalog::Catalog() : m_records(Traits(*this)) {}

stream::CatalogTraits Catalog::Traits(Catalog& self) {
    stream::CatalogTraits traits;
    traits.dir = L"netease";
    traits.scheme = "ncm";
    traits.encodeId = EncodeId;
    traits.decodeId = DecodeId;
    traits.cacheHeader = "# glasswave netease songs v1";
    traits.tags = [&self](const stream::Track& stored, file_info& info) { self.WriteTags(stored, info); };
    traits.encodeExtra = [&self](const stream::Id& id) { return self.EncodeExtra(id); };
    return traits;
}

Catalog& Catalog::Get() {
    static Catalog catalog;
    return catalog;
}

void Catalog::Put(const SongRecord& song) {
    Put(std::vector<SongRecord>{song});
}

void Catalog::Put(const std::vector<SongRecord>& songs) {
    std::vector<stream::TrackRecord> records;
    records.reserve(songs.size());
    std::lock_guard lock(m_mutex);
    for (const auto& song : songs) {
        if (song.track.id.empty()) continue;
        const auto known = m_tech.find(song.track.id);
        const bool retag = known == m_tech.end() || !(known->second == song.tech);
        m_tech[song.track.id] = song.tech;
        records.push_back({song.track, song.cover, retag});
    }
    m_records.Put(records);
}

void Catalog::PutAndHint(const std::vector<SongRecord>& songs, bool persist) {
    Put(songs);
    m_records.FlushHints(); // at once (main thread), with whatever else was waiting
    if (persist) m_records.QueueSave();
}

void Catalog::RestoreAndHint(const std::vector<stream::Track>& tracks) {
    std::vector<stream::TrackRecord> records;
    records.reserve(tracks.size());
    for (const auto& track : tracks)
        records.push_back({track, {}, false});
    m_records.RestoreAndHint(records);
}

std::optional<SongRecord> Catalog::FindSong(const stream::Id& id) const {
    auto track = m_records.FindTrack(id);
    if (!track) return std::nullopt;
    SongRecord song;
    song.track = std::move(*track);
    song.cover = m_records.CoverUrl(stream::ArtKind::Track, id);
    std::lock_guard lock(m_mutex);
    if (const auto tech = m_tech.find(id); tech != m_tech.end()) song.tech = tech->second;
    return song;
}

void Catalog::WriteTags(const stream::Track& stored, file_info& info) const {
    stream::WriteTrackTags(stored, info);
    SongTech tech;
    {
        std::lock_guard lock(m_mutex);
        if (const auto found = m_tech.find(stored.id); found != m_tech.end()) tech = found->second;
    }
    if (!tech.codec.empty()) info.info_set("codec", Utf8(tech.codec).c_str());
    if (tech.bitrate > 0) info.info_set_int("bitrate", (tech.bitrate + 500) / 1000); // kbps
    if (tech.sampleRate > 0) info.info_set_int("samplerate", tech.sampleRate);
    if (tech.bitsPerSample > 0) info.info_set_int("bitspersample", tech.bitsPerSample);
    info.info_set_int("channels", 2);
    if (tech.codec == L"FLAC") info.info_set("encoding", "lossless");
}

std::wstring Catalog::EncodeExtra(const stream::Id& id) const {
    SongTech tech;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_tech.find(id);
        if (found == m_tech.end()) return {};
        tech = found->second;
    }
    std::wstring codec = tech.codec;
    for (auto& c : codec)
        if (c == L'\t' || c == L'\r' || c == L'\n' || c == L'\x1e') c = L' ';
    return codec + kFieldWide + std::to_wstring(tech.bitrate) + kFieldWide +
           std::to_wstring(tech.sampleRate) + kFieldWide + std::to_wstring(tech.bitsPerSample);
}

void Catalog::Load() {
    std::vector<stream::Id> installed;
    {
        std::lock_guard lock(m_mutex);
        std::vector<stream::CachedTrack> songs = m_records.ReadCache();
        std::unordered_map<stream::Id, SongTech> tech;
        for (const auto& song : songs) {
            const std::string extra = Utf8(song.extra);
            std::vector<std::string> fields;
            size_t start = 0;
            while (start <= extra.size()) {
                const size_t end = extra.find(kFieldSeparator, start);
                fields.push_back(
                    extra.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            if (fields.size() != kExtraFields) continue;
            tech.emplace(song.track.id,
                         SongTech{Wide(fields[0]), ToInt(fields[1]), ToInt(fields[2]), ToInt(fields[3])});
        }
        installed = m_records.InstallMissing(std::move(songs));
        for (const auto& id : installed)
            if (const auto found = tech.find(id); found != tech.end()) m_tech[id] = found->second;
    }
    m_records.Hint(installed);
}

} // namespace glasswave::netease
