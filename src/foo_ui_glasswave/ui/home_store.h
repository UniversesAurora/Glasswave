#pragma once

#include "stream/provider.h"

#include <optional>

// The last home page each source sent, kept on disk (<profile>\glasswave\home\<source>.json) so
// Home shows it at once in the next run while the source is read again (owner, 2026-11-15). One
// file per source, written after every successful read, with the account it belongs to: another
// account's file is not shown.
namespace glasswave::ui::home_store {

// Main thread. The records it names are given back to the source (Provider::RestoreListRecords),
// so their covers and pages work before the source has been read.
std::optional<stream::Home> Load(stream::Provider& provider);
// Main thread; the file is written in the background.
void Save(const stream::Provider& provider, const stream::Home& home);

} // namespace glasswave::ui::home_store
