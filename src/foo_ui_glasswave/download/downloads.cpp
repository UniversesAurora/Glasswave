#include "pch.h"
#include "download/downloads.h"
#include "core/signal.h"
#include "core/log.h"
#include "core/file.h"
#include "core/text.h"
#include "core/paths.h"

#include "download/aria2.h"
#include "lyrics/lyrics_service.h"
#include "stream/provider.h"
#include "ui/text.h"

#include <SDK/album_art_helpers.h>

#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace glasswave::download {

namespace {

using core::Utf8;
using core::Wide;

constexpr size_t kWorkers = 10;         // threads; Preferences' "songs at once" limits the running ones
constexpr int kRetries = 3;             // renewed links per song before it fails
constexpr uint64_t kProgressMs = 250;   // progress notifications per song, at most
constexpr size_t kCoverLimit = 32 << 20; // a front cover larger than this is left out
constexpr const char* kHeader = "# glasswave downloads 1";

// {6B1E4A70-2C9D-4F3B-8E51-7A2D0C9F4B11}
cfg_var_modern::cfg_string cfg_folder({0x6b1e4a70, 0x2c9d, 0x4f3b, {0x8e, 0x51, 0x7a, 0x2d, 0x0c, 0x9f, 0x4b, 0x11}}, "");
// {6B1E4A71-2C9D-4F3B-8E51-7A2D0C9F4B11}
cfg_var_modern::cfg_int cfg_naming({0x6b1e4a71, 0x2c9d, 0x4f3b, {0x8e, 0x51, 0x7a, 0x2d, 0x0c, 0x9f, 0x4b, 0x11}}, 1);
// {6B1E4A72-2C9D-4F3B-8E51-7A2D0C9F4B11}
cfg_var_modern::cfg_int cfg_folders({0x6b1e4a72, 0x2c9d, 0x4f3b, {0x8e, 0x51, 0x7a, 0x2d, 0x0c, 0x9f, 0x4b, 0x11}}, 0);
// {6B1E4A75-2C9D-4F3B-8E51-7A2D0C9F4B11}
cfg_var_modern::cfg_int cfg_lyrics({0x6b1e4a75, 0x2c9d, 0x4f3b, {0x8e, 0x51, 0x7a, 0x2d, 0x0c, 0x9f, 0x4b, 0x11}}, 1);
// {6B1E4A73-2C9D-4F3B-8E51-7A2D0C9F4B11}
cfg_var_modern::cfg_int cfg_songsAtOnce({0x6b1e4a73, 0x2c9d, 0x4f3b, {0x8e, 0x51, 0x7a, 0x2d, 0x0c, 0x9f, 0x4b, 0x11}},
                                        kSongsAtOnce.defaultValue);
// {6B1E4A74-2C9D-4F3B-8E51-7A2D0C9F4B11}
cfg_var_modern::cfg_int cfg_connections({0x6b1e4a74, 0x2c9d, 0x4f3b, {0x8e, 0x51, 0x7a, 0x2d, 0x0c, 0x9f, 0x4b, 0x11}},
                                        kConnections.defaultValue);

struct Entry {
    Item item;
    stream::Provider* provider = nullptr;
    std::shared_ptr<abort_callback_impl> abort; // set while running
    bool cancel = false;   // Cancel while running: the worker removes it
    bool replace = false;  // Save as: the chosen file is replaced
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::vector<Entry> g_entries;
std::vector<std::thread> g_workers;
uint64_t g_nextId = 1;
bool g_loaded = false, g_quit = false;
bool g_posted = false, g_dirty = false; // a notification is on its way; the list needs saving
core::Signal<> g_listeners;
// Main thread: provider \x1f track -> the song's latest item, rebuilt with every notice (file
// existence only when the list itself changed).
std::unordered_map<std::wstring, SongState> g_index;
std::unordered_map<uint64_t, bool> g_fileThere;

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

Entry* Find(uint64_t id) {
    for (auto& e : g_entries)
        if (e.item.id == id) return &e;
    return nullptr;
}

bool Exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// ---- the list file ------------------------------------------------------------------------

std::filesystem::path ListFile() {
    const auto root = core::ProfileRoot();
    return root.empty() ? root : root / L"downloads.txt";
}

std::string Escape(const std::wstring& text) {
    std::string out;
    for (const char c : Utf8(text)) {
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

std::wstring Unescape(std::string_view text) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
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
    return Wide(out.c_str());
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        const size_t end = line.find('\t', start);
        parts.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

// One line per item: provider, track, title, artist, album, root, path, format, state,
// received, size, added, finished, error. A song that was running is saved as waiting.
void Save() {
    std::string text = std::string(kHeader) + "\n";
    {
        std::lock_guard lock(g_mutex);
        for (const auto& e : g_entries) {
            const Item& i = e.item;
            const State state = i.state == State::Running ? State::Waiting : i.state;
            for (const std::wstring* field : {&i.provider, &i.track, &i.title, &i.artist, &i.album, &i.root, &i.path,
                                              &i.format})
                text += Escape(*field) + '\t';
            text += std::to_string(static_cast<int>(state)) + '\t' + std::to_string(i.received) + '\t' +
                    std::to_string(i.size) + '\t' + std::to_string(i.added) + '\t' + std::to_string(i.finished) +
                    '\t' + Escape(i.error) + '\n';
        }
    }
    const auto file = ListFile();
    if (file.empty()) return;
    try {
        core::WriteFileAtomic(file, text);
    } catch (const std::exception& e) {  // the previous list stays
        core::Log(L"downloads: cannot save the list: " + Wide(e.what()));
    }
}

void Load() {
    const auto file = ListFile();
    if (file.empty()) return;
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::string line;
    if (!std::getline(in, line) || line != kHeader) return; // one current format; anything else starts empty
    std::lock_guard lock(g_mutex);
    while (std::getline(in, line)) {
        const auto parts = SplitTabs(line);
        if (parts.size() < 14) continue;
        Entry e;
        Item& i = e.item;
        std::wstring* fields[] = {&i.provider, &i.track, &i.title, &i.artist, &i.album, &i.root, &i.path, &i.format};
        for (size_t f = 0; f < std::size(fields); ++f)
            *fields[f] = Unescape(parts[f]);
        const int state = std::atoi(std::string(parts[8]).c_str());
        i.state = state >= 0 && state <= static_cast<int>(State::Done) ? static_cast<State>(state) : State::Failed;
        i.received = std::strtoull(std::string(parts[9]).c_str(), nullptr, 10);
        i.size = std::strtoull(std::string(parts[10]).c_str(), nullptr, 10);
        i.added = std::strtoll(std::string(parts[11]).c_str(), nullptr, 10);
        i.finished = std::strtoll(std::string(parts[12]).c_str(), nullptr, 10);
        i.error = Unescape(parts[13]);
        e.provider = stream::FindProvider(i.provider);
        if (!e.provider || i.track.empty()) continue;
        i.id = g_nextId++;
        g_entries.push_back(std::move(e));
    }
}

// ---- notifications --------------------------------------------------------------------------

std::wstring IndexKey(std::wstring_view provider, std::wstring_view track) {
    return std::wstring(provider) + L'\x1f' + std::wstring(track);
}


void RebuildIndex(bool checkFiles) {
    std::vector<Item> items;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& e : g_entries)
            items.push_back(e.item);
    }
    if (checkFiles) g_fileThere.clear();
    g_index.clear();
    for (const auto& item : items) {
        if (item.state == State::Done) {
            auto it = g_fileThere.find(item.id);
            if (it == g_fileThere.end()) it = g_fileThere.emplace(item.id, Exists(item.path)).first;
            if (!it->second) continue;
        }
        SongState state;
        state.id = item.id;
        state.state = item.state;
        state.progress = item.size ? static_cast<float>(static_cast<double>(item.received) / item.size) : 0.0f;
        g_index[IndexKey(item.provider, item.track)] = state; // the latest item wins
    }
}

// Tells the listeners on the main thread, once for any number of changes in between; `save`:
// the list itself changed and is written out.
void Changed(bool save) {
    {
        std::lock_guard lock(g_mutex);
        g_dirty = g_dirty || save;
        if (g_posted || g_quit) return;
        g_posted = true;
    }
    fb2k::inMainThread([] {
        bool save = false;
        {
            std::lock_guard lock(g_mutex);
            g_posted = false;
            save = std::exchange(g_dirty, false);
        }
        if (save) Save();
        RebuildIndex(save);
        g_listeners.Emit();
    });
}

// ---- file names -------------------------------------------------------------------------------

// A file or folder name: no characters Windows refuses, no trailing dots or spaces, no device
// names, at most 120 characters.
std::wstring Clean(std::wstring name) {
    for (auto& c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    if (name.size() > 120) name.resize(120);
    while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
        name.pop_back();
    while (!name.empty() && name.front() == L' ')
        name.erase(name.begin());
    if (name.empty()) return L"_";
    std::wstring upper = name.substr(0, name.find(L'.'));
    for (auto& c : upper)
        c = static_cast<wchar_t>(towupper(c));
    static const wchar_t* reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
                                        L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3",
                                        L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (const wchar_t* r : reserved)
        if (upper == r) return name + L"_";
    return name;
}

std::wstring Join(const std::vector<std::wstring>& names) {
    std::wstring out;
    for (const auto& name : names) {
        if (name.empty()) continue;
        if (!out.empty()) out += L", ";
        out += name;
    }
    return out;
}

// The file the naming rules give a song: <root>[\artist[\album]]\<name>.<ext>.
std::wstring PlannedPath(const Item& item, const stream::DownloadTags& tags, const std::string& extension) {
    std::wstring root = item.root.empty() ? Folder() : item.root;
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    const std::wstring title = tags.title.empty() ? item.title : tags.title;
    std::wstring artists = Join(tags.artists);
    if (artists.empty()) artists = item.artist;
    const std::wstring folderArtist =
        !tags.albumArtist.empty() ? tags.albumArtist : !tags.artists.empty() ? tags.artists.front() : item.artist;
    const std::wstring album = tags.album.empty() ? item.album : tags.album;
    std::wstring folder = root;
    const Folders folders = GetFolders();
    if (folders != Folders::None) folder += Clean(folderArtist.empty() ? L"Unknown artist" : folderArtist) + L'\\';
    if (folders == Folders::ArtistAlbum) folder += Clean(album.empty() ? L"Unknown album" : album) + L'\\';
    std::wstring name;
    switch (GetNaming()) {
    case Naming::Title: name = title; break;
    case Naming::ArtistTitle: name = artists.empty() ? title : artists + L" - " + title; break;
    case Naming::TitleArtist: name = artists.empty() ? title : title + L" - " + artists; break;
    }
    return folder + Clean(name) + L"." + Wide(extension.c_str());
}

// `path`, or "name (2).ext" and so on when a file or another download already has it.
std::wstring UniquePath(const std::wstring& path, uint64_t self) {
    const auto taken = [&](const std::wstring& candidate) {
        if (Exists(candidate) || Exists(candidate + L".part")) return true;
        for (const auto& e : g_entries)
            if (e.item.id != self && _wcsicmp(e.item.path.c_str(), candidate.c_str()) == 0) return true;
        return false;
    };
    if (!taken(path)) return path;
    const size_t dot = path.find_last_of(L'.');
    const std::wstring stem = path.substr(0, dot), ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    for (int n = 2;; ++n)
        if (const std::wstring candidate = stem + L" (" + std::to_wstring(n) + L")" + ext; !taken(candidate))
            return candidate;
}

// ---- tags -------------------------------------------------------------------------------------

std::vector<uint8_t> FetchCover(const std::wstring& url, abort_callback& abort) {
    auto request = http_client::get()->create_request("GET");
    file::ptr stream = request->run(Utf8(url).c_str(), abort);
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 64 * 1024> buffer{};
    while (true) {
        const t_size read = stream->read(buffer.data(), buffer.size(), abort);
        if (!read) break;
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
        if (bytes.size() > kCoverLimit) return {};
    }
    return bytes;
}

// The song's text tags replace whatever the file came with; the front cover is embedded.
// foobar2000's own writers do it, so every format it can tag works.
void WriteTags(const std::wstring& path, const stream::DownloadTags& tags, abort_callback& abort) {
    const std::string location = "file://" + Utf8(path);
    {
        service_ptr_t<input_info_writer> writer;
        input_entry::g_open_for_info_write(writer, nullptr, location.c_str(), abort);
        file_info_impl info;
        writer->get_info(0, info, abort);
        info.meta_remove_all();
        const auto add = [&](const char* field, const std::wstring& value) {
            if (!value.empty()) info.meta_add(field, Utf8(value).c_str());
        };
        add("title", tags.title);
        for (const auto& artist : tags.artists)
            add("artist", artist);
        add("album", tags.album);
        add("album artist", tags.albumArtist);
        if (tags.number > 0) add("tracknumber", std::to_wstring(tags.number));
        if (tags.disc > 0) add("discnumber", std::to_wstring(tags.disc));
        add("date", tags.date);
        for (const auto& genre : tags.genres)
            add("genre", genre);
        writer->set_info(0, info, abort);
        writer->commit(abort);
    }
    if (tags.coverUrl.empty()) return;
    const auto cover = FetchCover(tags.coverUrl, abort);
    if (cover.empty()) return;
    auto editor = album_art_editor::g_open(nullptr, location.c_str(), abort);
    editor->set(album_art_ids::cover_front, album_art_data_impl::g_create(cover.data(), cover.size()), abort);
    editor->commit(abort);
}

// ---- lyrics -----------------------------------------------------------------------------------

std::wstring LrcPath(const std::wstring& path) {
    std::filesystem::path lrc(path);
    lrc.replace_extension(L".lrc");
    return lrc.wstring();
}

// Sets (or, given no text, removes) the lyrics fields of a file through foobar2000's tag
// updater, which also deals with a file that is playing. MP3 keeps them in ID3's USLT frame
// ("UNSYNCED LYRICS" in foobar2000), the other formats in LYRICS.
class LyricsFilter : public file_info_filter {
  public:
    LyricsFilter(std::string field, std::string text) : m_field(std::move(field)), m_text(std::move(text)) {}
    bool apply_filter(trackRef, t_filestats, file_info& info) override {
        static const char* const fields[] = {"LYRICS", "UNSYNCED LYRICS", "UNSYNCEDLYRICS", "SYNCEDLYRICS"};
        bool changed = false; // the file already holds exactly these lyrics: nothing to rewrite
        for (const char* field : fields) {
            const char* value = info.meta_get(field, 0);
            const bool wanted = !m_text.empty() && _stricmp(field, m_field.c_str()) == 0;
            if (wanted ? !value || m_text != value || info.meta_get_count_by_name(field) != 1 : value != nullptr)
                changed = true;
        }
        if (!changed) return false;
        for (const char* field : fields)
            info.meta_remove_field(field);
        if (!m_text.empty()) info.meta_set(m_field.c_str(), m_text.c_str());
        return true;
    }

  private:
    std::string m_field, m_text;
};

void WriteLyrics(const std::wstring& path, const std::wstring& text) {
    const LyricsMode mode = GetLyricsMode();
    if (mode == LyricsMode::None || !Exists(path)) return;
    const bool embed = mode == LyricsMode::Embed || mode == LyricsMode::Both;
    const bool sidecar = mode == LyricsMode::File || mode == LyricsMode::Both;

    const std::wstring lrc = LrcPath(path);
    if (sidecar && !text.empty()) {
        const std::string bytes = "\xEF\xBB\xBF" + Utf8(text); // UTF-8 with a BOM, as players expect
        try {
            core::WriteFileAtomic(lrc, bytes);  // a failed write leaves an existing .lrc as it was
        } catch (const std::exception& e) {
            core::Log(L"downloads: cannot write the lyrics file: " + Wide(e.what()));
        }
    } else if (Exists(lrc)) {
        DeleteFileW(lrc.c_str()); // ours: the song's own name beside it
    }

    std::filesystem::path file(path);
    std::wstring extension = file.extension().wstring();
    for (auto& c : extension)
        c = static_cast<wchar_t>(towlower(c));
    const std::string field = extension == L".mp3" ? "UNSYNCED LYRICS" : "LYRICS";
    metadb_handle_list items;
    items.add_item(metadb::get()->handle_create(("file://" + Utf8(path)).c_str(), 0));
    metadb_io_v2::get()->update_info_async(
        items, fb2k::service_new<LyricsFilter>(field, embed ? Utf8(text) : std::string()), core_api::get_main_window(),
        metadb_io_v2::op_flag_no_errors | metadb_io_v2::op_flag_background | metadb_io_v2::op_flag_delay_ui, nullptr);
}

std::deque<uint64_t> g_lyricsQueue;
bool g_lyricsBusy = false;
std::set<uint64_t> g_lyricsLater; // changed lyrics, written after a pause
uint64_t g_lyricsLaterGeneration = 0;

// The next downloaded song's lyrics: what the lyrics panel would show for it, with its offset.
void PumpLyrics() {
    while (!g_lyricsBusy && !g_lyricsQueue.empty()) {
        const uint64_t id = g_lyricsQueue.front();
        g_lyricsQueue.pop_front();
        std::wstring path;
        stream::Provider* provider = nullptr;
        stream::Id track;
        {
            std::lock_guard lock(g_mutex);
            if (Entry* e = Find(id); e && e->item.state == State::Done) {
                path = e->item.path;
                provider = e->provider;
                track = e->item.track;
            }
        }
        if (!provider || GetLyricsMode() == LyricsMode::None) continue;
        const metadb_handle_ptr handle = provider->TrackItem(track);
        if (!handle.is_valid()) continue;
        g_lyricsBusy = true;
        lyrics::Service::Get().Lookup(handle, [handle, path](const lyrics::Result& result) {
            std::wstring text;
            if (result.status == lyrics::Status::Found && result.lyrics)
                text = lyrics::AddOffsetTag(result.lyrics->document,
                                            lyrics::Service::Get().Offset(handle, *result.lyrics));
            WriteLyrics(path, text);
            fb2k::inMainThread([] { // not inside Lookup's own call
                g_lyricsBusy = false;
                PumpLyrics();
            });
        });
    }
}

void QueueLyrics(uint64_t id) {
    if (std::find(g_lyricsQueue.begin(), g_lyricsQueue.end(), id) == g_lyricsQueue.end()) g_lyricsQueue.push_back(id);
    PumpLyrics();
}

// ---- workers ----------------------------------------------------------------------------------

// Progress of one song for the list (any worker).
void Progress(uint64_t id, uint64_t received, uint64_t size, double speed) {
    {
        std::lock_guard lock(g_mutex);
        if (Entry* e = Find(id)) {
            e->item.received = received;
            if (size) e->item.size = size;
            e->item.speed = speed;
        }
    }
    Changed(false);
}

// aria2's fetch over several connections into <part> (resumed from its control file). An
// error renews the link and continues, a few times.
void FetchAria2(uint64_t id, const stream::DownloadSource& source, const std::wstring& part, abort_callback& abort) {
    const std::filesystem::path file(part);
    const std::wstring dir = file.parent_path().wstring(), out = file.filename().wstring();
    stream::DownloadLink link = source.link(false, abort);
    std::string gid = aria2::Add(link, dir, out, Connections(), abort);
    int failures = 0;
    try {
        while (true) {
            abort.sleep(kProgressMs / 1000.0);
            const aria2::Status status = aria2::Tell(gid, abort);
            Progress(id, status.completed, status.total, static_cast<double>(status.speed));
            if (status.kind == aria2::Status::Kind::Complete) break;
            if (status.kind == aria2::Status::Kind::Error || status.kind == aria2::Status::Kind::Removed) {
                aria2::Remove(gid);
                if (++failures > kRetries)
                    throw pfc::exception(Utf8(status.error.empty() ? L"The download failed." : status.error).c_str());
                link = source.link(true, abort); // links expire
                gid = aria2::Add(link, dir, out, Connections(), abort);
            }
        }
    } catch (...) {
        aria2::Remove(gid); // stopped or failed: aria2 keeps the file and its control file
        throw;
    }
    aria2::Remove(gid);
    DeleteFileW((part + L".aria2").c_str());
}

// Downloads one song: resolve, name, fetch into <path>.part with aria2, then the final name and
// the tags.
void Run(uint64_t id, stream::Provider& provider, abort_callback& abort) {
    stream::Id track;
    {
        std::lock_guard lock(g_mutex);
        if (Entry* e = Find(id)) track = e->item.track;
    }
    stream::DownloadSource source = provider.ResolveDownload(track, abort);

    std::wstring path;
    bool replace = false;
    {
        std::lock_guard lock(g_mutex);
        Entry* e = Find(id);
        if (!e) return;
        e->item.format = source.format;
        if (!source.tags.title.empty()) e->item.title = source.tags.title;
        if (!source.tags.artists.empty()) e->item.artist = Join(source.tags.artists);
        if (!source.tags.album.empty()) e->item.album = source.tags.album;
        if (e->item.path.empty()) e->item.path = UniquePath(PlannedPath(e->item, source.tags, source.extension), id);
        if (e->replace) { // Save as: the chosen name with the file's own extension
            std::filesystem::path chosen(e->item.path);
            chosen.replace_extension(Wide(("." + source.extension).c_str()));
            e->item.path = chosen.wstring();
        }
        path = e->item.path;
        replace = e->replace;
    }
    Changed(true);

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::wstring part = path + L".part";
    std::wstring why;
    if (!aria2::Ensure(abort, &why))
        throw pfc::exception(Utf8(why.empty() ? std::wstring(L"The download engine cannot run.") : why).c_str());
    FetchAria2(id, source, part, abort);
    const uint64_t received = FileSize(part);
    if (!MoveFileExW(part.c_str(), path.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0))
        throw pfc::exception("Cannot rename the downloaded file.");
    try {
        WriteTags(path, source.tags, abort);
    } catch (const exception_aborted&) {
        throw;
    } catch (const std::exception& e) {
        FB2K_console_formatter() << "Glasswave: could not tag " << Utf8(path).c_str() << ": " << e.what();
    }
    {
        std::lock_guard lock(g_mutex);
        if (Entry* e = Find(id)) {
            e->item.state = State::Done;
            e->item.received = received;
            e->item.size = received;
            e->item.speed = 0;
            e->item.finished = Now();
            e->item.error.clear();
        }
    }
    Changed(true);
    fb2k::inMainThread([id] { QueueLyrics(id); });
}

void Worker() {
    while (true) {
        uint64_t id = 0;
        stream::Provider* provider = nullptr;
        std::shared_ptr<abort_callback_impl> abort;
        {
            std::unique_lock lock(g_mutex);
            Entry* next = nullptr;
            g_wake.wait(lock, [&] {
                if (g_quit) return true;
                int running = 0;
                for (const auto& e : g_entries)
                    running += e.item.state == State::Running;
                if (running >= SongsAtOnce()) return false;
                for (auto& e : g_entries)
                    if (e.item.state == State::Waiting) {
                        next = &e;
                        return true;
                    }
                return false;
            });
            if (g_quit) return;
            next->item.state = State::Running;
            next->item.speed = 0;
            next->item.error.clear();
            next->abort = abort = std::make_shared<abort_callback_impl>();
            id = next->item.id;
            provider = next->provider;
        }
        Changed(true);
        std::wstring error;
        bool aborted = false;
        try {
            Run(id, *provider, *abort);
        } catch (const exception_aborted&) {
            aborted = true;
        } catch (const std::exception& e) {
            error = Wide(e.what());
            if (error.empty()) error = L"The download failed.";
        }
        std::wstring removePart;
        {
            std::lock_guard lock(g_mutex);
            Entry* e = Find(id);
            if (!e) continue;
            e->abort.reset();
            e->item.speed = 0;
            if (e->cancel) { // Cancel: the item goes, with its partial file
                if (!e->item.path.empty()) removePart = e->item.path + L".part";
                g_entries.erase(g_entries.begin() + (e - g_entries.data()));
            } else if (aborted) {
                if (e->item.state == State::Running) e->item.state = State::Waiting; // shutting down
            } else if (!error.empty() && e->item.state == State::Running) { // not a pause that broke a read
                e->item.state = State::Failed;
                e->item.error = std::move(error);
            }
        }
        if (!removePart.empty()) {
            DeleteFileW(removePart.c_str());
            DeleteFileW((removePart + L".aria2").c_str());
        }
        g_wake.notify_all(); // a slot is free
        Changed(true);
    }
}

void EnsureWorkers() {
    if (!g_workers.empty() || g_quit) return;
    for (size_t i = 0; i < kWorkers; ++i)
        g_workers.emplace_back(Worker);
}

void EnsureLoaded() {
    if (g_loaded) return;
    g_loaded = true;
    Load();
    RebuildIndex(true);
    bool waiting = false;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& e : g_entries)
            waiting = waiting || e.item.state == State::Waiting;
    }
    if (waiting) EnsureWorkers();
}

std::wstring DefaultFolder() {
    PWSTR music = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &music))) folder = music;
    CoTaskMemFree(music);
    return folder.empty() ? std::wstring() : folder + L"\\Glasswave";
}

