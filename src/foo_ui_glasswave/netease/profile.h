#pragma once

#include <filesystem>
#include <string>

namespace glasswave::netease {

// <profile>\glasswave\netease; empty when the profile path is unavailable.
std::filesystem::path ProfileDir();
std::filesystem::path CacheDir();
std::filesystem::path CoverDir();
std::filesystem::path ServiceDir();

// Creates the directory tree; returns false when it could not be created.
bool EnsureDir(const std::filesystem::path& dir);

}  // namespace glasswave::netease
