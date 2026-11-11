#include "pch.h"

#include "lyrics/lyrics_service.h"

#include <cstdlib>
#include <iostream>

namespace {

using glasswave::lyrics::Lyrics;
using glasswave::lyrics::SameLyricsDocument;

size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

Lyrics Document() {
    Lyrics lyrics;
    lyrics.synced = true;
    lyrics.karaoke = true;
    lyrics.source = L"Source A";
    lyrics.title = L"Title";
    lyrics.lines = {
        {1000, 2000, L"one ", {{1000, 1400, L"one "}}, L"translation", L""},
        {2200, 3000, L"two", {{2200, 3000, L"two"}}, L"", L""},
    };
    return lyrics;
}

void CheckRequestIdentity() {
    using glasswave::lyrics::IsCurrentLyricsRequest;
    const std::wstring track = L"provider\037apple\037song-1\0370";
    Check(IsCurrentLyricsRequest(7, 7, track, track), "Current request must be accepted");
    Check(!IsCurrentLyricsRequest(6, 7, track, track), "Late result from an older request must be dropped");
    Check(!IsCurrentLyricsRequest(7, 7, track, L"provider\037apple\037song-2\0370"),
          "Result for another real track must be dropped");
}

void CheckDocumentRetention() {
    const Lyrics original = Document();
    Lyrics refresh = original;
    refresh.source = L"Source B";
    refresh.title = L"Updated title";
    Check(SameLyricsDocument(original, refresh),
          "Source and metadata-only refresh must retain the lyric document");

    refresh = original;
    refresh.lines[0].start = 1100;
    Check(!SameLyricsDocument(original, refresh), "Timing change must replace the lyric document");

    refresh = original;
    refresh.lines[1].text = L"changed";
    Check(!SameLyricsDocument(original, refresh), "Text change must replace the lyric document");

    refresh = original;
    refresh.lines[0].words[0].end = 1500;
    Check(!SameLyricsDocument(original, refresh), "Word timing change must replace the lyric document");

    refresh = original;
    refresh.lines[0].translation = L"changed translation";
    Check(!SameLyricsDocument(original, refresh), "Translation change must replace the lyric document");
}

} // namespace

int main() {
    CheckRequestIdentity();
    CheckDocumentRetention();
    std::cout << checks << " lyrics request state checks passed.\n";
}