void Shutdown() {
    std::vector<std::shared_ptr<abort_callback_impl>> running;
    {
        std::lock_guard lock(g_mutex);
        g_quit = true;
        for (auto& e : g_entries)
            if (e.abort) running.push_back(e.abort);
    }
    for (auto& abort : running)
        abort->abort();
    g_wake.notify_all();
    for (auto& worker : g_workers)
        worker.join();
    g_workers.clear();
    aria2::Shutdown();
    if (g_loaded) Save();
}

class Lifecycle : public initquit {
  public:
    void on_init() override {
        EnsureLoaded();
        // Another lyric for a song: its downloaded files follow.
        lyrics::Service::Get().SubscribeChanges([](const metadb_handle_ptr& track) {
            stream::Provider* provider = track.is_valid() ? stream::ProviderOf(track) : nullptr;
            if (!provider) return;
            const stream::Id id = provider->TrackId(track);
            std::vector<uint64_t> ids;
            {
                std::lock_guard lock(g_mutex);
                for (const auto& e : g_entries)
                    if (e.item.state == State::Done && e.item.provider == provider->Key() && e.item.track == id)
                        ids.push_back(e.item.id);
            }
            // Offsets change a step at a time: the files follow once the steps have stopped.
            g_lyricsLater.insert(ids.begin(), ids.end());
            const uint64_t generation = ++g_lyricsLaterGeneration;
            fb2k::callLater(1.5, [generation] {
                if (generation != g_lyricsLaterGeneration) return;
                for (const uint64_t item : std::exchange(g_lyricsLater, {}))
                    QueueLyrics(item);
            });
        });
    }
    void on_quit() override { Shutdown(); }
};

