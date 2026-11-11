#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "ui/song_grouping.h"

namespace {

size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

struct Row {
    std::wstring provider;
    std::wstring trackId;
    std::wstring albumId;
    std::wstring album;
    std::wstring artist;

    std::wstring Key() const {
        return glasswave::ui::RemoteAlbumGroupKey(provider, albumId, album, artist, trackId);
    }
};

std::vector<std::wstring> Keys(const std::vector<Row>& rows) {
    std::vector<glasswave::ui::RemoteAlbumGroupRow> identities;
    identities.reserve(rows.size());
    for (const auto& row : rows)
        identities.push_back({row.provider, row.albumId, row.album, row.artist, row.trackId});
    return glasswave::ui::RemoteAlbumGroupKeys(identities);
}

// Mirrors the expanded-tile album ordering: groups appear at their first row's position, rows
// inside one group retain the original song order, and activation still maps to that row.
std::vector<size_t> DisplayOrder(const std::vector<std::wstring>& keys) {
    std::vector<size_t> order(keys.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::vector<size_t> rank(keys.size());
    for (size_t i = 0; i < keys.size(); ++i)
        rank[i] = static_cast<size_t>(std::find(keys.begin(), keys.end(), keys[i]) - keys.begin());
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return rank[a] < rank[b];
    });
    return order;
}

size_t GroupCount(const std::vector<std::wstring>& keys, const std::vector<size_t>& order) {
    if (order.empty()) return 0;
    size_t count = 1;
    for (size_t i = 1; i < order.size(); ++i)
        if (keys[order[i]] != keys[order[i - 1]]) ++count;
    return count;
}

std::vector<std::wstring> ActivationTrackIds(const std::vector<Row>& rows,
                                             const std::vector<size_t>& order) {
    std::vector<std::wstring> ids;
    ids.reserve(order.size());
    for (size_t index : order) ids.push_back(rows[index].trackId);
    return ids;
}

void CheckInterleavedAlbumsAndDuplicateReferences() {
    const std::vector<Row> rows{
        {L"provider", L"t1", L"album-a", L"Album A", L"Artist A"},
        {L"provider", L"b1", L"album-b", L"Album B", L"Artist B"},
        {L"provider", L"t2", L"album-a", L"Album A", L"Artist A"},
        {L"provider", L"t1", L"album-a", L"Album A", L"Artist A"},
        {L"provider", L"b2", L"album-b", L"Album B", L"Artist B"},
    };
    const auto keys = Keys(rows);
    const auto order = DisplayOrder(keys);

    Check(order.size() == 5, "Duplicate song references must remain separate rows");
    Check(GroupCount(keys, order) == 2, "Interleaved rows must produce two album groups");
    Check(rows[order[0]].albumId == L"album-a" && rows[order[1]].albumId == L"album-a" &&
              rows[order[2]].albumId == L"album-a",
          "Interleaved album id must gather all three rows");
    Check(rows[order[3]].albumId == L"album-b" && rows[order[4]].albumId == L"album-b",
          "Second album must retain its rows in original order");
    Check(rows[order[2]].trackId == rows[order[0]].trackId,
          "Duplicate reference must not be removed during album grouping");
    Check(rows[order[0]].trackId == L"t1" && rows[order[1]].trackId == L"t2" &&
              rows[order[2]].trackId == L"t1" && rows[order[3]].trackId == L"b1" &&
              rows[order[4]].trackId == L"b2",
          "Grouping must preserve each album's original row order");
    Check(ActivationTrackIds(rows, order) == std::vector<std::wstring>{L"t1", L"t2", L"t1", L"b1", L"b2"},
          "Activation order must still map to the original song rows");
    Check(order == std::vector<size_t>{0, 2, 3, 1, 4},
          "Original row indices must remain the activation identity after grouping");
}

