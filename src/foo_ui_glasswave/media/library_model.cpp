#include "pch.h"
#include "media/library_model.h"
#include "ui/text.h"

namespace glasswave::media {

namespace {

constexpr wchar_t kSep = L'\x1F';

// One titleformat pass per track, fields separated by U+001F.
constexpr const char* kTrackScript =
    "[$meta(album artist)]\x1f[%artist%]\x1f[%album%]\x1f[%genre%]\x1f[%title%]\x1f[%tracknumber%]\x1f"
    "[%length%]\x1f[$year(%date%)]\x1f$if2(%discnumber%,1)\x1f$directory_path(%path%)";

std::vector<std::wstring> Split(const std::wstring& s, size_t expected) {
    std::vector<std::wstring> out;
    out.reserve(expected);
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == kSep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    while (out.size() < expected) out.emplace_back();
    return out;
}

int NumberOf(const std::wstring& s) { return s.empty() ? 0 : _wtoi(s.c_str()); }

}  // namespace

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, out.data(), static_cast<int>(out.size()), out.data(),
                      static_cast<int>(out.size()), nullptr, nullptr, 0);
    return out;
}

bool MatchesFilter(std::wstring_view haystack, std::wstring_view filter) {
    // Every whitespace-separated word of the filter must appear somewhere.
    size_t i = 0;
    while (i < filter.size()) {
        while (i < filter.size() && filter[i] == L' ') ++i;
        size_t j = i;
        while (j < filter.size() && filter[j] != L' ') ++j;
        if (j > i && haystack.find(filter.substr(i, j - i)) == std::wstring_view::npos) return false;
        i = j;
    }
    return true;
}