FB2K_SERVICE_FACTORY(Lifecycle);

} // namespace

std::wstring Folder() {
    const std::wstring folder = Wide(cfg_folder.get());
    return folder.empty() ? DefaultFolder() : folder;
}
void SetFolder(const std::wstring& folder) { cfg_folder.set(Utf8(folder).c_str()); }
Naming GetNaming() { return static_cast<Naming>(std::clamp<int64_t>(cfg_naming.get(), 0, 2)); }
void SetNaming(Naming naming) { cfg_naming.set(static_cast<int64_t>(naming)); }
Folders GetFolders() { return static_cast<Folders>(std::clamp<int64_t>(cfg_folders.get(), 0, 2)); }
void SetFolders(Folders folders) { cfg_folders.set(static_cast<int64_t>(folders)); }
LyricsMode GetLyricsMode() { return static_cast<LyricsMode>(std::clamp<int64_t>(cfg_lyrics.get(), 0, 3)); }
void SetLyricsMode(LyricsMode mode) {
    if (mode == GetLyricsMode()) return;
    cfg_lyrics.set(static_cast<int64_t>(mode));
    std::vector<uint64_t> ids; // every downloaded file follows the new setting
    {
        std::lock_guard lock(g_mutex);
        for (const auto& e : g_entries)
            if (e.item.state == State::Done) ids.push_back(e.item.id);
    }
    for (const uint64_t id : ids)
        QueueLyrics(id);
}

