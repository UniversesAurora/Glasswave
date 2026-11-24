#include "media/lookahead_policy.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::atomic_size_t g_checks = 0;

void Check(bool condition, std::string_view message) {
    ++g_checks;
    if (!condition) throw std::runtime_error(std::string(message));
}

using glasswave::media::kLookaheadArtHorizon;
using glasswave::media::kLookaheadMetadataHorizon;
using glasswave::media::LookaheadCandidate;
using glasswave::media::LookaheadPlan;
using glasswave::media::PlanLookahead;

LookaheadCandidate ReadyStream(std::wstring id) {
    LookaheadCandidate c;
    c.identity = std::move(id);
    c.isStreaming = true;
    c.itemIdentified = true;
    c.metadataComplete = true;
    c.albumArtKeyStable = true;
    c.regionalSettled = true;
    return c;
}

LookaheadCandidate LocalTrack(std::wstring id, bool identified = true) {
    LookaheadCandidate c;
    c.identity = std::move(id);
    c.isStreaming = false;
    c.itemIdentified = identified;
    c.metadataComplete = true;
    c.albumArtKeyStable = true;
    c.regionalSettled = true;
    return c;
}

} // namespace

int main() {
    try {
        // 1. Idle = idle: inactive playback produces zero work.
        {
            const std::vector<LookaheadCandidate> tracks{ReadyStream(L"t1"), ReadyStream(L"t2")};
            const LookaheadPlan plan = PlanLookahead(tracks, false, L"t0");
            Check(plan.selected.empty(), "inactive playback must select nothing");
            Check(plan.wantMetadata.empty(), "inactive playback must not request metadata");
            Check(plan.warmArt.empty(), "inactive playback must not warm cover art");
            Check(!plan.prefetchLyrics.has_value(), "inactive playback must not prefetch lyrics");
        }

        // 2. Staged horizons: metadata N+1..N+5, cover art N+1..N+4, lyrics N+1 only.
        {
            std::vector<LookaheadCandidate> tracks;
            for (int i = 1; i <= 7; ++i) {
                LookaheadCandidate c = ReadyStream(L"s" + std::to_wstring(i));
                c.regionalSettled = (i == 1); // N+1 settled, N+2..N+7 still need regional display
                tracks.push_back(std::move(c));
            }
            const LookaheadPlan plan = PlanLookahead(tracks, true, L"s0");
            Check(plan.selected.size() == kLookaheadMetadataHorizon,
                  "must cap selected candidates at metadata horizon (5)");
            Check(plan.wantMetadata.size() == 4 && plan.wantMetadata.front() == 1 &&
                      plan.wantMetadata.back() == 4,
                  "must want metadata/regional names for unsettled tracks within N+1..N+5");
            Check(plan.warmArt.size() == kLookaheadArtHorizon && plan.warmArt.front() == 0 &&
                      plan.warmArt.back() == 3,
                  "must warm cover art only for N+1..N+4");
            Check(plan.prefetchLyrics.has_value() && *plan.prefetchLyrics == 0,
                  "must prefetch lyrics only for settled N+1");
        }

        // 3. Cover art and lyrics gating: unlinked radio/streaming tracks wait for albumId & regional.
        {
            LookaheadCandidate unlinked;
            unlinked.identity = L"apple:101";
            unlinked.isStreaming = true;
            unlinked.itemIdentified = true;
            unlinked.metadataComplete = false;
            unlinked.albumArtKeyStable = false;
            unlinked.regionalSettled = false;

            LookaheadCandidate linkedPendingRegional;
            linkedPendingRegional.identity = L"apple:102";
            linkedPendingRegional.isStreaming = true;
            linkedPendingRegional.itemIdentified = true;
            linkedPendingRegional.metadataComplete = true;
            linkedPendingRegional.albumArtKeyStable = true;
            linkedPendingRegional.regionalSettled = false;

            const LookaheadPlan plan =
                PlanLookahead({unlinked, linkedPendingRegional}, true, L"apple:100");
            Check(plan.wantMetadata.size() == 2,
                  "both unlinked and regional-pending tracks must request metadata");
            Check(plan.warmArt.size() == 1 && plan.warmArt[0] == 1,
                  "must wait for albumArtKeyStable before warming cover art");
            Check(!plan.prefetchLyrics.has_value(),
                  "must not prefetch lyrics for N+1 while metadata/regional names are unsettled, and must not skip to N+2");
        }

        // 4. N+1 linked but regional display pending -> warms art, holds lyrics until regional settles.
        {
            LookaheadCandidate n1 = ReadyStream(L"apple:201");
            n1.regionalSettled = false;
            LookaheadCandidate n2 = ReadyStream(L"apple:202");

            const LookaheadPlan plan = PlanLookahead({n1, n2}, true, L"apple:200");
            Check(plan.wantMetadata.size() == 1 && plan.wantMetadata[0] == 0,
                  "regional-pending N+1 must request regional display");
            Check(plan.warmArt.size() == 2, "stable album art keys warm immediately");
            Check(!plan.prefetchLyrics.has_value(),
                  "must hold lyrics prefetch until N+1 regional display names settle");
        }

        // 5. Deduplication against current track and duplicate upcoming entries.
        {
            const std::vector<LookaheadCandidate> tracks{
                LocalTrack(L"cur"),
                LocalTrack(L"a"),
                LocalTrack(L"a"),
                LocalTrack(L"b"),
                LocalTrack(L""),
                LocalTrack(L"c"),
                LocalTrack(L"d"),
                LocalTrack(L"e"),
                LocalTrack(L"f"),
            };
            const LookaheadPlan plan = PlanLookahead(tracks, true, L"cur");
            Check(plan.selected == (std::vector<size_t>{1, 3, 5, 6, 7}),
                  "must skip current track, duplicates and empty identities up to 5 distinct tracks");
            Check(plan.wantMetadata.empty(), "local files never request streaming metadata");
            Check(plan.warmArt == (std::vector<size_t>{1, 3, 5, 6}),
                  "must warm art for first 4 distinct upcoming tracks");
            Check(plan.prefetchLyrics.has_value() && *plan.prefetchLyrics == 1,
                  "must prefetch lyrics for first distinct upcoming track");
        }

        // 6. Unidentified local track (e.g. untagged CD track) skips lyrics prefetch.
        {
            const LookaheadPlan plan = PlanLookahead({LocalTrack(L"cd1", false)}, true, L"cd0");
            Check(plan.warmArt.size() == 1, "local track cover warm does not require title tags");
            Check(!plan.prefetchLyrics.has_value(), "unidentified track must not prefetch lyrics");
        }

        std::cout << "playback_lookahead_tests: " << g_checks.load() << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
