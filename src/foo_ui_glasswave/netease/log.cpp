#include "pch.h"
#include "netease/log.h"
#include "netease/profile.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace glasswave::netease {

namespace {

std::mutex g_logMutex;

std::string Utf8(std::wstring_view s) {
    return pfc::stringcvt::string_utf8_from_wide(s.data(), s.size()).get_ptr();
}

std::wstring Stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    return buf;
}

std::filesystem::path FilePath() {
    const auto dir = ProfileDir();
    return dir.empty() ? std::filesystem::path() : dir / L"glasswave-netease.log";
}

void RotateIfNeeded(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 2u * 1024u * 1024u) return;
    auto old = path;
    old += L".1";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(path, old, ec);
}

void Write(std::wstring_view message) {
    const auto path = FilePath();
    if (path.empty() || !EnsureDir(path.parent_path())) return;
    std::lock_guard lock(g_logMutex);
    RotateIfNeeded(path);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return;
    const std::string line = Utf8(Stamp() + L"  ") + Utf8(message) + "\r\n";
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace

void Log(std::wstring_view message) { Write(message); }

void LogHttp(std::wstring_view method, std::wstring_view path, unsigned long status, long long elapsedMs) {
    std::wstring line(method);
    line += L" ";
    line += path;
    line += L" -> ";
    line += std::to_wstring(status);
    line += L" (";
    line += std::to_wstring(elapsedMs);
    line += L" ms)";
    Write(line);
}

std::wstring LogPath() {
    const auto path = FilePath();
    return path.empty() ? std::wstring() : path.wstring();
}

}  // namespace glasswave::netease