int SongsAtOnce() {
    return static_cast<int>(std::clamp<int64_t>(cfg_songsAtOnce.get(), kSongsAtOnce.min, kSongsAtOnce.max));
}
void SetSongsAtOnce(int songs) {
    cfg_songsAtOnce.set(std::clamp(songs, kSongsAtOnce.min, kSongsAtOnce.max));
    g_wake.notify_all(); // more may start now
}
int Connections() {
    return static_cast<int>(std::clamp<int64_t>(cfg_connections.get(), kConnections.min, kConnections.max));
}
void SetConnections(int connections) { cfg_connections.set(std::clamp(connections, kConnections.min, kConnections.max)); }
std::wstring Engine() {
    const std::wstring version = aria2::Version();
    return version.empty() ? std::wstring(L"aria2 (starts with the first download)") : L"aria2 " + version;
}

std::vector<Item> Items() {
    EnsureLoaded();
    std::lock_guard lock(g_mutex);
    std::vector<Item> items;
    items.reserve(g_entries.size());
    for (const auto& e : g_entries)
        items.push_back(e.item);
    return items;
}

std::optional<SongState> StateOf(const metadb_handle_ptr& item) {
    stream::Provider* provider = item.is_valid() ? stream::ProviderOf(item) : nullptr;
    if (!provider) return std::nullopt;
    const auto it = g_index.find(IndexKey(provider->Key(), provider->TrackId(item)));
    if (it == g_index.end()) return std::nullopt;
    return it->second;
}

