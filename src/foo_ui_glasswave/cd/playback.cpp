#include "pch.h"
#include "cd/playback.h"

#include "cd/disc.h"
#include "cd/discs.h"
#include "cd/musicbrainz.h"
#include "media/path_behaviour.h"
#include "media/special_playlists.h"

#include <SDK/initquit.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace glasswave::cd {

namespace {

using media::IsQueue;
using media::IsSpecial;
using media::PlayQueue;
using media::QueuePlaylist;

// A CD track foobar2000 plays from its own playlist: the queue takes the disc over (the song
// starts again from the queue) and that playlist goes.
void TakeOverDisc(const metadb_handle_ptr& track) {
    auto pm = playlist_manager::get();
    t_size playlist = SIZE_MAX, row = SIZE_MAX;
    if (!pm->get_playing_item_location(&playlist, &row) || IsQueue(playlist) || IsSpecial(playlist) ||
        pm->playlist_get_item_handle(playlist, row) != track)
        return;
    metadb_handle_list items;
    pm->playlist_get_all_items(playlist, items);
    for (const auto& item : items)
        if (!item.is_valid() || !cd::IsCdPath(item->get_path()))
            return; // a playlist of the owner's that holds a CD track: theirs
    PlayQueue(items, row);
    // The queue plays the disc now; the CD's own playlist (still at its index) can go.
    if (IsQueue(pm->get_playing_playlist()) && playlist < pm->get_playlist_count() &&
        pm->playlist_get_item_count(playlist) == items.get_count() && !IsSpecial(playlist))
        pm->remove_playlist(playlist);
    cd::TagDiscs(items);
}

void OnNewTrack(const metadb_handle_ptr& track) {
    if (!track.is_valid() || !cd::IsCdPath(track->get_path())) return;
    const std::string disc = cd::DiscKey(track->get_path());
    cd::SetDiscPresent(disc, true);
    // Played from File > Open audio CD while discs are read only when opened: its drive shows it.
    const auto& drives = cd::Drives();
    if (std::none_of(drives.begin(), drives.end(), [&](const cd::CdDrive& d) { return d.disc == disc; }))
        cd::ReadUnreadDrives();
    auto pm = playlist_manager::get();
    t_size playlist = SIZE_MAX, row = SIZE_MAX;
    if (pm->get_playing_item_location(&playlist, &row) && !IsQueue(playlist)) {
        TakeOverDisc(track);
        return;
    }
    // Played from the queue (as taken over, or restored after a restart): the disc's tracks there.
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) return;
    metadb_handle_list items, tracks;
    pm->playlist_get_all_items(queue, items);
    for (const auto& item : items)
        if (item.is_valid() && cd::DiscKey(item->get_path()) == disc) tracks.add_item(item);
    cd::TagDiscs(tracks);
}


class PlaybackEvents : public play_callback_impl_base {
  public:
    PlaybackEvents() : play_callback_impl_base(flag_on_playback_new_track) {}
    void on_playback_new_track(metadb_handle_ptr track) override {
        fb2k::inMainThread([track] { OnNewTrack(track); });
    }
};

// cdda:// tracks for the modules that look at items without knowing CDs.
class CdPaths final : public media::PathBehaviour {
  public:
    bool Owns(const char* path) const override { return IsCdPath(path); }
    bool Available(const char* path) const override { return DiscPresent(DiscKey(path)); }
    bool Identified(const metadb_handle_ptr& item) const override {
        const auto info = item->get_info_ref();
        return info.is_valid() && info->info().meta_get("ALBUM", 0) != nullptr;
    }
};

const CdPaths g_paths;
const bool g_pathsRegistered = (media::RegisterPathBehaviour(g_paths), true);

// The queue drops the tracks of discs no longer in a drive (`present`), and the play history
// stops showing them.
void DropAbsent(const std::vector<cd::Toc>& present) {
    std::map<std::string, bool> inDrive; // by disc (DiscKey)
    const auto judge = [&](const std::string& disc) {
        if (inDrive.contains(disc)) return;
        const auto known = cd::KnownDisc(disc);
        const auto same = [&](const cd::Toc& toc) {
            return known && toc.leadout == known->leadout && toc.offsets == known->offsets;
        };
        // A disc never looked up this session: in a drive if any audio disc is.
        inDrive[disc] = known ? std::any_of(present.begin(), present.end(), same) : !present.empty();
        cd::SetDiscPresent(disc, inDrive[disc]);
    };
    for (const auto& path : cd::PresentDiscs())
        judge(path);

    auto pm = playlist_manager::get();
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) return;
    metadb_handle_list items;
    pm->playlist_get_all_items(queue, items);
    pfc::bit_array_bittable gone(items.get_count());
    bool any = false;
    for (t_size i = 0; i < items.get_count(); ++i) {
        if (!items[i].is_valid() || !cd::IsCdPath(items[i]->get_path())) continue;
        const std::string disc = cd::DiscKey(items[i]->get_path());
        judge(disc);
        const bool drop = !inDrive[disc];
        gone.set(i, drop);
        any = any || drop;
    }
    if (!any) return;
    metadb_handle_ptr playing;
    if (playback_control::get()->get_now_playing(playing) && cd::IsCdPath(playing->get_path()) &&
        !inDrive[cd::DiscKey(playing->get_path())])
        playback_control::get()->stop();
    pm->playlist_remove_items(queue, gone);
}


constexpr double kStartupCheckSeconds = 3.0;

std::unique_ptr<PlaybackEvents> g_playback;

class Lifecycle : public initquit {
  public:
    void on_init() override {
        g_playback = std::make_unique<PlaybackEvents>();
        // A queue restored from the last run may hold a disc taken out meanwhile.
        fb2k::callLater(kStartupCheckSeconds, [] {
            if (g_playback) DropEjectedDiscs();
        });
    }
    void on_quit() override { g_playback.reset(); }
};
initquit_factory_t<Lifecycle> g_lifecycle;

} // namespace

void DropEjectedDiscs(std::function<void()> checked) {
    fb2k::splitTask([checked] {
        std::vector<cd::Toc> present;
        for (const wchar_t drive : cd::CdDrives())
            if (auto toc = cd::ReadToc(drive)) present.push_back(std::move(*toc));
        fb2k::inMainThread([present, checked] {
            DropAbsent(present);
            if (checked) checked();
        });
    });
}

} // namespace glasswave::cd
