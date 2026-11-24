#include "pch.h"

#include "lyrics/lyrics.h"

namespace glasswave::lyrics {

namespace {

// "mm:ss.xx", "mm:ss:xx", "mm:ss", "hh:mm:ss.xx"; the fraction may have 1-3 digits.
// Returns -1 if `s` is not a time.
int ParseTime(std::wstring_view s) {
    if (s.empty()) return -1;
    long long fields[3]{};
    int count = 0;
    long long fraction = 0;
    int fractionDigits = 0;
    bool inFraction = false, digit = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') {
            digit = true;
            if (inFraction) {
                if (fractionDigits < 3) {
                    fraction = fraction * 10 + (c - L'0');
                    ++fractionDigits;
                }
            } else {
                fields[count] = fields[count] * 10 + (c - L'0');
                if (fields[count] > 100000) return -1;
            }
        } else if (c == L':' && !inFraction) {
            if (!digit) return -1;
            // The third ':' field of "mm:ss:xx" is a fraction.
            if (count == 1 && i + 1 < s.size() && s.find(L':', i + 1) == std::wstring_view::npos &&
                s.find(L'.') == std::wstring_view::npos && s.size() - i - 1 <= 3) {
                inFraction = true;
            } else {
                if (++count > 2) return -1;
            }
            digit = false;
        } else if (c == L'.' && !inFraction) {
            if (!digit || count == 0) return -1;
            inFraction = true;
        } else if (c == L' ' || c == L'\t') {
            continue;
        } else {
            return -1;
        }
    }
    if (count == 0 && !inFraction) return -1;
    long long ms = 0;
    if (count == 2) ms = (fields[0] * 3600 + fields[1] * 60 + fields[2]) * 1000;
    else ms = (fields[0] * 60 + fields[1]) * 1000;
    while (fractionDigits < 3 && fractionDigits > 0) {
        fraction *= 10;
        ++fractionDigits;
    }
    ms += fraction;
    return ms > INT_MAX ? -1 : static_cast<int>(ms);
}

// The ID tags an LRC file carries (lower case). Any other "[word: text]" is lyric text: a
// section note such as "[Chorus: x2]" is a line of the song, not metadata to drop.
bool IsMetadataKey(const std::wstring& key) {
    static constexpr std::wstring_view kKeys[] = {
        L"ti", L"ar",    L"al",   L"au", L"by",   L"offset", L"length", L"re",       L"ve",
        L"id", L"total", L"sign", L"qq", L"hash", L"tool",   L"lang",   L"language", L"encoding"};
    return std::find(std::begin(kKeys), std::end(kKeys), key) != std::end(kKeys);
}

std::wstring_view Trim(std::wstring_view s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == 0xFEFF)) s.remove_prefix(1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r')) s.remove_suffix(1);
    return s;
}

enum class EntryRole { Auto, Translation, Pronunciation };

struct Entry {
    int time = 0;
    size_t order = 0;  // position in the file, keeps the original before its references
    EntryRole role = EntryRole::Auto;
    std::wstring text;
    std::vector<Word> words;
    int wordsEnd = -1;  // trailing <time> with no text after it
};

