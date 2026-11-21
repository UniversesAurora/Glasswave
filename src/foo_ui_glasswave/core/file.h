#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace glasswave::core {

// Replaces `path` with `bytes` all or nothing: the parent folders are created, the data goes to
// "<name>.part", is flushed, and then moves over the target. A failure leaves the previous file
// untouched, removes the temporary file and throws std::runtime_error.
void WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes);

// The whole file, or nullopt when it cannot be opened or read, or is larger than `maxBytes`.
// Reads while other programs hold the file open for reading or writing.
std::optional<std::string> ReadFileLimited(const std::filesystem::path& path, size_t maxBytes);

} // namespace glasswave::core
