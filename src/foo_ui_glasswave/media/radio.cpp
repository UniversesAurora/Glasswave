#include "pch.h"
#include "core/tsv.h"
#include "media/radio.h"
#include "core/signal.h"
#include "core/text.h"

#include "media/special_playlists.h"
#include "ui/i18n.h"
#include "core/log.h"

#include <deque>

namespace glasswave::media::radio {

namespace {

constexpr size_t kAhead = 2;      // songs queued after the one playing
constexpr int kMaxFailures = 3;   // empty answers in a row before the station stops asking

struct State {
    stream::Provider* provider = nullptr;
    stream::Id station;
    std::wstring name;
    bool active = false;   // its songs are in the queue
    uint64_t generation = 0; // the queue's content generation when they were put there
    std::deque<stream::Track> pending; // sent by the service, not queued yet
    bool fetching = false;
    bool stalled = false;  // the queue ran out while the service was asked
    int failures = 0;
    // The station's own account of its songs: what it queued, in order, and which of them plays.
    // The queue's places cannot tell: repeating one song puts the songs still to come before it.
    std::vector<metadb_handle_ptr> queued;
    size_t playing = 0;
};

State g_state;

// The station playing, kept for the next run: its source, its id and name, and the songs it put in
// the queue (their paths), so a queue that still holds them goes on as the station.
// {3F6A2C18-5B7D-4E21-9A44-6C1D825E37B9}
cfg_var_modern::cfg_string cfg_station({0x3f6a2c18, 0x5b7d, 0x4e21, {0x9a, 0x44, 0x6c, 0x1d, 0x82, 0x5e, 0x37, 0xb9}}, "");
constexpr char kField = '\x1f', kItem = '\x1e';
uint64_t g_serial = 0;
uint64_t g_scheduleListener = 0; // the queue's schedule moved (a song started, the queue changed) // a station started later makes the answers for an earlier one stale
core::Signal<> g_listeners;

using core::Utf8;
using core::Wide;

void Notify() {
    g_listeners.Emit();
}

void Save() {
    if (!g_state.active || !g_state.provider) {
        cfg_station = "";
        return;
    }
    std::string text =
        Utf8(g_state.provider->Key()) + kField + Utf8(g_state.station) + kField + Utf8(g_state.name) + kField;
    for (size_t i = 0; i < g_state.queued.size(); ++i) {
        if (i) text += kItem;
        text += g_state.queued[i]->get_path();
    }
    cfg_station = text.c_str();
}

void End() {
    if (!g_state.active && !g_state.fetching) return;
    g_state = {};
    cfg_station = "";
    ++g_serial;
    Notify();
}

// Still the queue's source: nothing else has been played into it since.
bool Current() {
    if (!g_state.active) return false;
    if (QueueContentGeneration() == g_state.generation) return true;
    core::Log(L"radio: " + g_state.name + L" ended (the queue was replaced)");
    End();
    return false;
}

metadb_handle_list Items(stream::Provider& provider, const std::vector<stream::Track>& tracks) {
    metadb_handle_list items;
    for (const auto& track : tracks)
        if (auto item = provider.TrackItem(track.id); item.is_valid()) items.add_item(item);
    return items;
}

void Fetch();

// Which of the station's songs plays: the nearest one to the last known that is the song now
// playing (a song can come twice; Previous goes back, Next on).
void FollowPlaying() {
    metadb_handle_ptr now;
    if (!playback_control::get()->get_now_playing(now) || g_state.queued.empty()) return;
    const size_t from = std::min(g_state.playing, g_state.queued.size() - 1);
    for (size_t d = 0; d < g_state.queued.size(); ++d) {
        if (from + d < g_state.queued.size() && g_state.queued[from + d] == now) {
            g_state.playing = from + d;
            return;
        }
        if (d <= from && g_state.queued[from - d] == now) {
            g_state.playing = from - d;
            return;
        }
    }
}

// The station's songs queued after the playing one.
size_t QueuedAhead() {
    FollowPlaying();
    return g_state.queued.empty() ? 0 : g_state.queued.size() - 1 - std::min(g_state.playing, g_state.queued.size() - 1);
}

// Keeps kAhead songs queued after the playing one, and asks for more before they run out.
void TopUp() {
    if (!Current()) return;
    // Nothing plays (a restored station before Play): nothing to keep ahead of, unless the queue
    // ran out under a playing station.
    if (!g_state.stalled && !playback_control::get()->is_playing()) return;
    const size_t ahead = QueuedAhead();
    metadb_handle_list add;
    while (ahead + add.get_count() < kAhead && !g_state.pending.empty()) {
        if (auto item = g_state.provider->TrackItem(g_state.pending.front().id); item.is_valid()) add.add_item(item);
        g_state.pending.pop_front();
    }
    if (add.get_count()) {
        core::Log(L"radio: " + std::to_wstring(add.get_count()) + L" more queued, " +
                  std::to_wstring(g_state.pending.size()) + L" waiting");
        for (size_t i = 0; i < add.get_count(); ++i) g_state.queued.push_back(add[i]);
        Save();
        if (!AppendSourceBatch(add, g_state.generation)) {
            core::Log(L"radio: " + g_state.name + L" ended (the queue refused its songs)");
            End();
            return;
        }
        if (std::exchange(g_state.stalled, false)) playback_control::get()->start(playback_control::track_command_next);
    }
    if (g_state.pending.size() < kAhead && !g_state.fetching && g_state.failures < kMaxFailures) Fetch();
}

void Fetch() {
    g_state.fetching = true;
    const uint64_t serial = g_serial;
    g_state.provider->FetchStationTracks(g_state.station, [serial](std::vector<stream::Track> tracks, std::wstring error) {
        if (serial != g_serial) return;
        g_state.fetching = false;
        if (tracks.empty()) {
            // A source still restoring its session (start-up) has not refused: asked again later.
            if (!g_state.provider->SessionRestoring() && g_state.provider->SignedIn()) ++g_state.failures;
            if (!error.empty()) console::print(("Glasswave radio: " + Utf8(error)).c_str());
            if (g_state.failures >= kMaxFailures) { // the songs queued play on as an ordinary list
                core::Log(L"radio: " + g_state.name + L" sends nothing; the queue goes on as a list");
                End();
            }
            return; // asked again when the next song starts
        }
        g_state.failures = 0;
        for (auto& track : tracks) g_state.pending.push_back(std::move(track));
        TopUp();
    });
}

class Playback : public play_callback_static {
  public:
    unsigned get_flags() override { return flag_on_playback_stop; }
    void on_playback_new_track(metadb_handle_ptr) override {}
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason != play_control::stop_reason_eof) return;
        fb2k::inMainThread([] {
            if (!Current()) return;
            g_state.stalled = true; // the songs arriving start playing again
            TopUp();
        });
    }
    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};

