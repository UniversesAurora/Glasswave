#include "pch.h"
#include "core/paths.h"

namespace glasswave::core {

std::filesystem::path ProfileRoot() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return {};
    } catch (...) {
        return {};
    }
    const std::wstring base = pfc::stringcvt::string_wide_from_utf8(native.get_ptr()).get_ptr();
    if (base.empty()) return {};
    return std::filesystem::path(base) / L"glasswave";
}

std::filesystem::path ProfileDir(std::wstring_view name) {
    const auto root = ProfileRoot();
    if (root.empty()) return {};
    auto dir = root / name;
    EnsureDir(dir);
    return dir;
}

bool EnsureDir(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

} // namespace glasswave::core