// Body of a line after its [time] tags: plain text or <time>word<time>word<time>.
void ParseBody(std::wstring_view body, int lineTime, Entry& e) {
    if (body.find(L'<') == std::wstring_view::npos) {
        e.text = std::wstring(Trim(body));
        return;
    }
    std::vector<std::pair<int, std::wstring>> parts;  // time (-1 = none yet), text
    int current = -1;
    std::wstring text;
    size_t i = 0;
    bool timed = false;
    while (i < body.size()) {
        if (body[i] == L'<') {
            const size_t close = body.find(L'>', i);
            if (close != std::wstring_view::npos) {
                const int t = ParseTime(body.substr(i + 1, close - i - 1));
                if (t >= 0) {
                    if (!text.empty() || current >= 0) parts.emplace_back(current, std::move(text));
                    text.clear();
                    current = t;
                    timed = true;
                    i = close + 1;
                    continue;
                }
            }
        }
        text.push_back(body[i]);
        ++i;
    }
    if (!timed) {
        e.text = std::wstring(Trim(body));
        return;
    }
    if (!text.empty()) parts.emplace_back(current, std::move(text));
    else e.wordsEnd = current;  // the closing time of the last word
    for (auto& [t, w] : parts) {
        if (w.empty()) continue;
        Word word;
        word.start = t >= 0 ? t : lineTime;
        word.text = std::move(w);
        e.words.push_back(std::move(word));
    }
    // Whitespace-only words merge into the word before them (they carry no timing worth showing).
    std::vector<Word> merged;
    for (auto& w : e.words) {
        if (Trim(w.text).empty() && !merged.empty()) merged.back().text += w.text;
        else merged.push_back(std::move(w));
    }
    e.words = std::move(merged);
    // Leading whitespace of the first word is noise.
    if (!e.words.empty()) {
        auto& first = e.words.front().text;
        size_t lead = 0;
        while (lead < first.size() && (first[lead] == L' ' || first[lead] == L'\t')) ++lead;
        first.erase(0, lead);
    }
    for (const auto& w : e.words) e.text += w.text;
    while (!e.text.empty() && (e.text.back() == L' ' || e.text.back() == L'\t')) e.text.pop_back();
    if (Trim(e.text).empty()) e.words.clear();
}

}  // namespace

Lyrics ParseLrc(std::wstring_view text) {
    Lyrics out;
    int offset = 0;
    std::vector<Entry> entries;
    std::vector<std::wstring> plain;
    size_t order = 0;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (end < text.size() && text[end] == L'\r' && pos < text.size() && text[pos] == L'\n') ++pos;
        if (line.empty()) {
            if (end >= text.size()) break;
            continue;
        }

        std::vector<int> times;
        size_t at = 0;
        bool tag = false;
        while (at < line.size() && line[at] == L'[') {
            const size_t close = line.find(L']', at);
            if (close == std::wstring_view::npos) break;
            const std::wstring_view inner = line.substr(at + 1, close - at - 1);
            const int t = ParseTime(inner);
            if (t >= 0) {
                times.push_back(t);
            } else if (times.empty()) {
                // [key:value] metadata
                const size_t colon = inner.find(L':');
                if (colon == std::wstring_view::npos) break;
                std::wstring key(Trim(inner.substr(0, colon)));
                const std::wstring value(Trim(inner.substr(colon + 1)));
                std::transform(key.begin(), key.end(), key.begin(), ::towlower);
                if (!IsMetadataKey(key)) break; // lyric text; kept as a line below
                if (key == L"offset") offset = _wtoi(value.c_str());
                else if (key == L"ti") out.title = value;
                else if (key == L"ar") out.artist = value;
                else if (key == L"al") out.album = value;
                tag = true;
                at = close + 1;
                break;  // a metadata line carries nothing else
            } else {
                break;
            }
            at = close + 1;
        }
        if (tag) continue;
        if (times.empty()) {
            plain.emplace_back(line);
            continue;
        }
        std::wstring_view body = line.substr(at);
        EntryRole role = EntryRole::Auto;
        if (body.starts_with(L"[gw:tr]")) {
            role = EntryRole::Translation;
            body.remove_prefix(7);
        } else if (body.starts_with(L"[gw:ro]")) {
            role = EntryRole::Pronunciation;
            body.remove_prefix(7);
        }
        for (int t : times) {
            Entry e;
            e.time = t;
            e.order = order++;
            e.role = role;
            ParseBody(body, t, e);
            entries.push_back(std::move(e));
        }
    }

    if (entries.empty()) {
        for (auto& p : plain) {
            Line l;
            l.text = std::move(p);
            out.lines.push_back(std::move(l));
        }
        return out;
    }

    out.synced = true;
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.time != b.time ? a.time < b.time : a.order < b.order;
    });

    // Group by timestamp: explicit [gw:tr]/[gw:ro] markers assign translation/pronunciation
    // unambiguously; unmarked lines follow ESLyric order (original, translation, romanization).
    // Empty entries end the line before them (instrumental gaps).
    std::vector<int> gapStarts;
    std::vector<int> romanEnds;
    for (size_t i = 0; i < entries.size();) {
        size_t j = i;
        while (j < entries.size() && entries[j].time == entries[i].time) ++j;
        Line* line = nullptr;
        Line current;
        int romanEnd = -1;
        int autoRefs = 0;
        for (size_t k = i; k < j; ++k) {
            Entry& e = entries[k];
            if (Trim(e.text).empty()) continue;
            if (e.role == EntryRole::Translation) {
                if (current.translation.empty()) current.translation = std::wstring(Trim(e.text));
                continue;
            }
            if (e.role == EntryRole::Pronunciation) {
                if (current.romanization.empty()) {
                    current.romanization = std::wstring(Trim(e.text));
                    current.romanWords = std::move(e.words);
                    romanEnd = e.wordsEnd;
                }
                continue;
            }
            if (!line) {
                current.start = e.time;
                current.text = std::move(e.text);
                current.words = std::move(e.words);
                current.end = e.wordsEnd;
                line = &current;
            } else if (autoRefs == 0) {
                if (current.translation.empty()) current.translation = std::wstring(Trim(e.text));
                ++autoRefs;
            } else if (autoRefs == 1) {
                if (current.romanization.empty()) {
                    current.romanization = std::wstring(Trim(e.text));
                    current.romanWords = std::move(e.words);
                    romanEnd = e.wordsEnd;
                }
                ++autoRefs;
            }
        }
        if (line) {
            out.lines.push_back(std::move(current));
            romanEnds.push_back(romanEnd);
        } else {
            gapStarts.push_back(entries[i].time);
        }
        i = j;
    }

    // End times: the explicit end of the last word, else the next line or gap.
    std::sort(gapStarts.begin(), gapStarts.end());
    for (size_t i = 0; i < out.lines.size(); ++i) {
        Line& l = out.lines[i];
        int next = i + 1 < out.lines.size() ? out.lines[i + 1].start : INT_MAX;
        const auto gap = std::upper_bound(gapStarts.begin(), gapStarts.end(), l.start);
        if (gap != gapStarts.end()) next = std::min(next, *gap);
        if (!l.words.empty()) {
            const int lastStart = l.words.back().start;
            if (l.end <= lastStart)  // no closing time: until the next line, at most 5 s
                l.end = next == INT_MAX ? lastStart + 1500 : std::max(lastStart, std::min(next, lastStart + 5000));
            for (size_t w = 0; w < l.words.size(); ++w)
                l.words[w].end = w + 1 < l.words.size() ? l.words[w + 1].start : l.end;
            out.karaoke = true;
        } else {
            l.end = next == INT_MAX ? l.start + 5000 : next;
        }
        if (l.end < l.start) l.end = l.start;
        if (!l.romanWords.empty()) {
            const int lastStart = l.romanWords.back().start;
            int rEnd = romanEnds[i];
            if (rEnd <= lastStart) rEnd = std::max(lastStart, l.end);
            for (size_t w = 0; w < l.romanWords.size(); ++w)
                l.romanWords[w].end = w + 1 < l.romanWords.size() ? l.romanWords[w + 1].start : rEnd;
        }
    }

    ApplyOffset(out, offset);
    return out;
}

