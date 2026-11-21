#include "pch.h"

#include "stream/track_snapshot.h"

#include "netease/profile.h"
#include "stream/provider.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace glasswave::stream {
namespace {

constexpr const char* kHeader = "glasswave track snapshot 1";
constexpr char kField = '\x1F';  // between a record's fields
constexpr char kValue = '\x1D';  // between a multi-value tag's values
constexpr double kSaveDelay = 3.0; // seconds after the last change

std::filesystem::path File() { return netease::ProfileDir().parent_path() / L"tracks" / L"snapshot.txt"; }

bool Streaming(const metadb_handle_ptr& item) {
    for (const Provider* provider : Providers())
        if (provider->Owns(item)) return true;
    return false;
}

std::string Escape(const char* text) {
    std::string out;
    for (const char* c = text; *c; ++c) {
        switch (*c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case kField: out += "\\f"; break;
        case kValue: out += "\\v"; break;
        default: out += *c;
        }
    }
    return out;
}

std::string Unescape(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            out += text[i];
            continue;
        }
        switch (text[++i]) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 'f': out += kField; break;
        case 'v': out += kValue; break;
        default: out += text[i];
        }
    }
    return out;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t at = text.find(separator); at != std::string_view::npos; at = text.find(separator, start)) {
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    parts.push_back(text.substr(start));
    return parts;
}

// path, subsong, length, then "m<name>=<value>[\x1D<value>...]" and "i<name>=<value>" fields.
void Write(std::string& out, const metadb_handle_ptr& item, const file_info& info) {
    out += Escape(item->get_path());
    out += kField;
    out += std::to_string(item->get_subsong_index());
    out += kField;
    char length[32];
    sprintf_s(length, "%.6f", info.get_length());
    out += length;
    for (t_size i = 0, count = info.meta_get_count(); i < count; ++i) {
        out += kField;
        out += 'm';
        out += Escape(info.meta_enum_name(i));
        out += '=';
        for (t_size v = 0, values = info.meta_enum_value_count(i); v < values; ++v) {
            if (v) out += kValue;
            out += Escape(info.meta_enum_value(i, v));
        }
    }
    for (t_size i = 0, count = info.info_get_count(); i < count; ++i) {
        out += kField;
        out += 'i';
        out += Escape(info.info_enum_name(i));
        out += '=';
        out += Escape(info.info_enum_value(i));
    }
    out += '\n';
}

bool Read(std::string_view line, std::string& path, t_uint32& subsong, file_info& info) {
    const auto fields = Split(line, kField);
    if (fields.size() < 3) return false;
    path = Unescape(fields[0]);
    subsong = static_cast<t_uint32>(std::strtoul(std::string(fields[1]).c_str(), nullptr, 10));
    info.set_length(std::strtod(std::string(fields[2]).c_str(), nullptr));
    for (size_t f = 3; f < fields.size(); ++f) {
        const std::string_view field = fields[f];
        const size_t equals = field.find('=');
        if (field.size() < 2 || equals == std::string_view::npos) continue;
        const std::string name = Unescape(field.substr(1, equals - 1));
        const std::string_view value = field.substr(equals + 1);
        if (field[0] == 'm') {
            for (const auto part : Split(value, kValue))
                info.meta_add(name.c_str(), Unescape(part).c_str());
        } else if (field[0] == 'i') {
            info.info_set(name.c_str(), Unescape(value).c_str());
        }
    }
    return !path.empty() && info.meta_exists("title");
}

// Every streaming song in a playlist, as metadb knows it now; written whole and swapped in.
void Save() {
    std::string out = std::string(kHeader) + "\n";
    std::unordered_set<const metadb_handle*> seen;
    auto manager = playlist_manager::get();
    for (t_size p = 0, playlists = manager->get_playlist_count(); p < playlists; ++p) {
        metadb_handle_list items;
        manager->playlist_get_all_items(p, items);
        for (t_size i = 0; i < items.get_count(); ++i) {
            const metadb_handle_ptr& item = items[i];
            if (!seen.insert(item.get_ptr()).second || !Streaming(item)) continue;
            metadb_info_container::ptr known;
            if (!item->get_info_ref(known)) continue;
            const file_info& info = known->info();
            if (!info.meta_exists("title") || info.info_exists(kPlaceholderInfo)) continue;
            Write(out, item, info);
        }
    }
    const auto file = File();
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    auto temp = file;
    temp += L".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(out.data(), static_cast<std::streamsize>(out.size()));
        stream.close();
        if (!stream) {
            std::filesystem::remove(temp, error);
            return;
        }
    }
    MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// At start-up, before anything formats or plays these songs. What a provider already put for a
// song wins (its tags are newer); the snapshot fills in the rest.
void Restore() {
    std::ifstream stream(File(), std::ios::binary);
    if (!stream) return;
    std::string line;
    if (!std::getline(stream, line) || line != kHeader) return;
    auto hints = metadb_io_v2::get()->create_hint_list();
    if (!hints.is_valid()) return;
    auto db = metadb::get();
    while (std::getline(stream, line)) {
        std::string path;
        t_uint32 subsong = 0;
        file_info_impl info;
        if (!Read(line, path, subsong, info)) continue;
        const metadb_handle_ptr item = db->handle_create(path.c_str(), subsong);
        metadb_info_container::ptr known;
        if (item->get_info_ref(known) && known->info().meta_exists("title") &&
            !known->info().info_exists(kPlaceholderInfo)) {
            const file_info& current = known->info();
            info.copy_meta(current);
            for (t_size i = 0, count = current.info_get_count(); i < count; ++i)
                info.info_set(current.info_enum_name(i), current.info_enum_value(i));
            if (current.get_length() > 0) info.set_length(current.get_length());
        }
        hints->add_hint(item, info, t_filestats(), true);
    }
    hints->on_done();
}

