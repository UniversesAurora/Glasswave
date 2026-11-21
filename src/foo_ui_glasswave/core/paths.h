#pragma once

#include <filesystem>
#include <string_view>

namespace glasswave::core {

// <profile>\glasswave; empty when the profile path is unavailable. Not created.
std::filesystem::path ProfileRoot();

// <profile>\glasswave\<name>, created on demand; empty when the profile path is unavailable.
std::filesystem::path ProfileDir(std::wstring_view name);

// Creates the directory tree; returns false when it could not be created.
bool EnsureDir(const std::filesystem::path& dir);

} // namespace glasswave::core