play_callback_static_factory_t<Playback> g_playback;

void ListenToSchedule();

// The last run's station: if the queue still holds its songs (the scheduler restored it), it goes
// on as that station (more songs come as they are needed). Otherwise, or without its source, the
// queue is an ordinary list and the record goes.
void Restore() {
    const std::string text = cfg_station.get().c_str();
    if (text.empty() || g_state.active) return;
    const auto fields = core::tsv::Split(text, kField); // provider, station, name, paths
    const std::string_view paths = fields.size() == 4 ? fields[3] : std::string_view();
    stream::Provider* provider = fields.size() == 4 ? stream::FindProvider(Wide(std::string(fields[0]))) : nullptr;
    const t_size queue = QueuePlaylist(false);
    metadb_handle_list items;
    if (queue != SIZE_MAX) playlist_manager::get()->playlist_get_all_items(queue, items);
    const auto expected = paths.empty() ? std::vector<std::string_view>() : core::tsv::Split(paths, kItem);
    bool same = provider && !expected.empty() && expected.size() == items.get_count();
    for (size_t i = 0; same && i < expected.size(); ++i) same = expected[i] == items[i]->get_path();
    if (!same) {
        cfg_station = "";
        return;
    }
    g_state.provider = provider;
    g_state.station = Wide(fields[1]);
    g_state.name = Wide(fields[2]);
    for (size_t i = 0; i < items.get_count(); ++i) g_state.queued.push_back(items[i]);
    g_state.playing = g_state.queued.size() - 1; // where it stopped is found as it plays
    g_state.generation = QueueContentGeneration();
    g_state.active = true;
    ++g_serial;
    core::Log(L"radio: " + g_state.name + L" restored with " + std::to_wstring(items.get_count()) +
              L" songs");
    ListenToSchedule();
    Notify();
}