bool g_saveQueued = false;
bool g_running = false;

void SaveSoon() {
    if (!g_running || g_saveQueued) return;
    g_saveQueued = true;
    fb2k::callLater(kSaveDelay, [] {
        g_saveQueued = false;
        if (g_running) Save();
    });
}

class TrackInfoChanges : public metadb_io_callback {
  public:
    void on_changed_sorted(metadb_handle_list_cref items, bool fromHook) override {
        if (fromHook || !g_running) return;
        for (t_size i = 0; i < items.get_count(); ++i)
            if (Streaming(items[i])) {
                SaveSoon();
                return;
            }
    }
};

class PlaylistChanges : public playlist_callback_static {
  public:
    unsigned get_flags() override { return flag_on_items_added | flag_on_items_removed | flag_on_playlists_removed; }
    void on_items_added(t_size, t_size, metadb_handle_list_cref, const bit_array&) override { SaveSoon(); }
    void on_items_removed(t_size, const bit_array&, t_size, t_size) override { SaveSoon(); }
    void on_playlists_removed(const bit_array&, t_size, t_size) override { SaveSoon(); }
    void on_items_reordered(t_size, const t_size*, t_size) override {}
    void on_items_removing(t_size, const bit_array&, t_size, t_size) override {}
    void on_items_selection_change(t_size, const bit_array&, const bit_array&) override {}
    void on_item_focus_change(t_size, t_size, t_size) override {}
    void on_items_modified(t_size, const bit_array&) override {}
    void on_items_modified_fromplayback(t_size, const bit_array&, play_control::t_display_level) override {}
    void on_items_replaced(t_size, const bit_array&, const pfc::list_base_const_t<t_on_items_replaced_entry>&) override {}
    void on_item_ensure_visible(t_size, t_size) override {}
    void on_playlist_activate(t_size, t_size) override {}
    void on_playlist_created(t_size, const char*, t_size) override {}
    void on_playlists_removing(const bit_array&, t_size, t_size) override {}
    void on_playlists_reorder(const t_size*, t_size) override {}
    void on_playlist_renamed(t_size, const char*, t_size) override {}
    void on_default_format_changed() override {}
    void on_playback_order_changed(t_size) override {}
    void on_playlist_locked(t_size, bool) override {}
};

class SnapshotLifecycle : public initquit {
  public:
    void on_init() override {
        try {
            Restore();
        } catch (...) {
        }
        g_running = true;
    }
    void on_quit() override {
        g_running = false;
        try {
            Save();
        } catch (...) {
        }
    }
};

FB2K_SERVICE_FACTORY(TrackInfoChanges);
FB2K_SERVICE_FACTORY(PlaylistChanges);
FB2K_SERVICE_FACTORY(SnapshotLifecycle);

} // namespace

void HintTags(metadb_hint_list::ptr& hints, const metadb_handle_ptr& item, const file_info& tags) {
    file_info_impl merged;
    metadb_info_container::ptr known;
    if (item->get_info_ref(known)) merged.copy(known->info());
    merged.copy_meta(tags);
    if (tags.get_length() > 0) merged.set_length(tags.get_length());
    for (t_size i = 0, count = tags.info_get_count(); i < count; ++i)
        merged.info_set(tags.info_enum_name(i), tags.info_enum_value(i));
    merged.info_remove(kPlaceholderInfo); // real tags now
    hints->add_hint(item, merged, t_filestats(), true);
}

namespace {
constexpr const char* kTechFields[] = {"codec",         "codec_profile", "encoding", "bitrate",
                                       "samplerate",    "bitspersample", "channels", "channel_mode",
                                       "tool"};
} // namespace

void CopyTechInfo(const file_info& from, file_info& to) {
    for (const char* name : kTechFields)
        if (const char* value = from.info_get(name)) to.info_set(name, value);
}

void NoteTechInfo(const metadb_handle_ptr& item, const file_info& played) {
    if (!item.is_valid()) return;
    auto tech = std::make_shared<file_info_impl>();
    CopyTechInfo(played, *tech);
    if (tech->info_get_count() == 0) return;
    fb2k::inMainThread([item, tech] {
        file_info_impl merged;
        metadb_info_container::ptr known;
        if (item->get_info_ref(known)) {
            const file_info& current = known->info();
            bool same = true;
            for (t_size i = 0, count = tech->info_get_count(); i < count && same; ++i) {
                const char* value = current.info_get(tech->info_enum_name(i));
                same = value && strcmp(value, tech->info_enum_value(i)) == 0;
            }
            if (same) return; // already kept
            merged.copy(current);
        }
        CopyTechInfo(*tech, merged);
        auto hints = metadb_io_v2::get()->create_hint_list();
        if (!hints.is_valid()) return;
        hints->add_hint(item, merged, t_filestats(), true);
        hints->on_done();
    });
}

void KnownTechInfo(const metadb_handle_ptr& item, file_info& out) {
    metadb_info_container::ptr known;
    if (!item.is_valid() || !item->get_info_ref(known)) return;
    const file_info& info = known->info();
    for (const char* name : kTechFields)
        if (!out.info_get(name))
            if (const char* value = info.info_get(name)) out.info_set(name, value);
}

bool KnownTags(const metadb_handle_ptr& item, file_info& out) {
    metadb_info_container::ptr known;
    if (!item.is_valid() || !item->get_info_ref(known)) return false;
    const file_info& info = known->info();
    if (!info.meta_exists("title") || info.info_exists(kPlaceholderInfo)) return false;
    out.copy_meta(info);
    if (out.get_length() <= 0 && info.get_length() > 0) out.set_length(info.get_length());
    return true;
}

} // namespace glasswave::stream
