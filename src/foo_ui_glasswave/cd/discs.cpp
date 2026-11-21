#include "pch.h"
#include "cd/discs.h"
#include "core/signal.h"
#include "core/text.h"

#include "app/settings.h"
#include "cd/musicbrainz.h"
#include "ui/i18n.h"
#include "ui/text.h"

#include <SDK/initquit.h>

#include <winioctl.h>

#include <algorithm>
#include <map>

namespace glasswave::cd {

namespace {

std::vector<CdDrive> g_drives;         // main thread, by letter
std::map<wchar_t, uint64_t> g_reading; // drive -> generation of its pending read
uint64_t g_generation = 0;
core::Signal<> g_listeners;

// Created on first use: foobar2000's core is not up while the DLL loads.
abort_callback_impl& Abort() {
    static abort_callback_impl abort;
    return abort;
}

CdDrive* Find(wchar_t drive) {
    for (auto& d : g_drives)
        if (d.drive == drive) return &d;
    return nullptr;
}

// Forgets the drive's disc (taken out, or another one came in).
void Forget(CdDrive& d, CdDrive::State state) {
    g_reading.erase(d.drive);
    if (d.state == CdDrive::State::Loaded) SetDiscPresent(d.disc, false);
    d.state = state;
    d.disc.clear();
    d.toc = {};
    d.tracks.remove_all();
}

// The drives Windows reports now: new ones are unread, gone ones drop their discs.
bool SyncDriveList() {
    const auto letters = CdDrives();
    bool changed = false;
    for (auto it = g_drives.begin(); it != g_drives.end();) {
        if (std::find(letters.begin(), letters.end(), it->drive) != letters.end()) {
            ++it;
            continue;
        }
        Forget(*it, CdDrive::State::Empty);
        it = g_drives.erase(it);
        changed = true;
    }
    for (const wchar_t letter : letters) {
        if (Find(letter)) continue;
        CdDrive d;
        d.drive = letter;
        g_drives.push_back(std::move(d));
        changed = true;
    }
    std::sort(g_drives.begin(), g_drives.end(),
              [](const CdDrive& a, const CdDrive& b) { return a.drive < b.drive; });
    return changed;
}

// Reads the info of tracks the loader left without any (a read while the drive was still
// spinning up shows the path and 0:00) and hands it to the database.
void LoadMissingInfo(const metadb_handle_list& tracks, abort_callback& abort) {
    auto hints = metadb_hint_list::create();
    for (const auto& track : tracks) {
        if (track->get_info_ref().is_valid()) continue;
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                input_info_reader::ptr reader;
                input_entry::g_open_for_info_read(reader, nullptr, track->get_path(), abort);
                file_info_impl info;
                reader->get_info(track->get_subsong_index(), info, abort);
                hints->add_hint(track, info, filestats_invalid, true);
                break;
            } catch (const exception_aborted&) {
                throw;
            } catch (...) {
                abort.sleep(1.0); // the drive may still be busy
            }
        }
    }
    hints->on_done();
}

void Finish(wchar_t drive, uint64_t generation, std::optional<Toc> toc, metadb_handle_list tracks) {
    const auto pending = g_reading.find(drive);
    if (pending == g_reading.end() || pending->second != generation) return; // superseded
    g_reading.erase(pending);
    CdDrive* d = Find(drive);
    if (!d) return;
    if (!toc || tracks.get_count() == 0) {
        d->state = CdDrive::State::Empty;
        NotifyDiscsChanged();
        return;
    }
    d->state = CdDrive::State::Loaded;
    d->toc = std::move(*toc);
    d->tracks = std::move(tracks);
    d->disc = DiscKey(d->tracks[0]->get_path());
    SetDiscPresent(d->disc, true);
    TagDiscs(d->tracks);
    NotifyDiscsChanged();
}

class Lifecycle : public initquit {
  public:
    void on_init() override {
        SyncDriveList();
        if (settings::AutoLoadCds()) ReadUnreadDrives();
    }
    void on_quit() override {
        Abort().abort();
        g_listeners.Clear();
        g_drives.clear();
        g_reading.clear();
    }
};
initquit_factory_t<Lifecycle> g_lifecycle;

} // namespace