bool Downloadable(const metadb_handle_ptr& item) {
    stream::Provider* provider = item.is_valid() ? stream::ProviderOf(item) : nullptr;
    return provider && provider->Can(stream::Capability::Download);
}

bool AnyDownloadable(const metadb_handle_list& items) {
    for (size_t i = 0; i < items.get_count(); ++i)
        if (Downloadable(items[i])) return true;
    return false;
}

namespace {

// A new item for a song (main thread); nullopt when its source cannot download it.
std::optional<Entry> NewEntry(const metadb_handle_ptr& handle) {
    stream::Provider* provider = handle.is_valid() ? stream::ProviderOf(handle) : nullptr;
    if (!provider || !provider->Can(stream::Capability::Download)) return std::nullopt;
    const stream::Id track = provider->TrackId(handle);
    if (track.empty()) return std::nullopt;
    Entry e;
    e.provider = provider;
    e.item.provider = std::wstring(provider->Key());
    e.item.track = track;
    if (const auto known = provider->FindTrack(track)) {
        e.item.title = known->title;
        e.item.artist = stream::JoinArtists(known->artists);
        e.item.album = known->album;
    }
    e.item.added = Now();
    return e;
}

} // namespace

size_t Add(const metadb_handle_list& items, const std::wstring& root) {
    EnsureLoaded();
    aria2::RetryAfterFailure();
    size_t added = 0;
    {
        std::lock_guard lock(g_mutex);
        for (size_t i = 0; i < items.get_count(); ++i) {
            auto entry = NewEntry(items[i]);
            if (!entry) continue;
            // Already queued, or downloaded and still there: nothing to do. A finished song
            // whose file is gone is fetched again.
            bool skip = false;
            for (auto it = g_entries.begin(); it != g_entries.end();) {
                if (it->item.provider == entry->item.provider && it->item.track == entry->item.track && root.empty()) {
                    if (it->item.state != State::Done || Exists(it->item.path)) {
                        skip = true;
                        break;
                    }
                    it = g_entries.erase(it);
                    continue;
                }
                ++it;
            }
            if (skip) continue;
            entry->item.root = root;
            entry->item.id = g_nextId++;
            g_entries.push_back(std::move(*entry));
            ++added;
        }
    }
    if (added) {
        EnsureWorkers();
        g_wake.notify_all();
        Changed(true);
    }
    return added;
}

