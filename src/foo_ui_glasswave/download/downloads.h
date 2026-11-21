#pragma once

#include "app/settings.h"
#include "stream/model.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Song downloads (owner, 2026-11-15): any source with stream::Capability::Download, through
// stream::Provider::ResolveDownload, so a new source only says where a song's bytes come from.
// The service queues songs, fetches a few at a time into "<file>.part" (resumed after a pause
// or a restart), names the file by the user's rules, tags it (text and front cover through
// foobar2000's own tag writers) and remembers it: from then on the source's input plays the
// file instead of streaming (LocalFile) until the file is gone.
//
// Threading: everything is called on the main thread, except LocalFile (any thread).
// Listeners run on the main thread, at most a few times a second while bytes arrive.
namespace glasswave::download {

enum class State {
    Waiting, // queued, not started
    Running,
    Paused,
    Failed,
    Done
};

struct Item {
    uint64_t id = 0;       // this run's handle
    std::wstring provider; // stream::Provider::Key
    stream::Id track;
    std::wstring title, artist, album; // as the list shows it
    std::wstring root;  // Download to...: the folder the naming rules start from (empty: the setting)
    std::wstring path;  // the file; empty until the song's details are known
    std::wstring format; // as the source names it ("FLAC")
    State state = State::Waiting;
    std::wstring error;  // Failed: why (untranslated or the service's own words)
    uint64_t received = 0, size = 0; // bytes; size 0: unknown
    double speed = 0;                // bytes per second while running
    int64_t added = 0, finished = 0; // Unix seconds
};

// File names and folders (NetEase's 音乐命名格式 and 文件智能分类).
enum class Naming { Title, ArtistTitle, TitleArtist };
enum class Folders { None, Artist, ArtistAlbum };

std::wstring Folder(); // where downloads go: the setting, or <Music>\Glasswave
void SetFolder(const std::wstring& folder);
Naming GetNaming();
void SetNaming(Naming naming);
Folders GetFolders();
void SetFolders(Folders folders);
// How many songs download at once, and over how many connections each (aria2's split).
inline constexpr settings::Range kSongsAtOnce{1, 10, 3};
inline constexpr settings::Range kConnections{1, 16, 8};
int SongsAtOnce();
void SetSongsAtOnce(int songs);
int Connections();
void SetConnections(int connections);
// Lyrics in downloaded files: the ones the lyrics panel shows for the song (the owner's
// selection, else the automatic answer, with its offset), embedded, beside the file as
// "<name>.lrc", or both. Kept in step: another selection, none, a new search or another offset
// rewrites them; a new setting rewrites every downloaded file.
enum class LyricsMode { None, Embed, File, Both };
LyricsMode GetLyricsMode();
void SetLyricsMode(LyricsMode mode);

// The engine as Preferences shows it ("aria2 1.37.0").
std::wstring Engine();

// The items, in the order they were added.
std::vector<Item> Items();
// True when the song's source can download it.
bool Downloadable(const metadb_handle_ptr& item);
bool AnyDownloadable(const metadb_handle_list& items);
// A song's download as song lists show it (main thread, cheap): the latest item of the song,
// nullopt when there is none or its file is gone.
struct SongState {
    uint64_t id = 0;
    State state = State::Waiting;
    float progress = 0; // 0-1 while unfinished
};
std::optional<SongState> StateOf(const metadb_handle_ptr& item);

// Queues the songs that can be downloaded and are not downloaded or queued already; `root`
// replaces the download folder (Download to...). Returns how many were queued.
size_t Add(const metadb_handle_list& items, const std::wstring& root = {});
// One song to one file (Save as...): replaces a file of that name.
bool AddAs(const metadb_handle_ptr& item, const std::wstring& path);

void Pause(const std::vector<uint64_t>& ids);
void Resume(const std::vector<uint64_t>& ids); // Paused or Failed: queued again
// Unfinished items leave the list and their partial files are deleted.
void Cancel(const std::vector<uint64_t>& ids);
// Finished items leave the list; with deleteFiles their files go to the Recycle Bin.
void Forget(const std::vector<uint64_t>& ids, bool deleteFiles);

// The downloaded file of a song while it exists (any thread).
std::optional<std::wstring> LocalFile(std::wstring_view provider, std::wstring_view track);

// Main-thread change notifications (the list, a state or progress).
uint64_t Subscribe(std::function<void()> listener);
void Unsubscribe(uint64_t token);

} // namespace glasswave::download