void ApplyOffset(Lyrics& lyrics, int offsetMs) {
    if (offsetMs == 0) return;
    // A positive offset shows the lyrics earlier.
    auto shift = [offsetMs](int& t) { t = std::max(0, t - offsetMs); };
    for (auto& l : lyrics.lines) {
        shift(l.start);
        shift(l.end);
        for (auto& w : l.words) {
            shift(w.start);
            shift(w.end);
        }
        for (auto& w : l.romanWords) {
            shift(w.start);
            shift(w.end);
        }
        for (auto& s : l.secondaries) {
            for (auto& w : s.words) {
                shift(w.start);
                shift(w.end);
            }
        }
    }
}

namespace {

std::wstring Utf16(std::string_view bytes, bool bigEndian) {
    std::wstring text;
    text.reserve(bytes.size() / 2);
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto a = static_cast<unsigned char>(bytes[i]), b = static_cast<unsigned char>(bytes[i + 1]);
        text.push_back(static_cast<wchar_t>(bigEndian ? (a << 8) | b : (b << 8) | a));
    }
    return text;
}

std::wstring MultiByte(std::string_view bytes, UINT cp, DWORD flags) {
    std::wstring text;
    if (bytes.empty()) return text;
    const int n = static_cast<int>(std::min<size_t>(bytes.size(), INT_MAX));
    const int len = MultiByteToWideChar(cp, flags, bytes.data(), n, nullptr, 0);
    if (len <= 0) return text;
    text.resize(static_cast<size_t>(len));
    MultiByteToWideChar(cp, flags, bytes.data(), n, text.data(), len);
    return text;
}

}  // namespace