bool AddAs(const metadb_handle_ptr& item, const std::wstring& path) {
    EnsureLoaded();
    aria2::RetryAfterFailure();
    auto entry = NewEntry(item);
    if (!entry || path.empty()) return false;
    entry->item.path = path;
    entry->replace = true;
    {
        std::lock_guard lock(g_mutex);
        entry->item.id = g_nextId++;
        g_entries.push_back(std::move(*entry));
    }
    EnsureWorkers();
    g_wake.notify_all();
    Changed(true);
    return true;
}

void Pause(const std::vector<uint64_t>& ids) {
    std::vector<std::shared_ptr<abort_callback_impl>> stop;
    {
        std::lock_guard lock(g_mutex);
        for (const uint64_t id : ids)
            if (Entry* e = Find(id); e && (e->item.state == State::Waiting || e->item.state == State::Running)) {
                e->item.state = State::Paused;
                if (e->abort) stop.push_back(e->abort);
            }
    }
    for (auto& abort : stop)
        abort->abort();
    Changed(true);
}

void Resume(const std::vector<uint64_t>& ids) {
    aria2::RetryAfterFailure();
    {
        std::lock_guard lock(g_mutex);
        for (const uint64_t id : ids)
            if (Entry* e = Find(id); e && (e->item.state == State::Paused || e->item.state == State::Failed)) {
                e->item.state = State::Waiting;
                e->item.error.clear();
            }
    }
    EnsureWorkers();
    g_wake.notify_all();
    Changed(true);
}

