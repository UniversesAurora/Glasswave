#pragma once

#include "cd/disc.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

// What MusicBrainz knows about an audio CD: found by the disc id of its table of contents
// (or, for a disc not yet submitted, by the TOC itself), the matching release and medium.
// The disc's tracks are tagged from it, and its cover comes from the Cover Art Archive.
namespace glasswave::cd {

struct ReleaseTrack {
    std::wstring title;
    std::wstring artist;
    std::wstring recordingId;
    std::wstring trackId;
};

struct Release {
    std::wstring id; // MusicBrainz release id
    std::wstring title;
    std::wstring artist; // release artist credit
    std::wstring date;
    std::wstring discId; // the disc's MusicBrainz id
    int disc = 1;        // medium position
    int discs = 1;       // media in the release
    bool frontCover = false;
    std::vector<ReleaseTrack> tracks; // the medium's tracks; [0] is the TOC's first track
};

// Blocking; worker threads only. The release of the disc a foobar2000 CD track is on, or null
// when it is not in a drive, MusicBrainz has none, or the service could not be reached. An
// answer (a release or none) is kept for the session; a failure is asked again next time.
std::shared_ptr<const Release> LookupDisc(const metadb_handle_ptr& track, abort_callback& abort);
// The TOC a disc (DiscKey) was found with (empty before a lookup).
std::optional<Toc> KnownDisc(const std::string& disc);

// Main thread: tags the CD tracks among `items` that have no album yet, in the background.
void TagDiscs(const metadb_handle_list& items);

} // namespace glasswave::cd