std::wstring DecodeText(std::string_view bytes, unsigned codePage) {
    const auto byteAt = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
    const bool le = bytes.size() >= 2 && byteAt(0) == 0xFF && byteAt(1) == 0xFE;
    const bool be = bytes.size() >= 2 && byteAt(0) == 0xFE && byteAt(1) == 0xFF;
    const bool utf8 = bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF";
    if (codePage == 1200 || codePage == 1201) return Utf16(bytes.substr(le || be ? 2 : 0), codePage == 1201);
    if (codePage != 0) return MultiByte(bytes.substr(utf8 ? 3 : 0), codePage, 0);
    if (le || be) return Utf16(bytes.substr(2), be);
    if (utf8) bytes.remove_prefix(3);
    if (bytes.empty()) return {};
    if (auto text = MultiByte(bytes, CP_UTF8, MB_ERR_INVALID_CHARS); !text.empty()) return text;
    return MultiByte(bytes, CP_ACP, 0);
}

std::wstring AddOffsetTag(std::wstring_view document, int addMs) {
    if (addMs == 0) return std::wstring(document);
    // Find an [offset:] line, and the end of the leading tag lines for a new one.
    int existing = 0;
    size_t tagStart = std::wstring_view::npos, tagEnd = 0, insertAt = 0;
    size_t pos = 0;
    bool inHeader = true;
    while (pos < document.size()) {
        size_t end = document.find(L'\n', pos);
        const size_t next = end == std::wstring_view::npos ? document.size() : end + 1;
        const std::wstring_view line = Trim(document.substr(pos, (end == std::wstring_view::npos ? document.size() : end) - pos));
        if (line.size() > 8 && line.front() == L'[' && line.back() == L']') {
            std::wstring key(line.substr(1, std::min<size_t>(7, line.size() - 2)));
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            if (key == L"offset:" && tagStart == std::wstring_view::npos) {
                existing = _wtoi(std::wstring(line.substr(8, line.size() - 9)).c_str());
                tagStart = pos;
                tagEnd = next;
            }
        }
        if (inHeader) {
            const bool tag = line.size() > 2 && line.front() == L'[' && ParseTime(line.substr(1, line.find(L']') - 1)) < 0 &&
                             line.find(L':') != std::wstring_view::npos;
            if (tag || line.empty())
                insertAt = next;
            else
                inHeader = false;
        }
        pos = next;
    }
    const std::wstring tag = L"[offset:" + std::to_wstring(existing + addMs) + L"]";
    std::wstring out(document);
    if (tagStart != std::wstring_view::npos) {
        const bool newline = tagEnd > tagStart && out[tagEnd - 1] == L'\n';
        const bool crlf = newline && tagEnd - tagStart >= 2 && out[tagEnd - 2] == L'\r';
        out.replace(tagStart, tagEnd - tagStart, tag + (crlf ? L"\r\n" : newline ? L"\n" : L""));
    } else {
        const bool crlf = document.find(L"\r\n") != std::wstring_view::npos;
        out.insert(insertAt, tag + (crlf ? L"\r\n" : L"\n"));
    }
    return out;
}

namespace {

std::wstring Stamp(int ms, wchar_t open, wchar_t close) {
    ms = std::max(0, ms);
    wchar_t buffer[32];
    swprintf_s(buffer, L"%c%02d:%02d.%02d%c", open, ms / 60000, ms / 1000 % 60, ms / 10 % 100, close);
    return buffer;
}

}  // namespace

