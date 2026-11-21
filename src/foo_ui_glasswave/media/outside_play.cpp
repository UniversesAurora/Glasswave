#include "pch.h"
#include "media/outside_play.h"

#include "media/playback_scheduler.h"
#include "media/special_playlists.h"

#include <SDK/initquit.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace glasswave::media {

namespace {

bool Editing() {
    auto* scheduler = PlaybackScheduler::Existing();
    return scheduler && scheduler->Editing();
}

std::wstring Folder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring NativePath(const metadb_handle_ptr& track) {
    pfc::string8 native;
    if (!track.is_valid() || !filesystem::g_get_native_path(track->get_path(), native)) return {};
    return pfc::stringcvt::string_wide_from_utf8(native).get_ptr();
}

// The queue is the opened file alone (nothing has changed since), played or about to be.
bool QueueIsOnly(const metadb_handle_ptr& track) {
    auto pm = playlist_manager::get();
    const t_size queue = QueuePlaylist(false);
    return queue != SIZE_MAX && pm->playlist_get_item_count(queue) == 1 &&
           pm->playlist_get_item_handle(queue, 0) == track;
}

// foobar2000 starts the opened file right after adding it; the folder may be read before that.
constexpr int kAdoptTries = 15;
constexpr double kAdoptRetrySeconds = 0.2;

void AdoptFolder(metadb_handle_list items, t_size start, int tries) {
    const metadb_handle_ptr opened = items[start];
    if (!QueueIsOnly(opened)) return; // the owner has moved on
    if (PlaybackScheduler::Get().AdoptPlaying(items, start)) return;
    if (tries > 1)
        fb2k::callLater(kAdoptRetrySeconds, [items, start, tries] { AdoptFolder(items, start, tries - 1); });
}

// The files beside an opened one, as foobar2000 adds them (its types, cue sheets, sorting).
void ExpandToFolder(const metadb_handle_ptr& opened) {
    const std::wstring native = NativePath(opened);
    const std::wstring folder = Folder(native);
    if (folder.empty()) return;
    pfc::list_t<pfc::string8> files;
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        files.add_item(
            pfc::stringcvt::string_utf8_from_wide((folder + L"\\" + found.cFileName).c_str()).get_ptr());
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (files.get_count() < 2) return;

    pfc::ptr_list_t<const char> locations;
    for (t_size i = 0; i < files.get_count(); ++i)
        locations.add_item(files[i].get_ptr());
    auto done = process_locations_notify::create([opened](metadb_handle_list_cref items) {
        t_size start = SIZE_MAX;
        for (t_size i = 0; i < items.get_count() && start == SIZE_MAX; ++i)
            if (items[i] == opened) start = i;
        if (start == SIZE_MAX || items.get_count() < 2) return;
        AdoptFolder(items, start, kAdoptTries);
    });
    playlist_incoming_item_filter_v2::get()->process_locations_async(
        locations,
        playlist_incoming_item_filter_v2::op_flag_background |
            playlist_incoming_item_filter_v2::op_flag_delay_ui,
        nullptr, nullptr, core_api::get_main_window(), done);
}

// foobar2000's "Play" of an opened file clears the queue and adds the file: a single song
// added by someone else that leaves the queue holding just it. It is expanded only once it
// plays (a song merely dropped into an empty queue stays alone).
class QueueEvents : public playlist_callback_impl_base {
  public:
    QueueEvents() : playlist_callback_impl_base(flag_on_items_added) {}

    void on_items_added(t_size playlist, t_size base, metadb_handle_list_cref items,
                        const bit_array&) override {
        if (!IsQueue(playlist) || Editing() || base != 0 || items.get_count() != 1 ||
            playlist_manager::get()->playlist_get_item_count(playlist) != 1)
            return;
        const metadb_handle_ptr opened = items[0];
        if (NativePath(opened).empty()) return; // a local file
        fb2k::inMainThread([opened] {
            if (QueueIsOnly(opened)) ExpandToFolder(opened);
        });
    }
};

std::unique_ptr<QueueEvents> g_queue;

class Lifecycle : public initquit {
  public:
    void on_init() override { g_queue = std::make_unique<QueueEvents>(); }
    void on_quit() override { g_queue.reset(); }
};
initquit_factory_t<Lifecycle> g_lifecycle;

} // namespace

} // namespace glasswave::media
