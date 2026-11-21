#include "pch.h"
#include "cd/musicbrainz.h"
#include "core/text.h"

#include "cd/discs.h"

#include "media/play_stats.h"
#include "net/http.h"
#include "core/json.h"
#include "core/log.h"
#include "ui/text.h"

#include <SDK/initquit.h>

#include <chrono>
#include <map>
#include <mutex>
#include <set>

namespace glasswave::cd {

namespace json = core::json;
using core::Log;
using core::Utf8;
using core::Wide;

namespace {

// MusicBrainz asks clients to name themselves and to stay at one request a second.
constexpr const wchar_t* kUserAgent = L"Glasswave/0.1 (foobar2000 user interface component)";
constexpr auto kRequestGap = std::chrono::milliseconds(1100);

struct Known {
    Toc toc;
    std::shared_ptr<const Release> release; // null: MusicBrainz has none
};

std::mutex g_mutex;                   // g_known
std::map<std::string, Known> g_known; // by foobar2000 disc path
std::mutex g_lookupMutex;             // one lookup at a time, at MusicBrainz's pace
std::chrono::steady_clock::time_point g_lastRequest;
// Aborted on shutdown. Built on first use: foobar2000's abort objects need the core, which a
// global would ask for while the DLL loads (the component then fails to load).
abort_callback_impl& Abort() {
    static abort_callback_impl abort;
    return abort;
}
std::set<std::string> g_tagging; // main thread: discs being looked up for tags

std::wstring Credit(const json::Object& object) {
    std::wstring text;
    if (auto credits = json::GetArray(object, L"artist-credit"))
        for (const auto& credit : json::Objects(*credits))
            text += json::GetString(credit, L"name") + json::GetString(credit, L"joinphrase");
    return text;
}

enum class Answer {
    Found,
    None,
    Failed
};

// One request to the web service: Found with a body, None for a 404, Failed otherwise.
Answer Fetch(const std::wstring& url, abort_callback& abort, std::string& body) {
    net::HttpOptions options;
    options.headers = {{L"User-Agent", kUserAgent}, {L"Accept", L"application/json"}};
    options.timeoutMs = 15000;
    net::HttpResponse response;
    // 503 is MusicBrainz's rate limit: wait a little longer each time and ask again.
    constexpr double kBusyWaits[] = {0.0, 1.5, 3.0, 6.0};
    for (const double busyWait : kBusyWaits) {
        if (busyWait > 0) abort.sleep(busyWait);
        const auto wait = g_lastRequest + kRequestGap - std::chrono::steady_clock::now();
        if (wait > std::chrono::steady_clock::duration::zero())
            abort.sleep(std::chrono::duration<double>(wait).count());
        response = net::SendHttp(url, options, abort);
        g_lastRequest = std::chrono::steady_clock::now();
        if (response.status != 503) break;
    }
    if (response.status == 404) return Answer::None;
    if (!response.ok()) {
        Log(L"cd: MusicBrainz request failed (" + std::to_wstring(response.status) + L" " + response.error +
            L"): " + url);
        return Answer::Failed;
    }
    body = response.body;
    return Answer::Found;
}

// The release and medium for the disc among a lookup's releases: the medium carrying the disc
// id (an exact lookup), else one with as many tracks (a TOC lookup); a release with a cover
// before one without.
std::shared_ptr<Release> Choose(const json::Object& root, const Toc& toc, const std::wstring& discId) {
    const auto releases = json::GetArray(root, L"releases");
    if (!releases) return nullptr;
    const size_t trackCount = toc.offsets.size();
    std::shared_ptr<Release> best;
    int bestScore = -1;
    for (const auto& release : json::Objects(*releases)) {
        const auto media = json::GetArray(release, L"media");
        if (!media) continue;
        const auto mediaList = json::Objects(*media);
        for (const auto& medium : mediaList) {
            bool exact = false;
            if (auto discs = json::GetArray(medium, L"discs"))
                for (const auto& disc : json::Objects(*discs))
                    exact = exact || json::GetString(disc, L"id") == discId;
            const auto tracks = json::GetArray(medium, L"tracks");
            if (!tracks || (!exact && tracks->Size() != trackCount)) continue;
            const bool cover = [&] {
                const auto art = json::GetObject(release, L"cover-art-archive");
                return art && json::GetBool(*art, L"front");
            }();
            const int score = (exact ? 2 : 0) + (cover ? 1 : 0);
            if (score <= bestScore) continue;
            auto chosen = std::make_shared<Release>();
            chosen->id = json::GetString(release, L"id");
            chosen->title = json::GetString(release, L"title");
            chosen->artist = Credit(release);
            chosen->date = json::GetString(release, L"date");
            chosen->discId = discId;
            chosen->disc = json::GetInt(medium, L"position", 1);
            chosen->discs = static_cast<int>(mediaList.size());
            chosen->frontCover = cover;
            for (const auto& track : json::Objects(*tracks)) {
                ReleaseTrack entry;
                entry.title = json::GetString(track, L"title");
                entry.artist = Credit(track);
                entry.trackId = json::GetString(track, L"id");
                if (auto recording = json::GetObject(track, L"recording")) {
                    entry.recordingId = json::GetString(*recording, L"id");
                    if (entry.title.empty()) entry.title = json::GetString(*recording, L"title");
                    if (entry.artist.empty()) entry.artist = Credit(*recording);
                }
                if (entry.artist.empty()) entry.artist = chosen->artist;
                chosen->tracks.push_back(std::move(entry));
            }
            best = std::move(chosen);
            bestScore = score;
        }
    }
    return best;
}

// Found/None as answered by MusicBrainz, Failed when it could not be asked.
Answer Query(const Toc& toc, abort_callback& abort, std::shared_ptr<const Release>& out) {
    const std::wstring discId = MusicBrainzDiscId(toc);
    if (discId.empty()) return Answer::Failed;
    const std::wstring base = L"https://musicbrainz.org/ws/2/discid/";
    const std::wstring inc = L"inc=artist-credits+recordings&fmt=json";
    std::string body;
    Answer answer = Fetch(base + discId + L"?" + inc, abort, body);
    if (answer == Answer::None) // not submitted: releases with the same track layout
        answer = Fetch(base + L"-?toc=" + TocQuery(toc) + L"&" + inc, abort, body);
    if (answer != Answer::Found) return answer;
    const auto root = json::Parse(body);
    if (!root) return Answer::Failed;
    out = Choose(*root, toc, discId);
    Log(L"cd: disc " + discId + (out ? L" is \"" + out->title + L"\" (" + out->id + L")" : L" is unknown"));
    return out ? Answer::Found : Answer::None;
}

std::string DiscPath(const metadb_handle_ptr& track) {
    return track.is_valid() ? DiscKey(track->get_path()) : std::string();
}

void Put(file_info& info, const char* field, const std::wstring& value) {
    if (value.empty())
        info.meta_remove_field(field);
    else
        info.meta_set(field, Utf8(value).c_str());
}

class TagFilter : public file_info_filter {
  public:
    TagFilter(std::shared_ptr<const Release> release, int firstTrack)
        : m_release(std::move(release)), m_first(firstTrack) {}

