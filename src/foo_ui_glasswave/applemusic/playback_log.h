#pragma once

#include <SDK/foobar2000.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::applemusic::playback_log {

namespace fs = std::filesystem;

inline constexpr std::uintmax_t kMaxBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxFieldBytes = 384u;

inline std::mutex& Mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

inline std::string Lower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

inline bool ContainsSensitiveMarker(std::string_view text) {
    static constexpr std::string_view markers[] = {
        "access_token", "authorization",    "bearer", "cookie", "credential",  "jwt",
        "password",     "music-user-token", "secret", "token",  "x-api-token",
    };
    const std::string lower = Lower(text);
    return std::any_of(std::begin(markers), std::end(markers),
                       [&](std::string_view marker) { return lower.find(marker) != std::string::npos; });
}

inline bool LooksLikeLongValue(std::string_view token) {
    if (token.size() < 40) return false;
    for (char c : token) {
        if (c == '.' || c == '_' || c == '-' || c == '+' || c == '/') {
            continue;
        }
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

inline bool LooksLikePath(std::string_view token) {
    return token.find(":/") != std::string_view::npos || token.find(":\\") != std::string_view::npos ||
           token.rfind("\\\\", 0) == 0;
}

inline std::string Sanitize(std::string_view raw) {
    if (raw.empty()) return "unspecified";
    if (raw.size() > 1024) return "<redacted-long-message>";
    if (ContainsSensitiveMarker(raw)) return "<redacted-sensitive>";
    if (raw.find_first_of("{}[]\r\n") != std::string_view::npos) return "<redacted-structured-data>";

    std::string out;
    out.reserve(std::min(raw.size(), kMaxFieldBytes));
    std::size_t offset = 0;
    while (offset < raw.size() && out.size() < kMaxFieldBytes) {
        while (offset < raw.size() && std::isspace(static_cast<unsigned char>(raw[offset])))
            ++offset;
        std::size_t end = offset;
        while (end < raw.size() && !std::isspace(static_cast<unsigned char>(raw[end])))
            ++end;
        std::string_view token = raw.substr(offset, end - offset);
        offset = end;
        if (token.empty()) continue;

        if (!out.empty()) out.push_back(' ');
        if (token.find("://") != std::string_view::npos || token.find('?') != std::string_view::npos ||
            LooksLikePath(token)) {
            out += "<redacted-url-or-path>";
        } else if (LooksLikeLongValue(token)) {
            out += "<redacted-long-value>";
        } else {
            for (char c : token) {
                if (out.size() >= kMaxFieldBytes) break;
                out.push_back(std::isprint(static_cast<unsigned char>(c)) ? c : ' ');
            }
        }
    }
    if (out.size() >= kMaxFieldBytes) {
        if (out.size() > kMaxFieldBytes - 3) out.resize(kMaxFieldBytes - 3);
        out += "...";
    }
    return out.empty() ? "unspecified" : out;
}

inline std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            result.data(), count, nullptr, nullptr) != count) {
        return {};
    }
    return result;
}

inline std::optional<fs::path> LogPath() noexcept {
    try {
        pfc::string8 native;
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return std::nullopt;
        const auto wide = pfc::stringcvt::string_wide_from_utf8(native.get_ptr());
        return fs::path(wide.get_ptr()) / L"glasswave" / L"applemusic" / L"playback.log";
    } catch (...) {
        return std::nullopt;
    }
}

inline std::string Timestamp() noexcept {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", static_cast<unsigned>(now.wYear),
                  static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
                  static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                  static_cast<unsigned>(now.wSecond), static_cast<unsigned>(now.wMilliseconds));
    return text;
}

inline void Write(std::wstring_view song_id, std::string_view event, std::string_view stage,
                  double decoded_seconds, std::string_view error_class = {},
                  std::string_view error = {}) noexcept {
    try {
        const std::optional<fs::path> path = LogPath();
        if (!path) return;

        std::lock_guard lock(Mutex());
        std::error_code ec;
        fs::create_directories(path->parent_path(), ec);
        if (ec) return;

        std::ostringstream line;
        line << Timestamp() << " event=" << event << " stage=" << stage
             << " song_id=" << Sanitize(WideToUtf8(song_id)) << " decoded_seconds=" << std::fixed
             << std::setprecision(3)
             << (std::isfinite(decoded_seconds) && decoded_seconds >= 0 ? decoded_seconds : 0.0);
        if (!error_class.empty()) line << " error_class=" << error_class;
        if (!error.empty()) line << " error=\"" << Sanitize(error) << '"';
        line << "\r\n";
        const std::string bytes = line.str();

        const std::uintmax_t current_size = fs::exists(*path, ec) && !ec ? fs::file_size(*path, ec) : 0;
        if ((ec || current_size + bytes.size() > kMaxBytes) && fs::exists(*path, ec)) {
            const fs::path rotated = path->wstring() + L".1";
            fs::remove(rotated, ec);
            ec.clear();
            fs::rename(*path, rotated, ec);
            if (ec) return;
        }

        std::ofstream output(*path, std::ios::binary | std::ios::app);
        if (!output) return;
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    } catch (...) {
    }
}

inline void Ready(std::wstring_view song_id, double decoded_seconds) noexcept {
    Write(song_id, "session_ready", "initialize", decoded_seconds);
}

inline void Skipped(std::wstring_view song_id, std::string_view reason) noexcept {
    Write(song_id, "playback_skipped", "decode_initialize", 0, "unavailable", reason);
}

inline void Complete(std::wstring_view song_id, double decoded_seconds) noexcept {
    Write(song_id, "playback_complete", "decode_run", decoded_seconds);
}

inline void Failure(std::wstring_view song_id, std::string_view stage, double decoded_seconds,
                    std::string_view error_class, std::string_view error) noexcept {
    Write(song_id, "playback_failure", stage, decoded_seconds, error_class, error);
}

inline void UnknownFailure(std::wstring_view song_id, std::string_view stage,
                           double decoded_seconds) noexcept {
    Write(song_id, "playback_failure", stage, decoded_seconds, "non_std_exception", "unknown exception");
}

} // namespace glasswave::applemusic::playback_log
