#pragma once

// Lyrics as the UI shows them: lines with optional per-word timing, a translation and a
// romanization. Every source (embedded tags, .lrc files, the searchers of
// lyrics/lyrics_service.h) delivers LRC text that ParseLrc turns into this model.

namespace glasswave::lyrics {

struct Word {
    int start = 0, end = 0;  // ms
    std::wstring text;       // includes a trailing space where the source had one
};

struct Line {
    int start = 0, end = 0;     // ms; unsynced lyrics: both 0
    std::wstring text;          // the whole line (the words joined when timed per word)
    std::vector<Word> words;    // empty: timed per line only
    std::wstring translation;   // first reference line with the same timestamp
    std::wstring romanization;  // second reference line, if any
};

struct Lyrics {
    std::vector<Line> lines;
    bool synced = false;   // lines carry timestamps
    bool karaoke = false;  // at least one line is timed per word
    std::wstring source;   // where they came from, for the UI ("Local file", a searcher's name)
    std::wstring title, artist, album;  // [ti:] [ar:] [al:] tags, or the searcher's match
};

// Parses LRC, including the enhanced format (<mm:ss.xx> word times) and reference lines
// (several lines with one timestamp: the first is the original, the next the translation
// and then the romanization, as ESLyric defines it). Text without timestamps gives unsynced
// lyrics, one line per non-empty text line. [offset:] is applied.
Lyrics ParseLrc(std::wstring_view text);

// Decodes a lyrics file: UTF-8 (with or without BOM), UTF-16 LE/BE with BOM, else the ANSI
// code page.
std::wstring DecodeText(std::string_view bytes);

}  // namespace glasswave::lyrics
