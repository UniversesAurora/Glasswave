#pragma once

#include "ui/page.h"
#include "ui/song_actions.h"

// All Music > Downloads (owner, 2026-11-15): the download manager, with tabs as on Recently
// Played. Downloading lists the songs queued, running, paused or failed, with Windows 7's
// progress bar, the speed and the size; Downloaded lists the files, newest first.
namespace glasswave::ui {

std::unique_ptr<Page> MakeDownloadsPage(PageHost& host, const View& view);

// Download, and Save as... (one song) or Download to... (several), for selected songs when
// any of them comes from a source that can download them (download::Service).
void AddDownloadActions(std::vector<MenuAction>& actions, const metadb_handle_list& items, HWND owner);
// Download and Download to... for the songs a fetch delivers (an album or a playlist).
void AddBulkDownloadActions(std::vector<MenuAction>& actions, SongsFetch songs, HWND owner);
// The folder dialog, starting in the download folder (Download to..., the settings). Empty:
// cancelled.
std::wstring PickDownloadFolder(HWND owner);
// Queues the songs a fetch delivers (an album's or a playlist's Download all).
void DownloadSongs(SongsFetch songs, HWND owner);

} // namespace glasswave::ui
