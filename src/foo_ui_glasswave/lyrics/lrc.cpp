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

std::wstring_view Trim(std::wstring_view s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == 0xFEFF)) s.remove_prefix(1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r')) s.remove_suffix(1);
    return s;
}

struct Entry {
    int time = 0;
    size_t order = 0;  // position in the file, keeps the original before its references
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
        const std::wstring_view body = line.substr(at);
        for (int t : times) {
            Entry e;
            e.time = t;
            e.order = order++;
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

    // Group by timestamp: the first non-empty entry is the line, the next ones its
    // references. Empty entries end the line before them (instrumental gaps).
    std::vector<int> gapStarts;
    for (size_t i = 0; i < entries.size();) {
        size_t j = i;
        while (j < entries.size() && entries[j].time == entries[i].time) ++j;
        Line* line = nullptr;
        Line current;
        int refs = 0;
        for (size_t k = i; k < j; ++k) {
            Entry& e = entries[k];
            if (Trim(e.text).empty()) continue;
            if (!line) {
                current.start = e.time;
                current.text = std::move(e.text);
                current.words = std::move(e.words);
                current.end = e.wordsEnd;
                line = &current;
            } else if (refs == 0) {
                current.translation = std::wstring(Trim(e.text));
                ++refs;
            } else if (refs == 1) {
                current.romanization = std::wstring(Trim(e.text));
                ++refs;
            }
        }
        if (line) out.lines.push_back(std::move(current));
        else gapStarts.push_back(entries[i].time);
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
    }

    if (offset != 0) {
        // A positive offset shows the lyrics earlier.
        auto shift = [offset](int& t) { t = std::max(0, t - offset); };
        for (auto& l : out.lines) {
            shift(l.start);
            shift(l.end);
            for (auto& w : l.words) {
                shift(w.start);
                shift(w.end);
            }
        }
    }
    return out;
}

std::wstring DecodeText(std::string_view bytes) {
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
        return text;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1])));
        return text;
    }
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") bytes.remove_prefix(3);
    if (bytes.empty()) return text;
    const int n = static_cast<int>(std::min<size_t>(bytes.size(), INT_MAX));
    UINT cp = CP_UTF8;
    int len = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), n, nullptr, 0);
    if (len <= 0) {
        cp = CP_ACP;
        len = MultiByteToWideChar(cp, 0, bytes.data(), n, nullptr, 0);
    }
    if (len <= 0) return text;
    text.resize(static_cast<size_t>(len));
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes.data(), n, text.data(), len);
    return text;
}

}  // namespace glasswave::lyrics
