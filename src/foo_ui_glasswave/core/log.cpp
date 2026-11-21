#include "pch.h"
#include "core/log.h"
#include "core/text.h"
#include "core/paths.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace glasswave::core {

namespace {

std::mutex g_logMutex;

std::wstring Stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    return buf;
}

std::filesystem::path FilePath() {
    const auto dir = ProfileRoot();
    return dir.empty() ? std::filesystem::path() : dir / L"glasswave.log";
}

constexpr uintmax_t kRotateBytes = 2u * 1024u * 1024u;

// The log stays open: a line costs one buffered write and a flush, not a directory check,
// a size query and an open/close of the file. Guarded by g_logMutex.
std::ofstream g_out;
uintmax_t g_size = 0;

bool OpenLocked() {
    if (g_out.is_open()) return true;
    const auto path = FilePath();
    if (path.empty() || !EnsureDir(path.parent_path())) return false;
    std::error_code ec;
    g_size = std::filesystem::file_size(path, ec);
    if (ec) g_size = 0;
    if (g_size >= kRotateBytes) {
        auto old = path;
        old += L".1";
        std::filesystem::remove(old, ec);
        std::filesystem::rename(path, old, ec);
        g_size = 0;
    }
    g_out.open(path, std::ios::binary | std::ios::app);
    return g_out.is_open();
}

void Write(std::wstring_view message) {
    const std::string line = Utf8(Stamp() + L"  ") + Utf8(message) + "\r\n";
    std::lock_guard lock(g_logMutex);
    if (!OpenLocked()) return;
    g_out.write(line.data(), static_cast<std::streamsize>(line.size()));
    g_out.flush();
    g_size += line.size();
    if (!g_out || g_size >= kRotateBytes) { // reopened, and rotated when due, by the next line
        g_out.close();
        g_out.clear();
    }
}

} // namespace

void Log(std::wstring_view message) {
    Write(message);
}

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

} // namespace glasswave::core