const std::vector<CdDrive>& Drives() {
    return g_drives;
}

const CdDrive* FindDrive(wchar_t drive) {
    return Find(drive);
}

std::wstring DriveTitle(const CdDrive& d) {
    const std::wstring letter = L" (" + std::wstring(1, d.drive) + L":)";
    if (d.state != CdDrive::State::Loaded) return ui::Tr(L"CD drive") + letter;
    if (d.tracks.get_count() > 0) {
        const auto info = d.tracks[0]->get_info_ref();
        if (info.is_valid())
            if (const char* album = info->info().meta_get("ALBUM", 0); album && *album)
                return core::Wide(album);
    }
    return ui::Tr(L"Audio CD") + letter;
}

void ReadDrive(wchar_t drive) {
    CdDrive* d = Find(drive);
    if (!d || d->state == CdDrive::State::Reading || d->state == CdDrive::State::Loaded) return;
    d->state = CdDrive::State::Reading;
    const uint64_t generation = ++g_generation;
    g_reading[drive] = generation;
    NotifyDiscsChanged();
    // foobar2000 lists the drive as a folder of tracks ("cdda://<id>,<nn>"). Its loader is
    // called directly on a worker, so no progress dialog shows; the tracks' info is read too.
    fb2k::splitTask([drive, generation] {
        std::optional<Toc> toc = ReadToc(drive);
        metadb_handle_list tracks;
        if (toc) {
            try {
                const char root[] = {static_cast<char>(drive), ':', '\\', 0};
                metadb_handle_list found;
                playlist_loader::g_path_to_handles_simple(root, found, Abort());
                std::vector<metadb_handle_ptr> ordered;
                for (const auto& item : found)
                    if (item.is_valid() && IsCdPath(item->get_path())) ordered.push_back(item);
                std::stable_sort(ordered.begin(), ordered.end(),
                                 [](const metadb_handle_ptr& a, const metadb_handle_ptr& b) {
                                     return TrackNumber(a->get_path(), a->get_subsong_index()) <
                                            TrackNumber(b->get_path(), b->get_subsong_index());
                                 });
                for (const auto& track : ordered)
                    tracks.add_item(track);
                LoadMissingInfo(tracks, Abort());
            } catch (const exception_aborted&) {
                return;
            } catch (...) {
                tracks.remove_all(); // unreadable: shown as empty, read again when opened
            }
        }
        fb2k::inMainThread([drive, generation, toc = std::move(toc), tracks = std::move(tracks)]() mutable {
            Finish(drive, generation, std::move(toc), std::move(tracks));
        });
    });
}

void ReadUnreadDrives() {
    for (const auto& d : std::vector<CdDrive>(g_drives))
        if (d.state == CdDrive::State::Unread || d.state == CdDrive::State::Empty) ReadDrive(d.drive);
}

void OnMediaChanged(unsigned long units, bool arrived) {
    bool changed = SyncDriveList();
    for (auto& d : g_drives) {
        if (!(units & (1ul << (d.drive - L'A')))) continue;
        Forget(d, arrived ? CdDrive::State::Unread : CdDrive::State::Empty);
        changed = true;
        if (arrived && settings::AutoLoadCds()) ReadDrive(d.drive); // notifies
    }
    if (changed) NotifyDiscsChanged();
}

int AddDiscsListener(std::function<void()> listener) {
    return static_cast<int>(g_listeners.Add(std::move(listener)));
}

void RemoveDiscsListener(int token) { g_listeners.Remove(static_cast<core::Signal<>::Token>(token)); }

void NotifyDiscsChanged() { g_listeners.Emit(); }

void Eject(wchar_t drive) {
    fb2k::splitTask([drive] {
        const wchar_t device[] = {L'\\', L'\\', L'.', L'\\', drive, L':', 0};
        HANDLE handle = CreateFileW(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        DWORD bytes = 0;
        DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytes, nullptr);
        CloseHandle(handle);
    });
}

} // namespace glasswave::cd
