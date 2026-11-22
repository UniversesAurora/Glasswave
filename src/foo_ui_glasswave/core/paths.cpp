#include "pch.h"
#include "core/paths.h"

namespace glasswave::core {

std::filesystem::path ComponentRoot() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ComponentRoot), &self) ||
        !self)
        return {};
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(self, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

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