std::wstring WriteLrc(const Lyrics& lyrics, const WriteOptions& options) {
    std::wstring out;
    const auto add = [&out](const std::wstring& line) {
        out += line;
        out += L"\r\n";
    };
    if (options.metadata) {
        if (!lyrics.title.empty()) add(L"[ti:" + lyrics.title + L"]");
        if (!lyrics.artist.empty()) add(L"[ar:" + lyrics.artist + L"]");
        if (!lyrics.album.empty()) add(L"[al:" + lyrics.album + L"]");
    }
    const bool timed = options.timestamps && lyrics.synced;
    const bool words = timed && !options.standard;
    // The text a line is written with (and merged by): its words with their times, or plain.
    const auto body = [&](const Line& l) {
        if (!words || l.words.empty()) return l.text;
        std::wstring s;
        for (const auto& w : l.words) s += Stamp(w.start, L'<', L'>') + w.text;
        return s + Stamp(l.end, L'<', L'>');
    };
    std::vector<bool> written(lyrics.lines.size());
    for (size_t i = 0; i < lyrics.lines.size(); ++i) {
        if (written[i]) continue;
        const Line& l = lyrics.lines[i];
        std::wstring stamps = timed ? Stamp(l.start, L'[', L']') : std::wstring();
        // Merged: later lines with the same text (and references, when they are written)
        // join this one; word-timed lines keep their own times and are never merged.
        if (timed && options.mergeSame && (!words || l.words.empty())) {
            for (size_t j = i + 1; j < lyrics.lines.size(); ++j) {
                const Line& m = lyrics.lines[j];
                if (written[j] || m.text != l.text || (words && !m.words.empty())) continue;
                if (options.references && (m.translation != l.translation || m.romanization != l.romanization)) continue;
                stamps += Stamp(m.start, L'[', L']');
                written[j] = true;
            }
        }
        add(stamps + body(l));
        if (options.references) {
            for (const auto* reference : {&l.translation, &l.romanization})
                if (!reference->empty()) add(stamps + *reference);
        }
    }
    return out;
}

namespace {

// Ideographs, kana, hangul and their punctuation are sung (and filled) one by one.
bool SingleUnit(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}

// The translation cut into words (with their trailing spaces) and single CJK characters, each
// with a weight for its share of the time: a character 1, a word a little more per letter.
std::vector<std::pair<std::wstring, float>> Units(const std::wstring& text) {
    std::vector<std::pair<std::wstring, float>> units;
    size_t i = 0;
    while (i < text.size()) {
        std::wstring unit;
        int letters = 0;
        if (SingleUnit(text[i])) {
            unit = text[i++];
            letters = 1;
        } else {
            while (i < text.size() && !SingleUnit(text[i]) && !iswspace(text[i])) {
                unit += text[i++];
                ++letters;
            }
        }
        while (i < text.size() && iswspace(text[i])) unit += text[i++];
        if (unit.empty()) continue;
        if (letters == 0 && !units.empty()) { // leading spaces join the unit before
            units.back().first += unit;
            continue;
        }
        units.push_back({std::move(unit), letters <= 1 ? 1.0f : 1.0f + (letters - 1) * 0.3f});
    }
    return units;
}

// Times the translation's units over the original's sung time: a share of the units' weight is
// the same share of the time the original's words take, gaps between words left out.
std::vector<Word> SpreadWords(const std::wstring& text, const std::vector<Word>& original) {
    std::vector<Word> words;
    const auto units = Units(text);
    float total = 0;
    for (const auto& u : units) total += u.second;
    long long voiced = 0;
    for (const auto& w : original) voiced += std::max(0, w.end - w.start);
    if (units.empty() || total <= 0 || voiced <= 0) return words;
    const auto at = [&](double share) { // the time at a share of the voiced time
        double left = share * static_cast<double>(voiced);
        for (const auto& w : original) {
            const int d = std::max(0, w.end - w.start);
            if (left <= d) return w.start + static_cast<int>(std::lround(left));
            left -= d;
        }
        return original.back().end;
    };
    float done = 0;
    for (const auto& [unit, weight] : units) {
        Word w;
        w.text = unit;
        w.start = at(done / total);
        done += weight;
        w.end = std::max(w.start, at(done / total));
        words.push_back(std::move(w));
    }
    return words;
}

std::array<TrackKind, 3> TrackSequence(DisplayOrder order) {
    switch (order) {
    case DisplayOrder::OriginalPronunciationTranslation:
        return {TrackKind::Original, TrackKind::Pronunciation, TrackKind::Translation};
    case DisplayOrder::OriginalTranslationPronunciation:
        return {TrackKind::Original, TrackKind::Translation, TrackKind::Pronunciation};
    case DisplayOrder::TranslationOriginalPronunciation:
        return {TrackKind::Translation, TrackKind::Original, TrackKind::Pronunciation};
    case DisplayOrder::TranslationPronunciationOriginal:
        return {TrackKind::Translation, TrackKind::Pronunciation, TrackKind::Original};
    case DisplayOrder::PronunciationOriginalTranslation:
        return {TrackKind::Pronunciation, TrackKind::Original, TrackKind::Translation};
    case DisplayOrder::PronunciationTranslationOriginal:
        return {TrackKind::Pronunciation, TrackKind::Translation, TrackKind::Original};
    }
    return {TrackKind::Original, TrackKind::Pronunciation, TrackKind::Translation};
}

bool TrackEnabled(const DisplayOptions& options, TrackKind kind) {
    switch (kind) {
    case TrackKind::Original: return options.showOriginal;
    case TrackKind::Translation: return options.showTranslation;
    case TrackKind::Pronunciation: return options.showPronunciation;
    }
    return false;
}

const std::wstring& TrackText(const Line& line, TrackKind kind) {
    switch (kind) {
    case TrackKind::Original: return line.text;
    case TrackKind::Translation: return line.translation;
    case TrackKind::Pronunciation: return line.romanization;
    }
    return line.text;
}

}  // namespace

