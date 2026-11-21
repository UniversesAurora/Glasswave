#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Audio CDs: the drive's table of contents and the identities services know a disc by.
// foobar2000's disc id is its own, so a disc is found by reading the drives (worker threads
// only: a drive may spin up).
namespace glasswave::cd {

struct Toc {
    wchar_t drive = 0;             // drive letter
    int first = 0;                 // first audio track number
    int last = 0;                  // last audio track number
    std::vector<uint32_t> offsets; // audio track starts, LBA + 150 (the 2 s pregap), first..last
    uint32_t leadout = 0;          // end of the audio session, LBA + 150

    double TrackSeconds(int track) const;
};

bool IsCdPath(const char* path);
// foobar2000 names a disc's tracks two ways: "cdda://<id>" with the track number as subsong
// (File > Open audio CD) and "cdda://<id>,<nn>" with subsong 0 (the drive added as a folder).
// The disc is "cdda://<id>" in both.
std::string DiscKey(const char* path);
unsigned TrackNumber(const char* path, unsigned subsong);

// Main thread: the discs (DiscKey) in a drive, as last seen
// (a track of it played, or the drives were read after a disc left one).
bool DiscPresent(const std::string& disc);
void SetDiscPresent(const std::string& disc, bool present);
std::vector<std::string> PresentDiscs();

std::vector<wchar_t> CdDrives();
std::optional<Toc> ReadToc(wchar_t drive);

// The disc of a foobar2000 CD track: the drive holding a disc whose track `subsong` lasts
// `seconds` (within a second); with one audio disc in the drives, that one.
std::optional<Toc> FindDisc(unsigned subsong, double seconds);

// MusicBrainz's disc id (SHA-1 of the TOC, MusicBrainz's base64 alphabet) and its fuzzy TOC
// query value ("first+last+leadout+offsets...").
std::wstring MusicBrainzDiscId(const Toc& toc);
std::wstring TocQuery(const Toc& toc);

} // namespace glasswave::cd