    bool apply_filter(trackRef location, t_filestats, file_info& info) override {
        if (info.meta_get("ALBUM", 0)) return false; // tagged meanwhile (by the owner)
        const int number = static_cast<int>(TrackNumber(location->get_path(), location->get_subsong_index()));
        const int index = number - m_first;
        if (index < 0 || static_cast<size_t>(index) >= m_release->tracks.size()) return false;
        const Release& r = *m_release;
        const ReleaseTrack& t = r.tracks[static_cast<size_t>(index)];
        Put(info, "TITLE", t.title);
        Put(info, "ARTIST", t.artist);
        Put(info, "ALBUM", r.title);
        Put(info, "ALBUM ARTIST", r.artist);
        Put(info, "DATE", r.date);
        Put(info, "TRACKNUMBER", std::to_wstring(number));
        Put(info, "TOTALTRACKS", std::to_wstring(r.tracks.size()));
        Put(info, "DISCNUMBER", r.discs > 1 ? std::to_wstring(r.disc) : std::wstring());
        Put(info, "TOTALDISCS", r.discs > 1 ? std::to_wstring(r.discs) : std::wstring());
        Put(info, "MUSICBRAINZ_ALBUMID", r.id);
        Put(info, "MUSICBRAINZ_DISCID", r.discId);
        Put(info, "MUSICBRAINZ_TRACKID", t.recordingId);
        Put(info, "MUSICBRAINZ_RELEASETRACKID", t.trackId);
        return true;
    }