Lyrics ProjectLyrics(const Lyrics& lyrics, const DisplayOptions& options) {
    Lyrics out = lyrics;
    const bool hasOriginal = std::any_of(lyrics.lines.begin(), lyrics.lines.end(),
                                         [](const Line& l) { return !l.text.empty(); });
    const bool hasTranslation = std::any_of(lyrics.lines.begin(), lyrics.lines.end(),
                                            [](const Line& l) { return !l.translation.empty(); });
    const bool hasPronunciation = std::any_of(lyrics.lines.begin(), lyrics.lines.end(),
                                              [](const Line& l) { return !l.romanization.empty(); });
    const auto songHas = [&](TrackKind kind) {
        switch (kind) {
        case TrackKind::Original: return hasOriginal;
        case TrackKind::Translation: return hasTranslation;
        case TrackKind::Pronunciation: return hasPronunciation;
        }
        return false;
    };

    std::vector<TrackKind> visible;
    for (TrackKind kind : TrackSequence(options.order)) {
        if (TrackEnabled(options, kind) && songHas(kind)) visible.push_back(kind);
    }
    if (visible.empty()) visible.push_back(TrackKind::Original);

    out.karaoke = false;
    for (size_t i = 0; i < out.lines.size(); ++i) {
        const Line& src = lyrics.lines[i];
        Line& dst = out.lines[i];
        dst.secondaries.clear();

        std::vector<TrackKind> lineTracks;
        lineTracks.reserve(visible.size());
        for (TrackKind kind : visible) {
            if (!TrackText(src, kind).empty()) lineTracks.push_back(kind);
        }
        if (lineTracks.empty()) lineTracks.push_back(TrackKind::Original);

        const TrackKind mainKind = lineTracks.front();
        dst.mainKind = mainKind;
        dst.text = TrackText(src, mainKind);
        if (mainKind == TrackKind::Original) {
            dst.words = src.words;
        } else if (mainKind == TrackKind::Pronunciation) {
            if (!src.romanWords.empty())
                dst.words = src.romanWords;
            else
                dst.words = src.words.empty() ? std::vector<Word>() : SpreadWords(src.romanization, src.words);
        } else {
            dst.words = src.words.empty() ? std::vector<Word>() : SpreadWords(src.translation, src.words);
        }
        if (!dst.words.empty()) out.karaoke = true;

        for (size_t s = 1; s < lineTracks.size(); ++s) {
            const TrackKind subKind = lineTracks[s];
            SecondaryLine sec;
            sec.kind = subKind;
            sec.text = TrackText(src, subKind);
            if (subKind == TrackKind::Original)
                sec.words = src.words;
            else if (subKind == TrackKind::Pronunciation)
                sec.words = src.romanWords;
            dst.secondaries.push_back(std::move(sec));
        }
    }
    return out;
}

}  // namespace glasswave::lyrics
