#pragma once

namespace glasswave::lyrics::native_sources {

// Registers source-slot built-ins for embedded/file-backed and provider-native lyrics.
void Start();
void Shutdown();

} // namespace glasswave::lyrics::native_sources