  private:
    std::shared_ptr<const Release> m_release;
    int m_first;
};

class Lifecycle : public initquit {
  public:
    void on_init() override {}
    void on_quit() override { Abort().abort(); }
};
initquit_factory_t<Lifecycle> g_lifecycle;

} // namespace

std::shared_ptr<const Release> LookupDisc(const metadb_handle_ptr& track, abort_callback& abort) {
    const std::string path = DiscPath(track);
    if (path.empty()) return nullptr;
    const auto known = [&path]() -> std::optional<Known> {
        std::lock_guard lock(g_mutex);
        const auto found = g_known.find(path);
        return found == g_known.end() ? std::nullopt : std::optional<Known>(found->second);
    };
    if (auto entry = known()) return entry->release;

    std::lock_guard lookup(g_lookupMutex);
    if (auto entry = known()) return entry->release; // answered while this one waited
    const auto toc =
        FindDisc(TrackNumber(track->get_path(), track->get_subsong_index()), track->get_length());
    if (!toc) return nullptr;
    std::shared_ptr<const Release> release;
    if (Query(*toc, abort, release) == Answer::Failed) return nullptr;
    std::lock_guard lock(g_mutex);
    g_known[path] = Known{*toc, release};
    return release;
}

std::optional<Toc> KnownDisc(const std::string& path) {
    std::lock_guard lock(g_mutex);
    const auto found = g_known.find(path);
    return found == g_known.end() ? std::nullopt : std::optional<Toc>(found->second.toc);
}

namespace {
// A lookup that failed (MusicBrainz busy, no network) is tried again this many times, this far apart.
constexpr int kTagAttempts = 4;
constexpr double kTagRetrySeconds = 20.0;

void TagDisc(const std::string& path, const metadb_handle_list& tracks, int attempt);
} // namespace

void TagDiscs(const metadb_handle_list& items) {
    std::map<std::string, metadb_handle_list> discs;
    for (const auto& track : items) {
        if (!track.is_valid() || !IsCdPath(track->get_path())) continue;
        const auto info = track->get_info_ref();
        // foobar2000 names an untagged CD's tracks "CD Track 01": a disc is tagged once it has an album.
        if (info.is_valid() && info->info().meta_get("ALBUM", 0)) continue;
        discs[DiscKey(track->get_path())].add_item(track);
    }
    for (auto& [path, tracks] : discs)
        if (g_tagging.insert(path).second) TagDisc(path, tracks, 1);
}

namespace {
void TagDisc(const std::string& path, const metadb_handle_list& tracks, int attempt) {
    fb2k::splitTask([path, tracks, attempt] {
        std::shared_ptr<const Release> release;
        try {
            release = LookupDisc(tracks[0], Abort());
        } catch (...) {
        }
        const auto toc = KnownDisc(path); // set once MusicBrainz answered, a release or none
        fb2k::inMainThread([path, tracks, release, attempt, answered = toc.has_value(),
                            first = toc ? toc->first : 1] {
            if (Abort().is_aborting()) {
                g_tagging.erase(path);
                return;
            }
            if (!answered && attempt < kTagAttempts) { // the lookup failed: later, once more
                fb2k::callLater(kTagRetrySeconds, [path, tracks, attempt] {
                    if (!Abort().is_aborting()) TagDisc(path, tracks, attempt + 1);
                });
                return;
            }
            g_tagging.erase(path);
            if (!release) return;
            auto written = fb2k::makeCompletionNotify([tracks, title = release->title](unsigned code) {
                if (code != metadb_io::update_info_success) {
                    Log(L"cd: tagging \"" + title + L"\" failed (" + std::to_wstring(code) + L")");
                    return;
                }
                Log(L"cd: tagged " + std::to_wstring(tracks.get_count()) + L" tracks of \"" + title + L"\"");
                media::PlayStats::Get().Refresh(tracks); // the history showed "CD Track 01"
                NotifyDiscsChanged();                    // the disc's name and rows
            });
            metadb_io_v2::get()->update_info_async(
                tracks, fb2k::service_new<TagFilter>(release, first), core_api::get_main_window(),
                metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background |
                    metadb_io_v2::op_flag_no_errors,
                written);
        });
    });
}
} // namespace

} // namespace glasswave::cd
