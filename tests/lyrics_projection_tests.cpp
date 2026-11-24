#include "lyrics/lyrics.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace glasswave::lyrics;

size_t g_checks = 0;

void Check(bool condition, std::string_view message) {
    ++g_checks;
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void TestTaggedLrcParsing() {
    // Explicit [gw:tr] and [gw:ro] with syllable timing on both main and pronunciation.
    const std::wstring raw =
        L"[00:10.00]<00:10.00>Ko<00:10.50>re<00:11.00>\n"
        L"[00:10.00][gw:tr]This is it\n"
        L"[00:10.00][gw:ro]<00:10.02>Ko<00:10.50>re<00:11.00>\n"
        L"[00:12.00]<00:12.00>OnlyRo<00:13.00>\n"
        L"[00:12.00][gw:ro]<00:12.10>Ro<00:12.60>ma<00:13.00>\n";

    Lyrics parsed = ParseLrc(raw);
    Check(parsed.synced && parsed.karaoke, "Tagged LRC must be synced karaoke");
    Check(parsed.lines.size() == 2, "Tagged LRC must produce 2 grouped lines");
    Check(parsed.lines[0].text == L"Kore", "Line 0 main text");
    Check(parsed.lines[0].translation == L"This is it", "Line 0 translation");
    Check(parsed.lines[0].romanization == L"Kore", "Line 0 romanization");
    Check(parsed.lines[0].romanWords.size() == 2 && parsed.lines[0].romanWords[0].start == 10020 &&
              parsed.lines[0].romanWords[1].end == 11000,
          "Line 0 must preserve real pronunciation word timing");
    Check(parsed.lines[1].translation.empty(),
          "Line 1 with only [gw:ro] must leave translation empty (no misattribution)");
    Check(parsed.lines[1].romanization == L"Roma", "Line 1 pronunciation text");
    Check(parsed.lines[1].romanWords.size() == 2 && parsed.lines[1].romanWords[0].start == 12100,
          "Line 1 must preserve pronunciation word timing");

    // Offset shifts both main words and romanWords.
    ApplyOffset(parsed, 100);
    Check(parsed.lines[0].start == 9900 && parsed.lines[0].romanWords[0].start == 9920,
          "ApplyOffset must shift romanWords alongside main words");

    // Standard unmarked 3-line ESLyric LRC continues to parse cleanly.
    const std::wstring standard =
        L"[00:05.00]Orig\n"
        L"[00:05.00]Trans\n"
        L"[00:05.00]Roman\n";
    Lyrics stdParsed = ParseLrc(standard);
    Check(stdParsed.lines.size() == 1 && stdParsed.lines[0].text == L"Orig" &&
              stdParsed.lines[0].translation == L"Trans" && stdParsed.lines[0].romanization == L"Roman",
          "Standard 3-line LRC must remain compatible");

    // WriteLrc omits internal [gw:*] tags.
    const std::wstring written = WriteLrc(parsed, {false, true, false, true, false});
    Check(written.find(L"[gw:") == std::wstring::npos, "WriteLrc must emit clean standard LRC");
}

void TestProjectionOrdersAndFallbacks() {
    const std::wstring raw =
        L"[00:10.00]<00:10.00>Orig1<00:11.00>\n"
        L"[00:10.00][gw:tr]Trans1\n"
        L"[00:10.00][gw:ro]<00:10.05>Pron1<00:11.00>\n"
        L"[00:12.00]<00:12.00>Orig2<00:13.00>\n";
    const Lyrics full = ParseLrc(raw);

    // 1. All three enabled, default order: Original -> Pronunciation -> Translation.
    {
        DisplayOptions opts{true, true, true, DisplayOrder::OriginalPronunciationTranslation};
        Lyrics p = ProjectLyrics(full, opts);
        Check(p.lines[0].mainKind == TrackKind::Original && p.lines[0].text == L"Orig1",
              "Default 3-line main must be Original");
        Check(p.lines[0].secondaries.size() == 2 &&
                  p.lines[0].secondaries[0].kind == TrackKind::Pronunciation &&
                  p.lines[0].secondaries[0].text == L"Pron1" &&
                  !p.lines[0].secondaries[0].words.empty() &&
                  p.lines[0].secondaries[1].kind == TrackKind::Translation &&
                  p.lines[0].secondaries[1].text == L"Trans1",
              "Default 3-line secondaries must be [Pronunciation (timed), Translation]");
        // Line 2 has only Original -> no empty secondaries.
        Check(p.lines[1].text == L"Orig2" && p.lines[1].secondaries.empty(),
              "Line without translation/pronunciation must omit empty secondaries");
    }

    // 2. All three enabled, Pronunciation as Main: Pronunciation -> Original -> Translation.
    {
        DisplayOptions opts{true, true, true, DisplayOrder::PronunciationOriginalTranslation};
        Lyrics p = ProjectLyrics(full, opts);
        Check(p.lines[0].mainKind == TrackKind::Pronunciation && p.lines[0].text == L"Pron1" &&
                  !p.lines[0].words.empty() && p.lines[0].words[0].start == 10050,
              "Pronunciation as main must use real romanWords timing");
        Check(p.lines[0].secondaries.size() == 2 &&
                  p.lines[0].secondaries[0].kind == TrackKind::Original &&
                  !p.lines[0].secondaries[0].words.empty() &&
                  p.lines[0].secondaries[1].kind == TrackKind::Translation,
              "Pronunciation-first 3-line secondaries must be [Original (timed), Translation]");
        // Line 2 lacks Pronunciation & Translation -> falls back to Original cleanly!
        Check(p.lines[1].mainKind == TrackKind::Original && p.lines[1].text == L"Orig2" &&
                  p.lines[1].secondaries.empty(),
              "Per-line fallback must show Original when Pronunciation is absent");
    }

    // 3. Strict NO cross-substitution: Original + Pronunciation on a song with only Original + Translation.
    {
        const Lyrics origTransOnly = ParseLrc(L"[00:01.00]Orig\n[00:01.00][gw:tr]Trans\n");
        DisplayOptions opts{true, false, true, DisplayOrder::PronunciationOriginalTranslation};
        Lyrics p = ProjectLyrics(origTransOnly, opts);
        Check(p.lines[0].mainKind == TrackKind::Original && p.lines[0].text == L"Orig" &&
                  p.lines[0].secondaries.empty(),
              "Original+Pronunciation must NOT substitute Translation when Pronunciation is absent");
    }

    // 4. Final fallback to Original: Translation-only on a song with only Original + Pronunciation.
    {
        const Lyrics origPronOnly = ParseLrc(L"[00:01.00]Orig\n[00:01.00][gw:ro]Pron\n");
        DisplayOptions opts{false, true, false, DisplayOrder::TranslationOriginalPronunciation};
        Lyrics p = ProjectLyrics(origPronOnly, opts);
        Check(p.lines[0].mainKind == TrackKind::Original && p.lines[0].text == L"Orig" &&
                  p.lines[0].secondaries.empty(),
              "Translation-only with no translation must fall back to Original, NOT Pronunciation");
    }

    // 5. Final fallback to Original: Pronunciation-only on a song with only Original + Translation.
    {
        const Lyrics origTransOnly = ParseLrc(L"[00:01.00]Orig\n[00:01.00][gw:tr]Trans\n");
        DisplayOptions opts{false, false, true, DisplayOrder::PronunciationTranslationOriginal};
        Lyrics p = ProjectLyrics(origTransOnly, opts);
        Check(p.lines[0].mainKind == TrackKind::Original && p.lines[0].text == L"Orig" &&
                  p.lines[0].secondaries.empty(),
              "Pronunciation-only with no pronunciation must fall back to Original, NOT Translation");
    }

    // 6. Translation-only when Translation exists -> Main = Translation with spread word timing!
    {
        DisplayOptions opts{false, true, false, DisplayOrder::OriginalPronunciationTranslation};
        Lyrics p = ProjectLyrics(full, opts);
        Check(p.lines[0].mainKind == TrackKind::Translation && p.lines[0].text == L"Trans1" &&
                  !p.lines[0].words.empty() && p.lines[0].secondaries.empty(),
              "Translation-only must promote Translation to Main with spread word timing");
    }
}

} // namespace

int main() {
    TestTaggedLrcParsing();
    TestProjectionOrdersAndFallbacks();
    std::cout << "lyrics_projection_tests: " << g_checks << " checks passed.\n";
    return 0;
}
