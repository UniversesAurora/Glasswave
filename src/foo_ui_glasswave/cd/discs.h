#pragma once

#include "cd/disc.h"

#include <functional>
#include <string>
#include <vector>

// The CD drives and the audio discs in them. Each drive has an entry in the navigation pane
// (Local Music > Discs), as Windows Media Player lists the disc in its drive. A disc is read
// (its tracks listed and looked up, never played) as soon as it is inserted when
// settings::AutoLoadCds is on, else when its drive is opened. No playlist is made; playing
// one makes the queue the disc. Taking a disc out drops it.
namespace glasswave::cd {

struct CdDrive {
    enum class State {
        Unread,  // not read since start-up or since a disc came in
        Reading,
        Empty,   // no audio disc
        Loaded,
    };
    wchar_t drive = 0;
    State state = State::Unread;
    std::string disc; // DiscKey of its tracks (Loaded)
    Toc toc;
    metadb_handle_list tracks; // in track order (Loaded)
};

// Main thread.
const std::vector<CdDrive>& Drives();
const CdDrive* FindDrive(wchar_t drive);
// The album once the disc is tagged, "Audio CD (G:)" before, "CD drive (G:)" without a disc read.
std::wstring DriveTitle(const CdDrive& drive);

// Reads the drive's disc in the background, without a progress dialog; nothing while it is
// being read or already loaded.
void ReadDrive(wchar_t drive);
// Reads every drive not read yet (auto-loading switched on, a CD played from elsewhere).
void ReadUnreadDrives();
// WM_DEVICECHANGE: discs came into or left the drives in `units` (bit 0 = A:).
void OnMediaChanged(unsigned long units, bool arrived);

// Called on the main thread when a drive or its disc changed, or the disc was tagged.
int AddDiscsListener(std::function<void()> listener);
void RemoveDiscsListener(int token);
void NotifyDiscsChanged();

// Opens the drive's tray.
void Eject(wchar_t drive);

} // namespace glasswave::cd
