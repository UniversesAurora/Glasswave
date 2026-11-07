#pragma once

// Cue sheet knowledge for the library and list views:
//
//  * Source identity: which audio file and track a library entry really plays, so entries
//    that are the same source exposed twice (embedded cue sheet + external .cue files, or a
//    folder registered twice) can be hidden. Different files (an MP3 and a FLAC of the same
//    song) are never treated as equal.
//  * Image coverage: a whole-file entry for an image that a cue sheet already splits into
//    tracks is hidden.
//  * Text repair: foobar2000 decodes BOM-less, non-UTF-8 cue sheets with the system ANSI
//    code page, which garbles e.g. Shift-JIS cues on a GBK system. The cue's real encoding
//    is detected (ICU charset detection + "do the FILE references resolve on disk") and the
//    sheet's TITLE / PERFORMER / REM fields are re-read for display.
//
// Main thread only. Clear() whenever the library is reloaded.

namespace glasswave::media {

struct SourceIdentity {
    std::wstring key;    // lower-cased native audio path + '|' + track within that file
    int preference = 0;  // lower is better when several entries share a key
};

struct RepairedText {
    std::wstring title, artist, album, albumArtist, genre, year;
};

class CueCatalog {
public:
    static CueCatalog& Get();
    void Clear();

    SourceIdentity Identify(const metadb_handle_ptr& handle);
    // True for a whole-file entry (subsong 0) of an image that some cue splits into tracks.
    // Valid after Identify() has seen the cue entries.
    bool IsSplitImage(const metadb_handle_ptr& handle);
    // Correctly decoded cue text when foobar2000's own decoding is known to be wrong.
    const RepairedText* Repair(const metadb_handle_ptr& handle);

private:
    struct TrackText {
        std::wstring title, performer;
    };
    struct CueInfo {
        bool utf8Bom = false;
        bool needsRepair = false;  // foobar2000's ANSI decoding differs from the real encoding
        std::wstring stem;         // cue file name without extension(s), lower-cased
        std::map<int, std::wstring> trackFile;      // TRACK number -> resolved audio path (lower)
        std::map<std::wstring, int> tracksPerFile;  // audio path -> number of tracks in it
        std::wstring albumTitle, albumPerformer, date, genre;
        std::map<int, TrackText> text;
        std::map<int, RepairedText> repaired;  // built lazily
    };
    CueInfo& Cue(const std::wstring& nativePath);

    std::map<std::wstring, CueInfo> m_cues;
    std::set<std::wstring> m_splitImages;  // lower-cased audio paths split by some cue
};

// foobar2000 path (file://, file-relative://, ...) to a native Windows path, or empty.
std::wstring NativePath(const char* fbPath);

}  // namespace glasswave::media
