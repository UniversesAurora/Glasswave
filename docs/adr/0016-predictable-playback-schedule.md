# ADR 0016: Predictable Glasswave playback scheduling

Status: accepted 2026-11-10; supersedes ADR 0014's decision to retain native random
selection. The owner explicitly requested a custom scheduler after the public-SDK study.

Glasswave owns the playback schedule and continues to delegate decoding, DSP, output and
input lookahead to foobar2000. A pure occurrence-based model keeps canonical unshuffled
order separate from randomized unplayed order, priority insertions and history. Identical
track handles at different queue positions have different identities.

The main-thread host projects its plan into Glasswave's protected Now Playing playlist;
source/library playlists are not reordered. Core playback order stays Default, preventing
two shuffle engines from competing. The same plan supplies the Up Next pane and prefetch.
Explicit SDK queue rows are preserved and included ahead of the playlist future. Play Next
is prepended as a stable block; ordinary additions enter random unplayed positions.

Shuffle-off restores surviving canonical order with bounded passes rather than repeated
linear searches. Manual drag updates canonical slot intent. Replace/Clear changes content
generation so delayed callbacks, checkpoints and paged-source feeds cannot restore an old
list. Persistence must validate exact queue identity before restoring occurrence roles.

The implementation uses public SDK APIs only. Callback-triggered queue mutations are
deferred until outside the core callback stack. Native-order actions are imported once;
host-written Default echoes do not disable the custom mode.

Schedule notifications are deferred and coalesced after committed changes. Both the UI
and prefetch subscribe to the host rather than inferring completion from SDK callback
registration order. A stale rendered drag is rejected by revision and refreshed. Owned
explicit-queue mirrors follow occurrence permutations exactly; foreign duplicate entries
retain distinct SDK positions. Commands use the scheduler transaction exclusively and
do not silently fall back to a second direct playlist implementation after failure.
