#pragma once

#include <string>
#include <string_view>
#include <vector>

// Records kept as text: one per line, fields separated by tabs, in UTF-8. A field's %, tab, LF
// and CR are written %-escaped, so any text survives the round trip.

namespace glasswave::core::tsv {

std::string Escape(std::string_view utf8);
std::string Unescape(std::string_view field);
// `text` cut at every `separator` (a line's fields, a file's lines): n separators give n + 1
// parts, the views pointing into `text`.
std::vector<std::string_view> Split(std::string_view text, char separator = '\t');

} // namespace glasswave::core::tsv
