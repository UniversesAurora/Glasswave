#pragma once

#include <SDK/foobar2000.h>

// What foobar2000 knew about every streaming song kept in a playlist - its tags, its technical
// info (codec, bit rate, sample rate...) and its length - saved across restarts and given back
// to metadb as the component starts, before playback resumes and before any list is drawn.
//
// Streaming songs have no file to read their info from, and metadb does not keep it across
// restarts: each provider fed it from its own records, which come back later (read from disk
// on a worker, after a session starts, or asked for again song by song) and not for every song
// (a song known only from a list page). Until then a restored queue showed placeholders and no
// bit rate, and the resumed song kept its placeholder title (owner, 2026-11-21: "Apple Music
// 1888098957"). The snapshot is provider-neutral: any path a registered provider owns.
//
// Providers write tags with HintTags, which keeps the technical info metadb already has.
namespace glasswave::stream {

// Set (to "1") in the info of a stand-in an input reports for a song nobody knows yet
// ("Apple Music <id>"): such info is never kept nor handed back as the song's.
inline constexpr const char* kPlaceholderInfo = "glasswave_placeholder";

// Adds a hint replacing `item`'s tags (meta fields, and the length when `tags` has one) while
// keeping metadb's technical info for it.
void HintTags(metadb_hint_list::ptr& hints, const metadb_handle_ptr& item, const file_info& tags);

// The tags metadb holds for `item` (restored from the snapshot or put by a provider) copied into
// `out`, with the length; false when it knows no title for it.
bool KnownTags(const metadb_handle_ptr& item, file_info& out);

// Technical info (codec, bit rate, sample rate, bit depth, channels, encoding) is what plays.
// Inputs report their decoder's while decoding and keep it with NoteTechInfo (any thread): it
// is written to metadb on the main thread, so the snapshot keeps it across restarts. Before
// a song decodes, KnownTechInfo fills what `out` lacks with the last one kept (owner,
// 2026-11-21: after a restart the resumed song showed no sample rate; NetEase's input reported
// only what its catalog record held, channels).
void CopyTechInfo(const file_info& from, file_info& to);
void NoteTechInfo(const metadb_handle_ptr& item, const file_info& played);
void KnownTechInfo(const metadb_handle_ptr& item, file_info& out);

} // namespace glasswave::stream
