#pragma once

namespace glasswave::lyrics::native_sources {

// Registers the built-in searchers: embedded and same-name file lyrics, each source's native
// lyrics, and the searchers a source contributes (Provider::LyricsSearchers).
void Start();
void Shutdown();

} // namespace glasswave::lyrics::native_sources