void LibraryModel::Reload() {
    m_tracks.clear();
    m_artists.clear();
    m_albums.clear();
    m_genres.clear();

    auto lib = library_manager::get();
    m_enabled = lib->is_library_enabled();
    metadb_handle_list items;
    lib->get_all_items(items);

    titleformat_object::ptr script;
    titleformat_compiler::get()->compile_safe(script, kTrackScript);

    struct Sortable {
        Track track;
        int disc = 0, number = 0;
    };
    std::vector<Sortable> rows;
    rows.reserve(items.get_count());
    pfc::string8 buf;
    // The same file can be registered twice under differently-cased paths when a folder
    // was added twice; show it once.
    std::set<std::wstring> seen;
    // Image + cue rips often appear twice: the FLAC's embedded cue sheet and the external
    // .cue both expose the same tracks. Keep one per folder/album/disc/track/title.
    std::set<std::wstring> seenTracks;
    for (t_size i = 0, n = items.get_count(); i < n; ++i) {
        const auto& h = items[i];
        const std::wstring id = Lower(ui::Wide(h->get_path())) + L'|' + std::to_wstring(h->get_subsong_index());
        if (!seen.insert(id).second) continue;
        h->format_title(nullptr, buf, script, nullptr);
        auto f = Split(ui::Wide(buf), 10);
        Sortable s;
        s.track.handle = h;
        s.track.hasAlbumArtist = !f[0].empty();
        s.track.folder = Lower(f[9]);
        s.track.albumArtist = f[0].empty() ? (f[1].empty() ? L"Unknown Artist" : f[1]) : f[0];
        s.track.artist = f[1].empty() ? s.track.albumArtist : f[1];
        s.track.album = f[2].empty() ? L"Unknown Album" : f[2];
        s.track.genre = f[3].empty() ? L"Unknown Genre" : f[3];
        s.track.title = f[4];
        if (s.track.title.empty()) {
            pfc::string8 name = pfc::string_filename(h->get_path());
            s.track.title = ui::Wide(name);
        }
        s.track.number = f[5];
        s.track.length = f[6];
        s.track.year = (f[7].empty() || NumberOf(f[7]) <= 0) ? std::wstring() : f[7];
        s.disc = NumberOf(f[8]);
        s.number = NumberOf(f[5]);
        if (s.number > 0) {
            const std::wstring trackKey = s.track.folder + kSep + Lower(s.track.album) + kSep + std::to_wstring(s.disc) +
                                          kSep + std::to_wstring(s.number) + kSep + Lower(s.track.title);
            if (!seenTracks.insert(trackKey).second) continue;
        }
        s.track.search = Lower(s.track.artist + L' ' + s.track.albumArtist + L' ' + s.track.album + L' ' +
                               s.track.title + L' ' + s.track.genre);
        rows.push_back(std::move(s));
    }

    auto cmp = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                               static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr,
                               0) - 2;  // -1, 0, 1
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const Sortable& a, const Sortable& b) {
        if (int c = cmp(a.track.albumArtist, b.track.albumArtist)) return c < 0;
        if (int c = cmp(a.track.year, b.track.year)) return c < 0;
        if (int c = cmp(a.track.album, b.track.album)) return c < 0;
        if (a.disc != b.disc) return a.disc < b.disc;
        if (a.number != b.number) return a.number < b.number;
        return cmp(a.track.title, b.track.title) < 0;
    });
    m_tracks.reserve(rows.size());
    for (auto& r : rows) m_tracks.push_back(std::move(r.track));

    // Groupings keep library order inside each group (album / disc / track).
    auto build = [&](std::vector<Group>& out, auto keyOf, auto fill) {
        std::map<std::wstring, size_t> index;
        for (size_t i = 0; i < m_tracks.size(); ++i) {
            const std::wstring key = keyOf(m_tracks[i]);
            auto [it, inserted] = index.try_emplace(key, out.size());
            if (inserted) {
                Group g;
                g.key = key;
                fill(g, m_tracks[i]);
                out.push_back(std::move(g));
            }
            out[it->second].tracks.push_back(i);
        }
        std::stable_sort(out.begin(), out.end(), [&](const Group& a, const Group& b) {
            if (int c = cmp(a.name, b.name)) return c < 0;
            return cmp(a.secondary, b.secondary) < 0;
        });
        for (auto& g : out) g.search = Lower(g.name + L' ' + g.secondary);
    };
    // Tagging is often inconsistent inside one album: borrow the ALBUM ARTIST tag from any
    // track of the same album in the same folder.
    {
        std::map<std::wstring, std::wstring> tagByAlbumFolder;
        for (const auto& t : m_tracks)
            if (t.hasAlbumArtist) tagByAlbumFolder.try_emplace(Lower(t.album) + kSep + t.folder, t.albumArtist);
        for (auto& t : m_tracks) {
            if (t.hasAlbumArtist) continue;
            auto it = tagByAlbumFolder.find(Lower(t.album) + kSep + t.folder);
            if (it == tagByAlbumFolder.end()) continue;
            t.albumArtist = it->second;
            t.hasAlbumArtist = true;
        }
    }
    build(m_artists, [](const Track& t) { return t.albumArtist; },
          [](Group& g, const Track& t) { g.name = t.albumArtist; });
    // Albums without an ALBUM ARTIST tag (compilations) are keyed by folder so their
    // tracks stay together; their artist column shows the shared artist or "Various Artists".
    build(m_albums,
          [](const Track& t) { return t.album + kSep + (t.hasAlbumArtist ? t.albumArtist : t.folder); },
          [](Group& g, const Track& t) {
              g.name = t.album;
              g.secondary = t.albumArtist;
              g.year = t.year;
          });
    for (auto& g : m_albums) {
        if (g.tracks.empty() || m_tracks[g.tracks.front()].hasAlbumArtist) continue;
        const std::wstring& first = m_tracks[g.tracks.front()].artist;
        const bool same = std::all_of(g.tracks.begin(), g.tracks.end(),
                                      [&](size_t i) { return m_tracks[i].artist == first; });
        g.secondary = same ? first : L"Various Artists";
        g.search = Lower(g.name + L' ' + g.secondary);
        for (size_t i : g.tracks)
            if (g.year.empty()) g.year = m_tracks[i].year;
    }
    build(m_genres, [](const Track& t) { return t.genre; }, [](Group& g, const Track& t) { g.name = t.genre; });
}

const std::vector<Group>& LibraryModel::Groups(GroupKind kind) const {
    switch (kind) {
    case GroupKind::Artist: return m_artists;
    case GroupKind::Album: return m_albums;
    default: return m_genres;
    }
}

const Group* LibraryModel::FindGroup(GroupKind kind, std::wstring_view key) const {
    for (const auto& g : Groups(kind))
        if (g.key == key) return &g;
    return nullptr;
}

}  // namespace glasswave::media
