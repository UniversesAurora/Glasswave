#include "pch.h"
#include "cd/disc.h"

#include <bcrypt.h>
#include <winioctl.h>
#include <ntddcdrm.h>

#include <cmath>
#include <cstdio>
#include <set>

namespace glasswave::cd {

namespace {

constexpr uint32_t kSessionGap = 11400;    // lead-out + lead-in + pregap between sessions
constexpr unsigned char kDataTrack = 0x04; // CONTROL bit: a data track

uint32_t Lba(const TRACK_DATA& track) {
    return (static_cast<uint32_t>(track.Address[1]) * 60 + track.Address[2]) * 75 + track.Address[3];
}

std::string Sha1(const std::string& text) {
    std::string digest(20, '\0');
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0,
                   reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()),
                   reinterpret_cast<PUCHAR>(digest.data()), 20) != 0)
        return {};
    return digest;
}

} // namespace

double Toc::TrackSeconds(int track) const {
    if (track < first || track > last) return 0;
    const size_t i = static_cast<size_t>(track - first);
    const uint32_t end = i + 1 < offsets.size() ? offsets[i + 1] : leadout;
    return (end - offsets[i]) / 75.0;
}

bool IsCdPath(const char* path) {
    return path && _strnicmp(path, "cdda://", 7) == 0;
}

std::string DiscKey(const char* path) {
    if (!IsCdPath(path)) return {};
    const std::string_view text(path);
    return std::string(text.substr(0, text.find(',')));
}

unsigned TrackNumber(const char* path, unsigned subsong) {
    if (subsong > 0 || !IsCdPath(path)) return subsong;
    const char* comma = strchr(path, ',');
    return comma ? static_cast<unsigned>(atoi(comma + 1)) : 0;
}

namespace {
std::set<std::string> g_present; // main thread
} // namespace

bool DiscPresent(const std::string& disc) {
    return g_present.contains(disc);
}

void SetDiscPresent(const std::string& disc, bool present) {
    if (present)
        g_present.insert(disc);
    else
        g_present.erase(disc);
}

std::vector<std::string> PresentDiscs() {
    return {g_present.begin(), g_present.end()};
}

std::vector<wchar_t> CdDrives() {
    std::vector<wchar_t> drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_CDROM) drives.push_back(root[0]);
    }
    return drives;
}

std::optional<Toc> ReadToc(wchar_t drive) {
    const wchar_t device[] = {L'\\', L'\\', L'.', L'\\', drive, L':', 0};
    HANDLE handle = CreateFileW(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    CDROM_TOC raw{};
    DWORD read = 0;
    const BOOL ok =
        DeviceIoControl(handle, IOCTL_CDROM_READ_TOC, nullptr, 0, &raw, sizeof(raw), &read, nullptr);
    CloseHandle(handle);
    if (!ok || raw.LastTrack < raw.FirstTrack) return std::nullopt;

    const int count = raw.LastTrack - raw.FirstTrack + 1; // TrackData[count] is the lead-out
    if (count + 1 > MAXIMUM_NUMBER_TRACKS) return std::nullopt;
    Toc toc;
    toc.drive = drive;
    toc.leadout = Lba(raw.TrackData[count]); // MSF addresses count the 2 s pregap
    // An enhanced CD ends with a data session: the audio ends a session gap before it.
    int lastAudio = count - 1;
    if (lastAudio > 0 && (raw.TrackData[lastAudio].Control & kDataTrack)) {
        toc.leadout = Lba(raw.TrackData[lastAudio]) - kSessionGap;
        --lastAudio;
    }
    int firstAudio = 0;
    while (firstAudio <= lastAudio && (raw.TrackData[firstAudio].Control & kDataTrack))
        ++firstAudio;
    if (firstAudio > lastAudio) return std::nullopt; // a data disc
    toc.first = raw.TrackData[firstAudio].TrackNumber;
    toc.last = raw.TrackData[lastAudio].TrackNumber;
    for (int i = firstAudio; i <= lastAudio; ++i)
        toc.offsets.push_back(Lba(raw.TrackData[i]));
    return toc;
}

std::optional<Toc> FindDisc(unsigned subsong, double seconds) {
    std::vector<Toc> discs;
    for (const wchar_t drive : CdDrives())
        if (auto toc = ReadToc(drive)) discs.push_back(std::move(*toc));
    for (const auto& toc : discs)
        if (std::abs(toc.TrackSeconds(static_cast<int>(subsong)) - seconds) < 1.0) return toc;
    if (discs.size() == 1) return discs.front(); // lengths may differ by a pregap
    return std::nullopt;
}

std::wstring MusicBrainzDiscId(const Toc& toc) {
    char hex[16];
    std::string text;
    std::snprintf(hex, sizeof(hex), "%02X", toc.first);
    text += hex;
    std::snprintf(hex, sizeof(hex), "%02X", toc.last);
    text += hex;
    std::snprintf(hex, sizeof(hex), "%08X", toc.leadout);
    text += hex;
    for (int track = 1; track < 100; ++track) {
        const int i = track - toc.first;
        const uint32_t offset = i >= 0 && static_cast<size_t>(i) < toc.offsets.size() ? toc.offsets[i] : 0;
        std::snprintf(hex, sizeof(hex), "%08X", offset);
        text += hex;
    }
    const std::string digest = Sha1(text);
    if (digest.size() != 20) return {};
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    std::wstring id;
    for (size_t i = 0; i < digest.size(); i += 3) {
        const auto byte = [&](size_t at) {
            return at < digest.size() ? static_cast<unsigned char>(digest[at]) : 0u;
        };
        const uint32_t chunk = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        const size_t have = std::min<size_t>(3, digest.size() - i);
        for (size_t k = 0; k < 4; ++k)
            id += k <= have ? static_cast<wchar_t>(kAlphabet[(chunk >> (18 - 6 * k)) & 63]) : L'-';
    }
    return id;
}

std::wstring TocQuery(const Toc& toc) {
    std::wstring query =
        std::to_wstring(toc.first) + L"+" + std::to_wstring(toc.last) + L"+" + std::to_wstring(toc.leadout);
    for (const uint32_t offset : toc.offsets)
        query += L"+" + std::to_wstring(offset);
    return query;
}

} // namespace glasswave::cd
