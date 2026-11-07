#pragma once
// Text encoding detection for BOM-less cue sheets (pure Win32 + ICU, no foobar2000 types,
// so it can be unit-tested on its own).
//
// Cue sheets are short and mostly ASCII, which makes statistical detection shaky between
// Shift-JIS, GBK/GB18030 and Big5. The ranking combines:
//   * ICU charset detection confidence (icu.dll, part of Windows 10 1903+),
//   * per-line validity: a line with invalid byte sequences for a code page costs points,
//     so one stray byte does not disqualify an otherwise clean decoding,
//   * script plausibility: real kana for Japanese readings; half-width katakana or Hangul
//     outside Korean are typical signs of a wrong decoding,
//   * a small tie-break toward the system ANSI code page.
// Single-byte Western code pages are only a last resort.

#include <windows.h>
#include <icu.h>
#include <algorithm>
#include <string>
#include <vector>

namespace glasswave::media::cue_encoding {

inline bool IsMultiByteStrictCp(UINT cp) {
    return cp == CP_UTF8 || cp == 932 || cp == 936 || cp == 950 || cp == 949 || cp == 54936 || cp == 51949 ||
           cp == 20932;
}

inline bool DecodeStrict(const std::string& bytes, UINT cp, std::wstring& out) {
    out.clear();
    if (bytes.empty()) return true;
    const DWORD flags = IsMultiByteStrictCp(cp) ? MB_ERR_INVALID_CHARS : 0;
    const int n = MultiByteToWideChar(cp, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(n);
    MultiByteToWideChar(cp, flags, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
    return true;
}

// Line-by-line decoding: valid lines decode exactly, invalid ones leniently.
inline std::wstring DecodeLenient(const std::string& bytes, UINT cp, int* badLines = nullptr) {
    std::wstring out;
    int bad = 0;
    size_t pos = 0;
    while (pos <= bytes.size()) {
        size_t end = bytes.find('\n', pos);
        const bool last = end == std::string::npos;
        if (last) end = bytes.size();
        const std::string line = bytes.substr(pos, end - pos);
        std::wstring decoded;
        if (!DecodeStrict(line, cp, decoded)) {
            ++bad;
            const int n = MultiByteToWideChar(cp, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
            decoded.resize(std::max(n, 0));
            if (n > 0) MultiByteToWideChar(cp, 0, line.data(), static_cast<int>(line.size()), decoded.data(), n);
        }
        out += decoded;
        if (last) break;
        out += L'\n';
        pos = end + 1;
    }
    if (badLines) *badLines = bad;
    return out;
}

inline bool IsAscii(const std::string& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

inline int CountRange(const std::wstring& s, wchar_t lo, wchar_t hi) {
    return static_cast<int>(std::count_if(s.begin(), s.end(), [=](wchar_t c) { return c >= lo && c <= hi; }));
}

struct Candidate {
    UINT codepage = 0;
    int score = 0;
};

inline UINT CodepageFromIcuName(const char* name) {
    struct Map {
        const char* icu;
        UINT cp;
    };
    static constexpr Map kMap[] = {
        {"Shift_JIS", 932}, {"GB18030", 54936}, {"Big5", 950}, {"EUC-JP", 20932}, {"EUC-KR", 51949}};
    for (const auto& m : kMap)
        if (_stricmp(m.icu, name) == 0) return m.cp;
    return 0;
}

// Ranked candidates for a BOM-less, non-UTF-8 text, best first. Never empty.
inline std::vector<Candidate> Rank(const std::string& bytes) {
    std::vector<Candidate> c = {{932, 0}, {54936, 0}, {950, 0}, {20932, 0}, {51949, 0}};

    UErrorCode status = U_ZERO_ERROR;
    if (UCharsetDetector* det = ucsdet_open(&status); U_SUCCESS(status) && det) {
        ucsdet_setText(det, bytes.data(), static_cast<int32_t>(bytes.size()), &status);
        int32_t count = 0;
        const UCharsetMatch** matches = ucsdet_detectAll(det, &count, &status);
        for (int32_t i = 0; U_SUCCESS(status) && i < count; ++i) {
            UErrorCode s2 = U_ZERO_ERROR;
            const UINT cp = CodepageFromIcuName(ucsdet_getName(matches[i], &s2));
            const int confidence = ucsdet_getConfidence(matches[i], &s2);
            for (auto& x : c)
                if (x.codepage == cp && U_SUCCESS(s2)) x.score = std::max(x.score, confidence);
        }
        ucsdet_close(det);
    }

    // Lines that contain non-ASCII bytes: the denominator for "how much is broken".
    int nonAsciiLines = 0;
    {
        size_t pos = 0;
        while (pos < bytes.size()) {
            size_t end = bytes.find('\n', pos);
            if (end == std::string::npos) end = bytes.size();
            if (!IsAscii(bytes.substr(pos, end - pos))) ++nonAsciiLines;
            pos = end + 1;
        }
    }

    // Double-byte pairs outside GB2312 (lead A1-F7, trail A1-FE). Real Chinese text sits
    // almost entirely inside GB2312; Shift-JIS bytes read as GBK land in the rare GBK
    // extension area instead (e.g. 堀江由衣 -> 杧峕桼堖).
    int outsideGb2312 = 0;
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x81) continue;
        const auto trail = static_cast<unsigned char>(bytes[i + 1]);
        if (!(lead >= 0xA1 && lead <= 0xF7 && trail >= 0xA1 && trail <= 0xFE)) ++outsideGb2312;
        ++i;
    }

    const UINT acp = GetACP();
    for (auto& x : c) {
        int bad = 0;
        const std::wstring text = DecodeLenient(bytes, x.codepage, &bad);
        if (nonAsciiLines > 0 && bad * 3 > nonAsciiLines) {  // more than a third broken: not it
            x.score = -1000;
            continue;
        }
        const int kana = CountRange(text, 0x3041, 0x30FA);
        const int halfKana = CountRange(text, 0xFF61, 0xFF9F);
        const int hangul = CountRange(text, 0xAC00, 0xD7A3);
        const int privateUse = CountRange(text, 0xE000, 0xF8FF);
        // Real kana is a strong sign of a right reading (Japanese titles in any encoding);
        // the Japanese code pages get the larger share.
        if (kana >= 2) x.score += (x.codepage == 932 || x.codepage == 20932) ? 30 : 10;
        if (x.codepage == 54936) x.score -= 3 * outsideGb2312;
        x.score -= 4 * privateUse;
        x.score -= 4 * halfKana;
        if (x.codepage != 51949) x.score -= 4 * hangul;
        x.score -= 15 * bad;
        if (x.codepage == acp || (x.codepage == 54936 && acp == 936)) x.score += 5;
    }
    std::stable_sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    c.erase(std::remove_if(c.begin(), c.end(), [](const Candidate& x) { return x.score <= -1000; }), c.end());
    if (c.empty()) c.push_back({GetACP(), 0});  // nothing CJK fits: system ANSI as a last resort
    return c;
}

}  // namespace glasswave::media::cue_encoding
