#include "pch.h"
#include "netease/profile.h"

namespace glasswave::netease {

namespace {

// Same profile root the Win7 artwork loader uses, without depending on it.
std::wstring NativeProfileFolder() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return {};
    } catch (...) {
        return {};
    }
    std::wstring path = pfc::stringcvt::string_wide_from_utf8(native.get_ptr()).get_ptr();
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + L"glasswave\\";
}

}  // namespace

std::filesystem::path ProfileDir() {
    const std::wstring base = NativeProfileFolder();
    if (base.empty()) return {};
    return std::filesystem::path(base) / L"netease";
}

std::filesystem::path CacheDir() {
    const auto dir = ProfileDir();
    return dir.empty() ? dir : dir / L"cache";
}

std::filesystem::path CoverDir() {
    const auto dir = CacheDir();
    return dir.empty() ? dir : dir / L"covers";
}

std::filesystem::path ServiceDir() {
    const auto dir = ProfileDir();
    return dir.empty() ? dir : dir / L"service";
}

bool EnsureDir(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

}  // namespace glasswave::netease
