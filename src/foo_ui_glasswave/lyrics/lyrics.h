#pragma once

// Lyrics as the UI shows them: lines with optional per-word timing, a translation and a
// romanization. Every source (embedded tags, .lrc files, the searchers of
// lyrics/lyrics_service.h) delivers LRC text that ParseLrc turns into this model.

namespace glasswave::lyrics {

enum class TrackKind : int {
    Original = 0,
    Translation = 1,
    Pronunciation = 2,
};

enum class DisplayOrder : int {
    OriginalPronunciationTranslation = 0,
    OriginalTranslationPronunciation = 1,
    TranslationOriginalPronunciation = 2,
    TranslationPronunciationOriginal = 3,
    PronunciationOriginalTranslation = 4,
    PronunciationTranslationOriginal = 5,
};

struct Word {
    int start = 0, end = 0;  // ms
    std::wstring text;       // includes a trailing space where the source had one
};

struct SecondaryLine {
    TrackKind kind = TrackKind::Translation;
    std::wstring text;
    std::vector<Word> words; // real per-word timing when available (Original or timed Pronunciation)
};

struct Line {
    int start = 0, end = 0;         // ms; unsynced lyrics: both 0
    std::wstring text;              // the main line (the words joined when timed per word)
    std::vector<Word> words;        // empty: timed per line only
    std::wstring translation;       // translation track, if any
    std::wstring romanization;      // pronunciation / romanization track, if any
    std::vector<Word> romanWords;   // pronunciation word timing when provided (e.g. Apple TTML)
    TrackKind mainKind = TrackKind::Original;
    std::vector<SecondaryLine> secondaries; // projected 0..2 secondary lines shown under main
};

struct DisplayOptions {
    bool showOriginal = true;
    bool showTranslation = true;
    bool showPronunciation = false;
    DisplayOrder order = DisplayOrder::OriginalPronunciationTranslation;
};

struct Lyrics {
    std::vector<Line> lines;
    bool synced = false;   // lines carry timestamps
    bool karaoke = false;  // at least one line is timed per word
    std::wstring source;   // where they came from, for the UI ("Local file", a searcher's name)
    std::wstring sourceKey;  // the searcher's stable key, when a searcher found them
    std::wstring title, artist, album;  // [ti:] [ar:] [al:] tags, or the searcher's match
    // Where the lines came from: the LRC document as found (Lyric info, saving, Select), and
    // the file it was read from with the code page it was read in (0: detected), if any.
    std::wstring document;
    std::wstring file;
    unsigned codePage = 0;
};

// Parses LRC, including the enhanced format (<mm:ss.xx> word times) and reference lines
// (several lines with one timestamp: the first is the original, the next the translation
// and then the romanization, as ESLyric defines it). Text without timestamps gives unsynced
// lyrics, one line per non-empty text line. [offset:] is applied.
Lyrics ParseLrc(std::wstring_view text);

// Decodes a lyrics file: UTF-8 (with or without BOM), UTF-16 LE/BE with BOM, else the ANSI
// code page. A non-zero `codePage` reads the bytes in that code page instead (1200 / 1201
// for UTF-16 LE / BE); a byte order mark is skipped either way.
std::wstring DecodeText(std::string_view bytes, unsigned codePage = 0);

// Projects raw lyrics into a main line plus 0..2 secondary lines according to `options`:
// never cross-substitutes translation and pronunciation, falls back to original when no
// enabled track is present, and preserves real syllable timing on original / pronunciation.
Lyrics ProjectLyrics(const Lyrics& lyrics, const DisplayOptions& options);

// Moves every time by `offsetMs` the way LRC's [offset:] does: positive shows the lyrics
// earlier. Times stop at 0.
void ApplyOffset(Lyrics& lyrics, int offsetMs);

// The document with `addMs` more of [offset:] (the tag replaced, or added after the other
// tags); 0 leaves it as it is.
std::wstring AddOffsetTag(std::wstring_view document, int addMs);

// Lyrics written out again as text, for the clipboard.
struct WriteOptions {
    bool metadata = false;    // [ti:] [ar:] [al:]
    bool timestamps = false;  // [mm:ss.xx] (and the words' <mm:ss.xx> unless `standard`)
    bool mergeSame = false;   // lines with the same text share one line of timestamps
    bool references = false;  // translations and romanizations
    bool standard = false;    // word timing left out
};
std::wstring WriteLrc(const Lyrics& lyrics, const WriteOptions& options);

}  // namespace glasswave::lyrics