class RestoreAtStart : public initquit {
  public:
    // After every component's start (the scheduler restores the queue in its own).
    void on_init() override {
        fb2k::inMainThread([] { fb2k::inMainThread([] { Restore(); }); });
    }
    void on_quit() override {}
};

initquit_factory_t<RestoreAtStart> g_restore;

} // namespace

namespace {
// The schedule tells when the playing song moves on (after it has moved, unlike a playback
// callback, which may run before the scheduler's own).
void ListenToSchedule() {
    if (g_scheduleListener) return;
    // Get(), not Existing(): a station needs the queue, so the scheduler exists from here on and
    // the station always follows its schedule.
    g_scheduleListener = PlaybackScheduler::Get().AddListener([] {
        if (!g_state.active) return;
        TopUp();
        Notify(); // the buttons follow where the station stands
    });
}
} // namespace

void Play(stream::Provider& provider, const stream::Id& station, std::wstring name) {
    End();
    g_state.provider = &provider;
    g_state.station = station;
    g_state.name = std::move(name);
    g_state.fetching = true;
    const uint64_t serial = ++g_serial;
    ListenToSchedule();
    provider.FetchStationTracks(station, [serial](std::vector<stream::Track> tracks, std::wstring error) {
        if (serial != g_serial) return;
        g_state.fetching = false;
        if (tracks.empty()) {
            const std::wstring name = g_state.name;
            g_state = {};
            Notify();
            // A read dropped because the source stopped or signed out is nothing to report.
            if (!stream::IsRequestDropped(error))
                popup_message::g_show(
                    Utf8(ui::Tr(error.empty() ? L"The station sent no songs." : error)).c_str(),
                    Utf8(name).c_str());
            return;
        }
        // The first songs go into the queue; the rest wait until they are a song away.
        const size_t first = std::min(tracks.size(), kAhead + 1);
        std::vector<stream::Track> now(tracks.begin(), tracks.begin() + static_cast<std::ptrdiff_t>(first));
        for (size_t i = first; i < tracks.size(); ++i) g_state.pending.push_back(tracks[i]);
        const metadb_handle_list items = Items(*g_state.provider, now);
        for (size_t i = 0; i < items.get_count(); ++i) g_state.queued.push_back(items[i]);
        g_state.playing = 0;
        if (!PlayQueue(items, 0)) {
            // None of the songs could be put in the queue (no playable item, or the queue
            // refused): not a station that is playing.
            const std::wstring name = g_state.name;
            g_state = {};
            Notify();
            popup_message::g_show(Utf8(ui::Tr(L"The station sent no songs.")).c_str(), Utf8(name).c_str());
            return;
        }
        g_state.generation = QueueContentGeneration();
        g_state.active = true;
        Save();
        core::Log(L"radio: " + g_state.name + L" started, " + std::to_wstring(tracks.size()) +
                  L" songs sent");
        Notify();
        TopUp();
    });
}

bool Active() {
    if (!g_state.active) return false;
    if (QueueContentGeneration() == g_state.generation) return true;
    fb2k::inMainThread([] { Current(); }); // something else was played: the station ends (and says so)
    return false;
}

std::wstring Name() { return Active() ? g_state.name : std::wstring(); }

bool CanDislike() { return Active() && g_state.provider->CanDislike(g_state.station); }

// Previous goes back through what the station played, never before its first song: there is
// nothing before it (no station's "back" from there is something the queue could play).
bool CanGoBack() {
    if (!Active()) return true;
    FollowPlaying();
    return g_state.playing > 0;
}

void DislikeCurrent() {
    if (!Current() || !g_state.provider->CanDislike(g_state.station)) return;
    metadb_handle_ptr now;
    if (!playback_control::get()->get_now_playing(now)) return;
    const stream::Id track = g_state.provider->TrackId(now);
    if (track.empty()) return;
    g_state.provider->DislikeStationTrack(g_state.station, track, [](std::wstring error) {
        if (!error.empty()) console::print(("Glasswave radio: " + Utf8(error)).c_str());
    });
    playback_control::get()->start(playback_control::track_command_next);
}

int Subscribe(std::function<void()> callback) {
    return static_cast<int>(g_listeners.Add(std::move(callback)));
}

void Unsubscribe(int token) {
    g_listeners.Remove(static_cast<core::Signal<>::Token>(token));
}

} // namespace glasswave::media::radio
