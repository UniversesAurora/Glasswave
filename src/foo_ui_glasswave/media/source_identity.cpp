#include "pch.h"
#include "media/source_identity.h"
#include "core/text.h"
#include "media/cue_encoding.h"
#include "media/library_model.h"
#include "ui/text.h"

namespace glasswave::media {

namespace {

namespace enc = cue_encoding;

bool EndsWithI(const std::wstring& s, std::wstring_view suffix) {
    return s.size() >= suffix.size() &&
           _wcsnicmp(s.c_str() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

std::wstring DirOf(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring StemOf(const std::wstring& name) {
    const size_t p = name.find_last_of(L'.');
    return p == std::wstring::npos ? name : name.substr(0, p);
}

bool Exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Resolve a cue FILE reference the way foobar2000 does: the named file, or a file with the
// same stem and another extension (cue sheets often still say .wav after re-encoding).
std::wstring ResolveAudio(const std::wstring& cueDir, const std::wstring& name) {
    std::wstring direct = cueDir + L'\\' + name;
    if (Exists(direct)) return direct;
    const std::wstring stem = StemOf(FileNameOf(name));
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((cueDir + L'\\' + stem + L".*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    static constexpr const wchar_t* kNotAudio[] = {L".cue", L".log", L".txt", L".jpg", L".jpeg",
                                                   L".png", L".webp", L".accurip", L".m3u", L".m3u8"};
    do {
        const std::wstring candidate = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (std::any_of(std::begin(kNotAudio), std::end(kNotAudio),
                        [&](const wchar_t* ext) { return EndsWithI(candidate, ext); }))
            continue;
        if (_wcsicmp(StemOf(candidate).c_str(), stem.c_str()) == 0) {
            found = cueDir + L'\\' + candidate;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::wstring Unquote(const std::wstring& rest) {
    const size_t q1 = rest.find(L'"');
    if (q1 != std::wstring::npos) {
        const size_t q2 = rest.find(L'"', q1 + 1);
        return rest.substr(q1 + 1, q2 == std::wstring::npos ? std::wstring::npos : q2 - q1 - 1);
    }
    std::wstring s = rest;
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}

bool StartsWithI(const std::wstring& s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.data(), prefix.size()) == 0;
}

}  // namespace

std::wstring NativePath(const char* fbPath) {
    static constexpr char kRelative[] = "file-relative://";
    if (strncmp(fbPath, kRelative, sizeof(kRelative) - 1) == 0) {
        // Portable installs store library paths relative to the foobar2000 folder.
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring joined = DirOf(exe) + L'\\' + core::Wide(fbPath + sizeof(kRelative) - 1);
        wchar_t full[4096]{};
        if (GetFullPathNameW(joined.c_str(), 4096, full, nullptr)) return full;
        return joined;
    }
    pfc::string8 native;
    if (filesystem::g_get_native_path(fbPath, native) && strstr(native.c_str(), "://") == nullptr)
        return core::Wide(native);
    return {};
}

CueCatalog& CueCatalog::Get() {
    static CueCatalog instance;
    return instance;
}

void CueCatalog::Clear() {
    m_cues.clear();
    m_splitImages.clear();
}

CueCatalog::CueInfo& CueCatalog::Cue(const std::wstring& nativePath) {
    auto [it, inserted] = m_cues.try_emplace(Lower(nativePath));
    CueInfo& info = it->second;
    if (!inserted) return info;
    info.stem = Lower(StemOf(StemOf(FileNameOf(nativePath))));  // "X.ape.cue" -> "x"

    std::string bytes;
    {
        HANDLE f = CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return info;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
            bytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
            bytes.resize(read);
        }
        CloseHandle(f);
    }
    info.utf8Bom = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                   static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF;
    if (info.utf8Bom) bytes.erase(0, 3);

    // How foobar2000 reads it: UTF-8 with BOM or valid UTF-8 as UTF-8, otherwise system ANSI.
    std::wstring probe;
    const bool validUtf8 = enc::DecodeStrict(bytes, CP_UTF8, probe);
    std::vector<UINT> order;
    if (info.utf8Bom || validUtf8 || enc::IsAscii(bytes)) {
        order.push_back(CP_UTF8);
    } else {
        for (const auto& c : enc::Rank(bytes)) order.push_back(c.codepage);
    }

    // Take the best-ranked decoding whose FILE references exist on disk.
    const std::wstring dir = DirOf(nativePath);
    UINT chosen = 0;
    std::wstring chosenText;
    for (UINT cp : order) {
        std::wstring text = cp == CP_UTF8 ? probe : enc::DecodeLenient(bytes, cp);
        if (chosen == 0) {  // fallback: best-ranked even if FILE does not resolve
            chosen = cp;
            chosenText = text;
        }
        bool anyFile = false, allResolved = true;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find_first_of(L"\r\n", pos);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(pos, end - pos);
            pos = end + 1;
            const size_t first = line.find_first_not_of(L" \t");
            if (first == std::wstring::npos) continue;
            line.erase(0, first);
            if (StartsWithI(line, L"FILE ")) {
                std::wstring rest = line.substr(5);
                // drop the trailing file type (WAVE / BINARY / MP3 ...) of unquoted names
                if (rest.find(L'"') == std::wstring::npos) {
                    const size_t sp = rest.find_last_of(L' ');
                    if (sp != std::wstring::npos) rest.erase(sp);
                }
                anyFile = true;
                if (ResolveAudio(dir, Unquote(rest)).empty()) allResolved = false;
            }
        }
        if (anyFile && allResolved) {
            chosen = cp;
            chosenText = std::move(text);
            break;
        }
    }

    // Parse the chosen decoding.
    std::wstring currentFile;
    int track = 0;
    size_t pos = 0;
    while (pos < chosenText.size()) {
        size_t end = chosenText.find_first_of(L"\r\n", pos);
        if (end == std::wstring::npos) end = chosenText.size();
        std::wstring line = chosenText.substr(pos, end - pos);
        pos = end + 1;
        const size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        line.erase(0, first);
        if (StartsWithI(line, L"FILE ")) {
            std::wstring rest = line.substr(5);
            if (rest.find(L'"') == std::wstring::npos) {
                const size_t sp = rest.find_last_of(L' ');
                if (sp != std::wstring::npos) rest.erase(sp);
            }
            currentFile = Lower(ResolveAudio(dir, Unquote(rest)));
        } else if (StartsWithI(line, L"TRACK ")) {
            track = _wtoi(line.c_str() + 6);
            if (track > 0 && !currentFile.empty()) {
                info.trackFile[track] = currentFile;
                ++info.tracksPerFile[currentFile];
            }
        } else if (StartsWithI(line, L"TITLE ")) {
            (track > 0 ? info.text[track].title : info.albumTitle) = Unquote(line.substr(6));
        } else if (StartsWithI(line, L"PERFORMER ")) {
            (track > 0 ? info.text[track].performer : info.albumPerformer) = Unquote(line.substr(10));
        } else if (track == 0 && StartsWithI(line, L"REM DATE ")) {
            info.date = Unquote(line.substr(9)).substr(0, 4);
        } else if (track == 0 && StartsWithI(line, L"REM GENRE ")) {
            info.genre = Unquote(line.substr(10));
        }
    }
    for (const auto& [file, count] : info.tracksPerFile)
        if (count >= 2) m_splitImages.insert(file);

    // foobar2000 got the text wrong if the real encoding is not what it used (ANSI).
    const UINT acp = GetACP();
    const bool sameAsAnsi = chosen == acp || (chosen == 54936 && acp == 936);
    info.needsRepair = !info.utf8Bom && chosen != CP_UTF8 && chosen != 0 && !sameAsAnsi;
    return info;
}

SourceIdentity CueCatalog::Identify(const metadb_handle_ptr& handle) {
    const char* fbPath = handle->get_path();
    const std::wstring native = NativePath(fbPath);
    const t_uint32 subsong = handle->get_subsong_index();
    SourceIdentity id;
    if (native.empty()) {  // non-file sources (streams, archives): identity is the path itself
        id.key = Lower(core::Wide(fbPath)) + L'|' + std::to_wstring(subsong);
        return id;
    }
    if (!EndsWithI(native, L".cue")) {
        id.key = Lower(native) + L'|' + std::to_wstring(subsong);
        id.preference = 0;  // the audio file itself (embedded tags / embedded cue sheet)
        return id;
    }
    const CueInfo& cue = Cue(native);
    auto it = cue.trackFile.find(static_cast<int>(subsong));
    if (it == cue.trackFile.end()) {
        id.key = Lower(native) + L'|' + std::to_wstring(subsong);
        id.preference = 4;
        return id;
    }
    // One file per track: the cue entry stands for the whole file (subsong 0).
    const auto per = cue.tracksPerFile.find(it->second);
    const bool wholeFile = per != cue.tracksPerFile.end() && per->second == 1 && cue.tracksPerFile.size() > 1;
    id.key = it->second + L'|' + (wholeFile ? std::wstring(L"0") : std::to_wstring(subsong));
    // Prefer cues foobar2000 decodes correctly, then one named after its audio file;
    // mis-decoded or renamed copies come last.
    const std::wstring audioStem = Lower(StemOf(FileNameOf(it->second)));
    id.preference = cue.needsRepair ? 3 : (cue.utf8Bom ? 1 : (cue.stem == audioStem ? 1 : 2));
    return id;
}

bool CueCatalog::IsSplitImage(const metadb_handle_ptr& handle) {
    if (handle->get_subsong_index() != 0) return false;
    const std::wstring native = NativePath(handle->get_path());
    if (native.empty() || EndsWithI(native, L".cue")) return false;
    return m_splitImages.count(Lower(native)) != 0;
}

const RepairedText* CueCatalog::Repair(const metadb_handle_ptr& handle) {
    const std::wstring native = NativePath(handle->get_path());
    if (native.empty() || !EndsWithI(native, L".cue")) return nullptr;
    CueInfo& cue = Cue(native);
    if (!cue.needsRepair) return nullptr;
    const int track = static_cast<int>(handle->get_subsong_index());
    auto [it, inserted] = cue.repaired.try_emplace(track);
    if (inserted) {
        RepairedText& r = it->second;
        const auto t = cue.text.find(track);
        r.title = t != cue.text.end() ? t->second.title : std::wstring();
        r.artist = (t != cue.text.end() && !t->second.performer.empty()) ? t->second.performer : cue.albumPerformer;
        r.album = cue.albumTitle;
        r.albumArtist = cue.albumPerformer;
        r.genre = cue.genre;
        r.year = cue.date;
    }
    return &it->second;
}

}  // namespace glasswave::media