void Cancel(const std::vector<uint64_t>& ids) {
    std::vector<std::shared_ptr<abort_callback_impl>> stop;
    std::vector<std::wstring> parts;
    {
        std::lock_guard lock(g_mutex);
        for (const uint64_t id : ids) {
            Entry* e = Find(id);
            if (!e || e->item.state == State::Done) continue;
            if (e->abort) { // the worker removes it once it has stopped writing
                e->cancel = true;
                stop.push_back(e->abort);
                continue;
            }
            if (!e->item.path.empty()) parts.push_back(e->item.path + L".part");
            g_entries.erase(g_entries.begin() + (e - g_entries.data()));
        }
    }
    for (auto& abort : stop)
        abort->abort();
    for (const auto& part : parts) {
        DeleteFileW(part.c_str());
        DeleteFileW((part + L".aria2").c_str());
    }
    Changed(true);
}

void Forget(const std::vector<uint64_t>& ids, bool deleteFiles) {
    std::wstring files; // double-null-terminated, as SHFileOperation wants
    {
        std::lock_guard lock(g_mutex);
        for (const uint64_t id : ids) {
            Entry* e = Find(id);
            if (!e || e->item.state != State::Done) continue;
            if (deleteFiles && Exists(e->item.path)) files += e->item.path + L'\0';
            if (deleteFiles && Exists(LrcPath(e->item.path))) files += LrcPath(e->item.path) + L'\0';
            g_entries.erase(g_entries.begin() + (e - g_entries.data()));
        }
    }
    if (!files.empty()) {
        files += L'\0';
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = files.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        SHFileOperationW(&op);
    }
    Changed(true);
}

std::optional<std::wstring> LocalFile(std::wstring_view provider, std::wstring_view track) {
    std::wstring path;
    {
        std::lock_guard lock(g_mutex);
        for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it)
            if (it->item.state == State::Done && it->item.provider == provider && it->item.track == track) {
                path = it->item.path;
                break;
            }
    }
    if (path.empty() || !Exists(path)) return std::nullopt;
    return path;
}

uint64_t Subscribe(std::function<void()> listener) { return g_listeners.Add(std::move(listener)); }

void Unsubscribe(uint64_t token) { g_listeners.Remove(token); }

} // namespace glasswave::download