void CheckMixedIdPresence() {
    const std::vector<Row> rows{
        {L"provider", L"id-1", L"album-1", L"Album", L"Artist"},
        {L"provider", L"id-2", L"", L"album", L"artist"},
        {L"provider", L"id-3", L"", L"ALBUM", L"ARTIST"},
        {L"provider", L"id-4", L"", L"Album", L"Other Artist"},
        {L"other-provider", L"id-5", L"id-1", L"Album", L"Artist"},
    };
    const auto keys = Keys(rows);
    const auto order = DisplayOrder(keys);
    Check(keys[0] == keys[1] && keys[1] == keys[2],
          "A unique nearby album id must join missing-id rows with matching name and artist");
    Check(keys[3] != keys[0], "Missing-id fallback must respect the artist");
    Check(keys[4] != keys[0], "Identical ids must remain provider-scoped");
    Check(GroupCount(keys, order) == 3, "Mixed identities must still resolve to three album groups");
    Check(order == std::vector<size_t>{0, 1, 2, 3, 4},
          "Merging missing ids must preserve the original activation rows");
}

void CheckMissingIdsAndProviderScope() {
    const Row first{L"provider-a", L"t1", L"", L"Shared Album", L"Shared Artist"};
    const Row caseVariant{L"provider-a", L"t2", L"", L"shared album", L"shared artist"};
    const Row otherArtist{L"provider-a", L"t3", L"", L"Shared Album", L"Other Artist"};
    const Row otherProvider{L"provider-b", L"t1", L"", L"Shared Album", L"Shared Artist"};
    const Row sameIdOtherMetadata{L"provider-a", L"t4", L"album-a", L"Renamed Album", L"Other Artist"};
    const Row sameIdFirst{L"provider-a", L"t5", L"album-a", L"Original Album", L"Shared Artist"};
    const Row unknownOne{L"provider-a", L"t6", L"", L"", L""};
    const Row unknownTwo{L"provider-a", L"t7", L"", L"", L""};

    Check(first.Key() == caseVariant.Key(), "Missing album ids must group by folded name and artist");
    Check(first.Key() != otherArtist.Key(), "Same album title with a different artist must stay separate");
    Check(first.Key() != otherProvider.Key(), "Identical metadata from another provider must stay separate");
    Check(sameIdFirst.Key() == sameIdOtherMetadata.Key(), "A supplied album id must take precedence");
    Check(unknownOne.Key() != unknownTwo.Key(), "Tracks without album identity must remain separate");

    const Row ambiguousFirst{L"provider-a", L"t8", L"album-one", L"Shared Album", L"Shared Artist"};
    const Row ambiguousSecond{L"provider-a", L"t9", L"album-two", L"Shared Album", L"Shared Artist"};
    const Row ambiguousMissing{L"provider-a", L"t10", L"", L"Shared Album", L"Shared Artist"};
    const std::vector<Row> ambiguousRows{ambiguousFirst, ambiguousSecond, ambiguousMissing};
    const auto ambiguousKeys = Keys({ambiguousFirst, ambiguousSecond, ambiguousMissing});
    const auto ambiguousOrder = DisplayOrder(ambiguousKeys);
    Check(ambiguousKeys[0] != ambiguousKeys[1], "Different supplied album ids must stay separate");
    Check(ambiguousKeys[2] != ambiguousKeys[0] && ambiguousKeys[2] != ambiguousKeys[1],
          "A missing id must not join either of two ambiguous albums with the same metadata");
    Check(GroupCount(ambiguousKeys, ambiguousOrder) == 3,
          "An ambiguous missing id must remain a separate third album group");
    const std::vector<std::wstring> ambiguousActivation{L"t8", L"t9", L"t10"};
    Check(ActivationTrackIds(ambiguousRows, ambiguousOrder) == ambiguousActivation,
          "Ambiguous fallback must not remap song activation");
}

}  // namespace

int main() {
    CheckInterleavedAlbumsAndDuplicateReferences();
    CheckMixedIdPresence();
    CheckMissingIdsAndProviderScope();
    std::cout << "Song grouping checks passed: " << checks << '\n';
    return 0;
}
