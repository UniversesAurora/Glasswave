#include "pch.h"
#include "stream/list_session.h"
#include "core/text.h"
#include "stream/provider.h"
#include "cache/lists.h"
#include "cache/store.h"
#include "core/log.h"
#include "ui/i18n.h"

#include <map>
#include <deque>
#include <set>

namespace glasswave::stream {
namespace {

constexpr size_t kPageSize = 100;
constexpr size_t kBulkPageSize = 1000; // rows asked for at once when they are read anyway
constexpr size_t kMaxRows = 200000;
constexpr size_t kMaxPages = 4096;
constexpr size_t kWarmSessions = 24;
constexpr size_t kWarmRows = 128000;
// Lists confirmed within these windows are served without asking the service again. Changes
// made in Glasswave revalidate affected lists at once (RevalidateLists) and a manual refresh
// always re-reads; these windows only bound how soon external changes appear on their own.
constexpr int64_t kUserListFreshSeconds = 30 * 60;
constexpr int64_t kCatalogListFreshSeconds = 24 * 60 * 60;
constexpr size_t kEnrichmentConcurrency = 2;
constexpr size_t kMaxTrackedMutations = 256;
constexpr size_t kMaxCompletedRevalidations = 1024;

using core::Utf8;

bool IsCatalogList(ListKind kind) {
    return kind == ListKind::AlbumTracks || kind == ListKind::ArtistTopTracks ||
           kind == ListKind::ArtistAlbums || kind == ListKind::ArtistPlaylists ||
           kind == ListKind::ArtistSimilarArtists;
}
bool IsMutableUserList(ListKind kind) {
    return !IsCatalogList(kind) && !IsCatalogSearch(kind);
}
int64_t FreshSecondsFor(ListKind kind) {
    return IsCatalogList(kind) ? kCatalogListFreshSeconds : kUserListFreshSeconds;
}
bool MatchesTarget(const ListQuery& query, const ListTarget& target) {
    return target.kind == query.kind && (target.id.empty() || target.id == query.id) &&
           (target.section.empty() || target.section == query.section);
}
bool MatchesAnyTarget(const ListQuery& query, const std::vector<ListTarget>& targets) {
    if (targets.empty()) return IsMutableUserList(query.kind);
    return std::any_of(targets.begin(), targets.end(),
                       [&query](const ListTarget& target) { return MatchesTarget(query, target); });
}

void Part(std::string& target, std::string_view value) {
    target += std::to_string(value.size()) + ":";
    target.append(value);
}
std::string LogicalKey(const ListQuery& query, std::string_view cacheContext) {
    std::string key = "list-v2;";
    Part(key, std::to_string(static_cast<int>(query.kind)));
    Part(key, Utf8(query.id));
    Part(key, Utf8(query.section));
    Part(key, std::to_string(static_cast<int>(query.shelf)));
    Part(key, Utf8(query.search));
    Part(key, ui::UsingChinese() ? "zh-CN" : "en");
    Part(key, cacheContext);
    return key;
}
size_t RowCount(const ListPage& page) {
    return std::visit([](const auto& rows) { return rows.size(); }, page.rows);
}
template <typename Row> Id RowIdentity(const Row& row) {
    if constexpr (std::is_same_v<Row, CatalogSearchItem>)
        return CatalogSearchItemKey(row);
    else
        return row.id;
}
ListPage EmptyPage(ListKind kind) {
    ListPage page;
    switch (kind) {
    case ListKind::ArtistAlbums:
    case ListKind::SavedAlbums:
        page.rows = std::vector<Album>{};
        break;
    case ListKind::FollowedArtists:
    case ListKind::ArtistSimilarArtists:
        page.rows = std::vector<Artist>{};
        break;
    case ListKind::Playlists:
    case ListKind::ArtistPlaylists:
    case ListKind::CatalogSearchPlaylists:
        page.rows = std::vector<Playlist>{};
        break;
    case ListKind::CatalogSearchArtists:
        page.rows = std::vector<Artist>{};
        break;
    case ListKind::CatalogSearchAlbums:
        page.rows = std::vector<Album>{};
        break;
    case ListKind::CatalogSearchTopResults:
        page.rows = std::vector<CatalogSearchItem>{};
        break;
    case ListKind::Groups:
        page.rows = std::vector<BrowseGroup>{};
        break;
    default:
        break;
    }
    return page;
}
std::string CursorKey(const ListCursor& cursor) {
    std::string key = std::to_string(cursor.offset) + ";";
    Part(key, Utf8(cursor.value));
    return key;
}
bool AppendPage(ListPage& target, ListPage page) {
    if (target.rows.index() != page.rows.index() || RowCount(target) + RowCount(page) > kMaxRows)
        return false;
    std::visit(
        [&](auto& rows) {
            using Rows = std::decay_t<decltype(rows)>;
            auto& more = std::get<Rows>(page.rows);
            rows.insert(rows.end(), std::make_move_iterator(more.begin()),
                        std::make_move_iterator(more.end()));
        },
        target.rows);
    if (page.playlist) target.playlist = std::move(page.playlist);
    if (page.album) target.album = std::move(page.album);
    if (page.artist) target.artist = std::move(page.artist);
    target.more = page.more;
    target.next = std::move(page.next);
    if (page.total) target.total = page.total;
    if (!page.revision.empty()) target.revision = std::move(page.revision);
    target.enrichmentPending = target.enrichmentPending || page.enrichmentPending;
    return true;
}

// `first` is what a source now returns as page one of the list `shown` holds (completely, or
// its head): the same known total and the same rows, in order, at the head.
bool SameLeadingPage(const ListPage& shown, const ListPage& first) {
    if (shown.rows.index() != first.rows.index() || !shown.total || !first.total ||
        *shown.total != *first.total || (!shown.more && *shown.total != RowCount(shown)))
        return false;
    return std::visit(
        [&first](const auto& rows) {
            const auto& head = std::get<std::decay_t<decltype(rows)>>(first.rows);
            if (head.size() > rows.size() || (head.empty() && !rows.empty())) return false;
            return std::equal(head.begin(), head.end(), rows.begin(),
                              [](const auto& a, const auto& b) { return RowIdentity(a) == RowIdentity(b); });
        },
        shown.rows);
}

bool SameOccurrences(const ListRows& left, const ListRows& right) {
    if (left.index() != right.index()) return false;
    return std::visit(
        [&right](const auto& rows) {
            const auto& other = std::get<std::decay_t<decltype(rows)>>(right);
            return rows.size() == other.size() &&
                   std::equal(rows.begin(), rows.end(), other.begin(),
                              [](const auto& a, const auto& b) { return RowIdentity(a) == RowIdentity(b); });
        },
        left);
}

bool ReplaceMetadata(ListPage& target, size_t offset, const ListPage& page) {
    if (target.rows.index() != page.rows.index() || offset > RowCount(target) ||
        RowCount(page) > RowCount(target) - offset)
        return false;
    return std::visit(
        [&page, offset](auto& rows) {
            const auto& replacement = std::get<std::decay_t<decltype(rows)>>(page.rows);
            if (!std::equal(replacement.begin(), replacement.end(), rows.begin() + offset,
                            [](const auto& a, const auto& b) { return RowIdentity(a) == RowIdentity(b); }))
                return false;
            std::copy(replacement.begin(), replacement.end(), rows.begin() + offset);
            return true;
        },
        target.rows);
}

struct EnrichmentWork {
    ListPage page;
    size_t offset = 0, ordinal = 0;
    uint64_t generation = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
    bool finished = false;
};

struct Lease {
    ListSession::Changed changed;
    ListLoadMode mode = ListLoadMode::OnDemand;
    bool cancelled = false;
    bool next = false;
};
struct Read : std::enable_shared_from_this<Read> {
    struct PersistWaiter {
        uint64_t target = 0;
        ListSession::PersistentSaveDone done;
    };
    Provider& provider;
    ListQuery query;
    ListCachePolicy policy;
    std::string cacheContext;
    std::string cacheKey;
    cache::lists::Identity identity;
    ListState state;
    std::vector<std::weak_ptr<Lease>> leases;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::optional<ListPage> shadow;
    std::vector<cache::lists::Record> shadowRecords; // the re-read's pages, persisted on publication
    std::set<std::string> cursors;
    ListCursor cursor;
    uint64_t request = 0, generation = 0, used = 0;
    uint64_t restoreAttempt = 0;
    uint64_t saveSequence = 0;
    uint64_t fetchMutationSeq = 0;
    size_t networkPages = 0, shadowTarget = 0, emptyPages = 0;
    int64_t validatedAt = 0;
    bool started = false, initializing = false, cacheLoading = false, inFlight = false, valid = true;
    bool restoreWaiting = false, restoreFailed = false;
    bool fullReread = false; // the running re-read must not stop at an unchanged first page
    bool changedByWrite = false; // a write went through since the rows were read
    // The read goes on from the cursor saved with the rows on screen; once the service has
    // rejected such a cursor, this list is read from its start instead.
    bool resumedCursor = false, cursorRejected = false;
    bool gated = false;          // interrupted while the account was not verified
    // How a page read after the rows on screen joins their persisted snapshot: the generation
    // that wrote them in this run (0: none), or the snapshot loaded from disk (`writeBase`).
    uint64_t writeGeneration = 0;
    std::string writeBase;
    size_t metadataFrontier = 0, activeEnrichments = 0;
    std::map<size_t, size_t> metadataReadyRanges;
    std::deque<std::shared_ptr<EnrichmentWork>> enrichmentQueue;
    std::set<uint64_t> pendingSaves;
    std::optional<uint64_t> firstFailedSave;
    std::vector<PersistWaiter> saveWaiters;
    bool pumpingEnrichment = false;

    Read(Provider& source, ListQuery value, ListCachePolicy storage, std::string logicalKey,
         std::string context, std::string sessionKey)
        : provider(source), query(std::move(value)), policy(storage), cacheContext(std::move(context)),
          cacheKey(std::move(sessionKey)),
          identity{Utf8(source.Key()), std::move(logicalKey), cache::AccountRevision(Utf8(source.Key()))} {
        state.page = EmptyPage(query.kind);
    }
    bool HasLeases() const {
        return std::any_of(leases.begin(), leases.end(), [](const auto& weak) {
            auto lease = weak.lock();
            return lease && !lease->cancelled;
        });
    }
    bool WantsAll() const {
        return std::any_of(leases.begin(), leases.end(), [](const auto& weak) {
            auto lease = weak.lock();
            return lease && !lease->cancelled && lease->mode == ListLoadMode::All;
        });
    }
    bool WantsNext() const {
        return std::any_of(leases.begin(), leases.end(), [](const auto& weak) {
            auto lease = weak.lock();
            return lease && !lease->cancelled && lease->next;
        });
    }
    bool NetworkAllowed() const { return !provider.SessionRestoring() || provider.AccountVerified(); }
    void ConsumeNext() {
        for (const auto& weak : leases)
            if (auto lease = weak.lock()) lease->next = false;
    }
    bool ContextCurrent() const { return provider.ListCacheContext(query) == cacheContext; }
    bool Current(uint64_t id) const;
    void Emit() {
        ++state.version;
        std::erase_if(leases, [](const auto& weak) { return weak.expired(); });
        std::vector<std::shared_ptr<Lease>> current;
        for (const auto& weak : leases)
            if (auto lease = weak.lock(); lease && !lease->cancelled) current.push_back(std::move(lease));
        for (const auto& lease : current)
            if (!lease->cancelled && lease->changed) {
                const auto notify = lease->changed;
                notify(state);
            }
    }
    void Start();
    void LoadCache();
    void Refresh(bool preserveDemand = false, bool allowRestoreRetry = false);
    void BeginRestoreAttempt(bool forceRetry);
    void RestoreAttemptDone(uint64_t id, std::wstring error);
    void PersistPage(bool update, cache::lists::Record record);
    void SaveCompleted(uint64_t id, bool success);
    void ResolveSaveWaiters();
    void Next(const std::shared_ptr<Lease>& lease);
    void Fetch();
    void Received(uint64_t id, ListPage page, std::wstring error);
    bool PublishShadowIfReady();
    void MarkMetadataReady(size_t offset, size_t count);
    void PumpEnrichment();
    void Enriched(const std::shared_ptr<EnrichmentWork>& work, ListPage page, std::wstring error);
    void Stop() {
        if (cancelled) cancelled->store(true);
        enrichmentQueue.clear();
        activeEnrichments = 0;
        valid = false;
        ++request;
        inFlight = false;
        state.loading = state.refreshing = false;
        auto waiters = std::move(saveWaiters);
        pendingSaves.clear();
        for (auto& waiter : waiters)
            if (waiter.done) waiter.done(false);
        for (const auto& weak : leases)
            if (auto lease = weak.lock()) {
                lease->cancelled = true;
                lease->changed = {};
            }
    }
    void StopForContextChange() {
        state.loading = state.refreshing = false;
        Stop();
    }
};

struct MutationStamp {
    int64_t atSeconds = 0;
    uint64_t seq = 0;
};
struct TargetMutation {
    ListTarget target;
    MutationStamp stamp;
};
struct ProviderMutations {
    MutationStamp allMutable;
    std::vector<TargetMutation> targeted;
};

std::map<std::string, std::shared_ptr<Read>> reads;
std::map<std::string, ProviderMutations> providerMutations;
std::map<std::string, uint64_t> completedRevalidations;
uint64_t clock = 0, nextGeneration = 0, mutationSeq = 0;
bool stopping = false;

void RecordMutations(const std::string& providerKey, const std::vector<ListTarget>& targets) {
    const MutationStamp stamp{cache::Store::Now(), ++mutationSeq};
    auto& entry = providerMutations[providerKey];
    if (targets.empty()) {
        entry.allMutable = stamp;
        return;
    }
    for (const auto& target : targets) {
        const auto existing = std::find_if(entry.targeted.begin(), entry.targeted.end(),
                                           [&target](const TargetMutation& item) {
                                               return item.target.kind == target.kind &&
                                                      item.target.id == target.id &&
                                                      item.target.section == target.section;
                                           });
        if (existing != entry.targeted.end()) {
            existing->stamp = stamp;
        } else {
            if (entry.targeted.size() >= kMaxTrackedMutations) {
                const auto oldest = std::min_element(
                    entry.targeted.begin(), entry.targeted.end(),
                    [](const TargetMutation& a, const TargetMutation& b) { return a.stamp.seq < b.stamp.seq; });
                if (oldest != entry.targeted.end()) entry.targeted.erase(oldest);
            }
            entry.targeted.push_back({target, stamp});
        }
    }
}

bool MutatedSince(const std::string& providerKey, const ListQuery& query, int64_t validatedAt,
                  const std::string& sessionKey) {
    const auto found = providerMutations.find(providerKey);
    if (found == providerMutations.end()) return false;
    uint64_t completed = 0;
    if (const auto it = completedRevalidations.find(sessionKey); it != completedRevalidations.end())
        completed = it->second;
    const auto newer = [validatedAt, completed](const MutationStamp& stamp) {
        return stamp.seq != 0 && stamp.seq > completed && stamp.atSeconds >= validatedAt;
    };
    if (IsMutableUserList(query.kind) && newer(found->second.allMutable)) return true;
    for (const auto& item : found->second.targeted)
        if (MatchesTarget(query, item.target) && newer(item.stamp)) return true;
    return false;
}

void MarkRevalidated(const std::string& sessionKey, uint64_t seq) {
    if (seq == 0 || sessionKey.empty()) return;
    if (completedRevalidations.size() >= kMaxCompletedRevalidations && !completedRevalidations.contains(sessionKey)) {
        const auto oldest =
            std::min_element(completedRevalidations.begin(), completedRevalidations.end(),
                             [](const auto& a, const auto& b) { return a.second < b.second; });
        if (oldest != completedRevalidations.end()) completedRevalidations.erase(oldest);
    }
    auto& slot = completedRevalidations[sessionKey];
    if (seq > slot) slot = seq;
}

void ClearProviderMutations(const std::string& providerKey) {
    providerMutations.erase(providerKey);
    const std::string prefix = providerKey + ";";
    std::erase_if(completedRevalidations,
                  [&prefix](const auto& entry) { return entry.first.starts_with(prefix); });
}

void MarkDiskTargetsStale(std::wstring_view provider, const std::vector<ListTarget>& targets,
                          const std::set<std::string>& alreadyCovered) {
    if (targets.empty()) return;
    Provider* source = FindProvider(provider);
    if (!source) return;
    const std::string providerKey = Utf8(provider);
    const uint64_t accountRevision = cache::AccountRevision(providerKey);
    std::set<std::string> marked;
    const auto markQuery = [&](const ListQuery& query) {
        const auto policy = IsCatalogSearch(query.kind) && !query.cache ? ListCachePolicy::None
                                                                        : source->ListPolicy(query);
        if (policy != ListCachePolicy::Persistent) return;
        std::string context = source->ListCacheContext(query);
        std::string logical = LogicalKey(query, context);
        std::string key = providerKey + ";" + logical;
        Part(key, std::to_string(accountRevision));
        Part(key, std::to_string(static_cast<int>(policy)));
        if (alreadyCovered.contains(key) || !marked.insert(key).second) return;
        cache::lists::MarkStale({providerKey, std::move(logical), accountRevision});
    };
    for (const auto& target : targets) {
        switch (target.kind) {
        case ListKind::PlaylistTracks:
        case ListKind::AlbumTracks:
            if (!target.id.empty()) {
                ListQuery q;
                q.kind = target.kind;
                q.id = target.id;
                markQuery(q);
            }
            break;
        case ListKind::Favorites:
        case ListKind::SavedAlbums:
        case ListKind::FollowedArtists: {
            ListQuery q;
            q.kind = target.kind;
            markQuery(q);
            break;
        }
        case ListKind::Playlists: {
            for (const Shelf shelf : {Shelf::Created, Shelf::Saved}) {
                ListQuery q;
                q.kind = ListKind::Playlists;
                q.shelf = shelf;
                markQuery(q);
            }
            break;
        }
        case ListKind::SectionTracks:
            if (!target.section.empty()) {
                ListQuery q;
                q.kind = ListKind::SectionTracks;
                q.id = target.id;
                q.section = target.section;
                markQuery(q);
            }
            break;
        case ListKind::Groups:
            if (!target.section.empty()) {
                ListQuery q;
                q.kind = ListKind::Groups;
                q.section = target.section;
                markQuery(q);
            }
            break;
        default:
            break;
        }
    }
}

void Prune() {
    size_t count = 0, rows = 0;
    for (const auto& [key, read] : reads) {
        (void)key;
        ++count;
        rows += RowCount(read->state.page);
    }
    while (count > kWarmSessions || rows > kWarmRows) {
        auto oldest = reads.end();
        for (auto it = reads.begin(); it != reads.end(); ++it)
            if (!it->second->HasLeases() && !it->second->inFlight &&
                (oldest == reads.end() || it->second->used < oldest->second->used))
                oldest = it;
        if (oldest == reads.end()) break;
        rows -= RowCount(oldest->second->state.page);
        auto retired = oldest->second;
        reads.erase(oldest);
        retired->Stop();
        --count;
    }
}
bool Read::Current(uint64_t id) const {
    return !stopping && valid && ContextCurrent() && request == id &&
           cache::AccountRevision(identity.provider) == identity.accountRevision;
}
void Read::Start() {
    if (!valid || stopping) return;
    if (!ContextCurrent()) {
        StopForContextChange();
        return;
    }
    used = ++clock;
    if (started) {
        const bool refresh = !inFlight && !cacheLoading &&
                             (generation == 0 || (cancelled && cancelled->load()) || validatedAt == 0 ||
                              cache::Store::Now() - validatedAt >= FreshSecondsFor(query.kind));
        if (refresh) {
            if (!NetworkAllowed()) {
                BeginRestoreAttempt(false);
                return;
            }
            initializing = true;
        }
        Emit();
        if (refresh)
            Refresh(true);
        else if (NetworkAllowed() &&
                 ((shadow && RowCount(*shadow) < shadowTarget) || (WantsAll() && !state.complete)) &&
                 state.error.empty())
            Fetch();
        return;
    }
    started = true;
    initializing = true;
    // Reading the disk snapshot is not a load the user should see; only a miss is.
    state.loading = policy != ListCachePolicy::Persistent && NetworkAllowed();
    Emit();
    if (policy != ListCachePolicy::Persistent) {
        if (NetworkAllowed())
            Refresh(true);
        else {
            initializing = false;
            BeginRestoreAttempt(false);
        }
        return;
    }
    LoadCache();
}
void Read::LoadCache() {
    const uint64_t id = ++request;
    cacheLoading = true;
    initializing = true;
    cache::lists::Load(identity, [weak = weak_from_this(), id](std::optional<cache::lists::Record> cached) {
        auto self = weak.lock();
        if (!self) return;
        if (!self->ContextCurrent()) {
            self->StopForContextChange();
            return;
        }
        if (!self->Current(id)) return;
        self->cacheLoading = false;
        self->initializing = false;
        core::Log(L"list cache: " + std::wstring(self->provider.Key()) + L" kind " +
                  std::to_wstring(static_cast<int>(self->query.kind)) + L" id " + self->query.id +
                  L" section " + self->query.section +
                  (cached ? L" hit rows " + std::to_wstring(RowCount(cached->page)) + L" pages " +
                                std::to_wstring(cached->pages) +
                                (cached->page.more ? L" partial" : L" complete") + L" described " +
                                std::to_wstring(cached->metadataReadyRows) + L" age " +
                                std::to_wstring(cache::Store::Now() - cached->validatedAt) + L"s"
                          : std::wstring(L" miss")));
        if (cached && cached->page.rows.index() == self->state.page.rows.index()) {
            self->provider.RestoreListRecords(cached->page);
            self->provider.RefreshCachedDisplay(cached->page);
            self->state.page = std::move(cached->page);
            self->state.appendOffset = 0;
            ++self->state.dataVersion;
            self->state.pages = cached->pages;
            self->state.complete = !self->state.page.more;
            self->state.fromCache = true;
            self->state.metadataReadyRows = std::min(cached->metadataReadyRows, RowCount(self->state.page));
            self->state.updatedRows.reset();
            self->state.loading = self->state.refreshing = false;
            self->validatedAt = cached->validatedAt;
            self->writeGeneration = 0;
            self->writeBase = std::move(cached->base);
            self->Emit();
            const bool mutated =
                MutatedSince(self->identity.provider, self->query, self->validatedAt, self->cacheKey);
            if (self->validatedAt == 0 || mutated) self->changedByWrite = true;
            // A complete snapshot that was confirmed within its freshness window needs no request at all.
            if (self->state.complete && !self->state.page.enrichmentPending && self->validatedAt != 0 &&
                !mutated && cache::Store::Now() - self->validatedAt < FreshSecondsFor(self->query.kind)) {
                self->generation = ++nextGeneration;
                return;
            }
        }
        if (self->NetworkAllowed())
            self->Refresh(true);
        else
            self->BeginRestoreAttempt(false);
    });
}
void Read::BeginRestoreAttempt(bool forceRetry) {
    if (!valid || stopping || !HasLeases() || NetworkAllowed() || restoreWaiting) return;

    std::wstring restoreError = provider.SessionRestoreError();
    if (!forceRetry && restoreFailed) {
        initializing = false;
        state.loading = state.refreshing = false;
        if (!restoreError.empty()) state.error = std::move(restoreError);
        Emit();
        return;
    }
    if (!restoreError.empty() && !forceRetry) {
        restoreFailed = true;
        initializing = false;
        state.loading = state.refreshing = false;
        state.error = std::move(restoreError);
        Emit();
        return;
    }

    const uint64_t id = ++restoreAttempt;
    restoreWaiting = true;
    bool accepted = false;
    try {
        accepted = provider.RetrySessionRestore([weak = weak_from_this(), id](std::wstring error) mutable {
            fb2k::inMainThread([weak, id, error = std::move(error)]() mutable {
                if (auto self = weak.lock()) self->RestoreAttemptDone(id, std::move(error));
            });
        });
    } catch (...) {
        restoreError = L"The provider session could not be restored.";
    }

    if (!accepted) {
        restoreWaiting = false;
        initializing = false;
        state.loading = state.refreshing = false;
        if (!restoreError.empty()) {
            restoreFailed = true;
            state.error = std::move(restoreError);
        }
        Emit();
        return;
    }

    restoreFailed = false;
    initializing = false;
    state.loading = RowCount(state.page) == 0;
    state.refreshing = false;
    state.error.clear();
    Emit();
}
void Read::RestoreAttemptDone(uint64_t id, std::wstring error) {
    if (stopping || !valid || id != restoreAttempt) return;
    restoreWaiting = false;
    if (!ContextCurrent()) {
        StopForContextChange();
        return;
    }
    if (!HasLeases()) return;

    // Account verification may complete before later library collection reads. Once the gate is
    // open, that partial failure belongs to the normal list path rather than session restore.
    if (NetworkAllowed()) {
        restoreFailed = false;
        state.error.clear();
        // The account notification may already have started this read.
        if (!inFlight) Refresh(true, false);
        return;
    }

    if (error.empty()) error = provider.SessionRestoreError();
    if (error.empty()) error = L"The provider session could not be restored.";
    restoreFailed = true;
    initializing = false;
    state.loading = state.refreshing = false;
    state.error = std::move(error);
    Emit();
}
void Read::PersistPage(bool update, cache::lists::Record record) {
    const uint64_t id = ++saveSequence;
    pendingSaves.emplace(id);
    record.base = writeBase;
    writeGeneration = generation;
    auto persisted = [weak = weak_from_this(), id](bool success) {
        if (auto self = weak.lock()) self->SaveCompleted(id, success);
    };
    if (update)
        cache::lists::UpdatePage(identity, std::move(record), generation, std::move(persisted));
    else
        cache::lists::SavePage(identity, std::move(record), generation, std::move(persisted));
}
void Read::SaveCompleted(uint64_t id, bool success) {
    pendingSaves.erase(id);
    if (!success)
        core::Log(L"list cache: " + std::wstring(provider.Key()) + L" kind " +
                  std::to_wstring(static_cast<int>(query.kind)) + L" id " + query.id +
                  L" page was not persisted");
    if (!success && (!firstFailedSave || id < *firstFailedSave)) firstFailedSave = id;
    ResolveSaveWaiters();
}
void Read::ResolveSaveWaiters() {
    struct Ready {
        ListSession::PersistentSaveDone done;
        bool success = false;
    };
    std::vector<Ready> ready;
    for (auto it = saveWaiters.begin(); it != saveWaiters.end();) {
        const bool stillPending =
            std::any_of(pendingSaves.begin(), pendingSaves.end(), [&](uint64_t id) { return id <= it->target; });
        if (stillPending) {
            ++it;
            continue;
        }
        const bool failed = firstFailedSave && *firstFailedSave <= it->target;
        if (it->done) ready.push_back({std::move(it->done), !failed});
        it = saveWaiters.erase(it);
    }
    for (auto& item : ready)
        item.done(item.success);
}
void Read::Refresh(bool preserveDemand, bool allowRestoreRetry) {
    if (!valid || stopping || !HasLeases()) return;
    if (!ContextCurrent()) {
        StopForContextChange();
        return;
    }
    if (cacheLoading) {
        // Invalidation changed the manager epoch, so the in-flight load can no longer publish.
        // Re-read the same current scope/context and defer HTTP until that snapshot is visible.
        if (!NetworkAllowed()) state.loading = state.refreshing = false;
        LoadCache();
        return;
    }
    if (!NetworkAllowed()) {
        ++request;
        if (cancelled) cancelled->store(true);
        cancelled = std::make_shared<std::atomic_bool>(false);
        enrichmentQueue.clear();
        activeEnrichments = metadataFrontier = 0;
        metadataReadyRanges.clear();
        cacheLoading = false;
        initializing = false;
        inFlight = false;
        gated = true;
        BeginRestoreAttempt(allowRestoreRetry);
        return;
    }
    gated = false;
    ++request;
    if (cancelled) cancelled->store(true);
    cancelled = std::make_shared<std::atomic_bool>(false);
    enrichmentQueue.clear();
    activeEnrichments = metadataFrontier = 0;
    metadataReadyRanges.clear();
    cacheLoading = false;
    initializing = false;
    generation = ++nextGeneration;
    fetchMutationSeq = mutationSeq;
    inFlight = false;
    fullReread = !preserveDemand || changedByWrite; // asked for explicitly, or known to differ
    changedByWrite = false;
    if (!preserveDemand) ConsumeNext();
    cursor = {};
    resumedCursor = false;
    cursors.clear();
    cursors.insert(CursorKey(cursor));
    networkPages = emptyPages = 0;
    shadowTarget = RowCount(state.page);
    shadow = EmptyPage(query.kind);
    shadowRecords.clear();
    state.loading = true;
    state.refreshing = shadowTarget != 0;
    state.error.clear();
    Fetch();
}
void Read::Next(const std::shared_ptr<Lease>& lease) {
    if (!valid || stopping || !lease || lease->cancelled) return;
    if (!ContextCurrent()) {
        StopForContextChange();
        return;
    }
    used = ++clock;
    lease->next = true;
    if (!started || initializing || cacheLoading || inFlight) return;
    // Preserve demand while the account gate is closed. A later verified notification
    // restarts the read with Refresh(true), which consumes this request.
    if (!NetworkAllowed()) return;
    if (!shadow && state.complete) {
        lease->next = false;
        return;
    }
    ConsumeNext();
    state.error.clear();
    Fetch();
}
void Read::Fetch() {
    if (!valid || stopping || inFlight || !HasLeases()) return;
    if (!ContextCurrent()) {
        StopForContextChange();
        return;
    }
    if (!NetworkAllowed()) {
        state.loading = state.refreshing = false;
        return;
    }
    if (networkPages >= kMaxPages) {
        state.loading = state.refreshing = false;
        state.error = L"The list exceeded the page limit. Refresh to try again.";
        Emit();
        return;
    }
    inFlight = true;
    state.loading = true;
    state.error.clear();
    const uint64_t id = ++request;
    Emit();
    if (!Current(id)) {
        if (!ContextCurrent()) StopForContextChange();
        return;
    }
    if (!HasLeases()) {
        inFlight = false;
        return;
    }
    try {
        ListQuery requestQuery = query;
        requestQuery.cancelled = cancelled;
        if (provider.NeedsAccount(requestQuery) && !provider.SignedIn()) {
            Received(id, {}, provider.SignInRequiredMessage());
            return;
        }
        // The first page stays small: it is shown at once and may end an unchanged re-read.
        // Rows that will be read anyway (a re-read catching up, a list wanted whole) are asked
        // for in larger pages, which a source may serve with parallel requests.
        size_t limit = kPageSize;
        if (shadow && networkPages != 0 && RowCount(*shadow) < shadowTarget)
            limit = std::clamp(shadowTarget - RowCount(*shadow), kPageSize, kBulkPageSize);
        else if ((!shadow || networkPages != 0) && WantsAll())
            limit = kBulkPageSize;
        provider.FetchListPage(
            requestQuery, cursor, limit,
            [weak = weak_from_this(), id](ListPage page, std::wstring error) mutable {
                // Even a synchronous legacy adapter must not recursively drain a list.
                fb2k::inMainThread([weak, id, page = std::move(page), error = std::move(error)]() mutable {
                    if (auto self = weak.lock()) {
                        if (!self->ContextCurrent()) {
                            self->StopForContextChange();
                        } else if (self->Current(id)) {
                            self->Received(id, std::move(page), std::move(error));
                        }
                    }
                });
            });
    } catch (...) {
        Received(id, {}, L"The list could not be loaded. Retry or refresh.");
    }
}
void Read::Received(uint64_t id, ListPage page, std::wstring error) {
    if (stopping || !valid) return;
    if (!ContextCurrent()) {
        StopForContextChange();
        return;
    }
    if (!NetworkAllowed()) {
        Refresh(true);
        return;
    }
    if (!Current(id)) return;
    inFlight = false;
    if (!error.empty())
        core::Log(L"list: " + std::wstring(provider.Key()) + L" kind " +
                  std::to_wstring(static_cast<int>(query.kind)) + L" id " + query.id + L" failed: " + error);
    if (error.empty() && page.rows.index() != state.page.rows.index())
        error = L"The service returned a different list type.";
    if (error.empty() && page.more && cursors.contains(CursorKey(page.next)))
        error = L"The service returned a repeated page. Refresh to try again.";
    const size_t followingEmpty = page.more && RowCount(page) == 0 ? emptyPages + 1 : 0;
    if (error.empty() && followingEmpty > 8) error = L"The service returned too many empty pages.";
    if (!error.empty() && resumedCursor && !cursorRejected) {
        // A continuation saved with the rows may no longer be accepted (it expired, or an
        // earlier build made it): the list is read again from its start rather than failing.
        core::Log(L"list: " + std::wstring(provider.Key()) + L" kind " +
                  std::to_wstring(static_cast<int>(query.kind)) + L" id " + query.id +
                  L" saved cursor rejected; reading from the start");
        cursorRejected = true;
        resumedCursor = false;
        changedByWrite = true; // the re-read must not stop at an unchanged first page
        Refresh(true);
        return;
    }
    resumedCursor = false;
    if (!error.empty()) {
        state.loading = state.refreshing = false;
        state.error = std::move(error);
        Emit();
        return;
    }
    const ListPage& building = shadow ? *shadow : state.page;
    if ((!building.revision.empty() && !page.revision.empty() && building.revision != page.revision) ||
        (building.total && page.total && building.total != page.total)) {
        state.loading = state.refreshing = false;
        state.error = L"The list changed while loading. Refresh to try again.";
        Emit();
        return;
    }
    if (RowCount(building) + RowCount(page) > kMaxRows || (page.total && *page.total > kMaxRows)) {
        state.loading = state.refreshing = false;
        state.error = L"The list exceeded the item limit.";
        Emit();
        return;
    }
    // A list read only in part continues where its rows end, in the snapshot that holds them.
    const bool resumable = state.complete || policy != ListCachePolicy::Persistent || writeGeneration != 0 ||
                           !writeBase.empty();
    if (shadow && networkPages == 0 && resumable && !cursorRejected && RowCount(state.page) != 0 &&
        !state.page.enrichmentPending &&
        ((!page.revision.empty() && page.revision == state.page.revision) ||
         (!fullReread && SameLeadingPage(state.page, page)))) {
        // The service still has the rows on screen: the re-read ends with its first page
        // instead of downloading every page of an unchanged list again. A source that gives
        // a revision is exact. Without one an automatic re-read accepts the same total with
        // the same first page as unchanged; a refresh the user asks for always reads it all.
        // A list read only in part goes on from its last row when more is wanted.
        if (page.playlist) state.page.playlist = std::move(page.playlist);
        if (page.album) state.page.album = std::move(page.album);
        if (page.artist) state.page.artist = std::move(page.artist);
        if (provider.RefreshCachedDisplay(state.page)) ++state.dataVersion;
        shadow.reset();
        shadowRecords.clear();
        if (writeGeneration != 0) generation = writeGeneration;
        cursor = state.complete ? ListCursor{} : state.page.next;
        resumedCursor = !cursor.value.empty() || cursor.offset != 0;
        cursors.clear();
        cursors.insert(CursorKey({}));
        cursors.insert(CursorKey(cursor));
        networkPages = state.pages;
        emptyPages = 0;
        metadataFrontier = RowCount(state.page);
        state.loading = state.refreshing = false;
        state.updatedRows.reset();
        validatedAt = cache::Store::Now();
        MarkRevalidated(cacheKey, fetchMutationSeq);
        if (policy == ListCachePolicy::Persistent && (!writeBase.empty() || writeGeneration != 0))
            cache::lists::TouchValidated(identity, writeBase, writeBase.empty() ? writeGeneration : 0,
                                         validatedAt);
        const bool continueRead = !state.complete && (WantsAll() || WantsNext());
        ConsumeNext();
        Emit();
        if (continueRead && Current(id) && HasLeases()) Fetch();
        return;
    }
    ++networkPages;
    emptyPages = followingEmpty;
    if (page.more) cursors.insert(CursorKey(page.next));
    cursor = page.next;
    const bool more = page.more;
    const size_t offset = RowCount(building);
    std::shared_ptr<EnrichmentWork> enrichment;
    if (page.enrichmentPending) {
        enrichment = std::make_shared<EnrichmentWork>(
            EnrichmentWork{page, offset, networkPages, generation, cancelled});
    } else {
        MarkMetadataReady(offset, RowCount(page));
    }
    if (shadow) {
        // A re-read is written to disk only when it replaces the rows on screen. Until then
        // the persisted snapshot stays the complete older one, so closing the program in the
        // middle of a re-read cannot leave a shorter list behind.
        if (policy == ListCachePolicy::Persistent)
            shadowRecords.push_back({page, networkPages, cache::Store::Now()});
        AppendPage(*shadow, std::move(page));
        if (enrichment) enrichmentQueue.push_back(std::move(enrichment));
        PublishShadowIfReady();
    } else {
        if (policy == ListCachePolicy::Persistent)
            PersistPage(false, {page, networkPages, cache::Store::Now()});
        state.appendOffset = RowCount(state.page);
        AppendPage(state.page, std::move(page));
        ++state.dataVersion;
        state.pages = networkPages;
        state.complete = !more;
        state.metadataReadyRows = metadataFrontier;
        state.page.enrichmentPending = metadataFrontier < RowCount(state.page);
        if (enrichment) enrichmentQueue.push_back(std::move(enrichment));
    }
    state.updatedRows.reset();
    // Rows still waiting behind an unpublished re-read are not confirmed yet.
    if (!shadow) {
        validatedAt = cache::Store::Now();
        MarkRevalidated(cacheKey, fetchMutationSeq);
    }
    state.loading = false;
    const auto publishedGeneration = generation;
    Emit();
    Prune();
    // A consumer may already request the next page from Emit(). That changes request,
    // but must not abandon enrichment for this still-current source generation.
    if (valid && !stopping && generation == publishedGeneration && HasLeases()) PumpEnrichment();
    if (!Current(id) || !HasLeases()) return;
    // A re-read fetches as far as the rows it replaces; past that only on demand.
    const bool catchingUp = shadow && RowCount(*shadow) < shadowTarget;
    const bool continueRead = more && (catchingUp || WantsAll() || WantsNext() || emptyPages != 0);
    ConsumeNext();
    if (continueRead) Fetch();
}

// The re-read replaces the rows on screen once it is no worse than them: it reaches as far
// (or the list ended) and none of its pages still waits for related records. Swapping
// earlier would put bare rows in place of described ones, and a view that needs the
// descriptions (album tiles) would empty out and fill again.
bool Read::PublishShadowIfReady() {
    if (!shadow) return false;
    if (shadow->more && RowCount(*shadow) < shadowTarget) return false;
    if (metadataFrontier < std::min(state.metadataReadyRows, RowCount(*shadow)) &&
        (activeEnrichments != 0 || !enrichmentQueue.empty()))
        return false;
    const bool more = shadow->more;
    state.page = std::move(*shadow);
    shadow.reset();
    state.appendOffset = 0;
    ++state.dataVersion;
    state.fromCache = false;
    state.refreshing = false;
    state.pages = networkPages;
    state.complete = !more;
    state.metadataReadyRows = metadataFrontier;
    state.page.enrichmentPending = metadataFrontier < RowCount(state.page);
    state.updatedRows.reset();
    auto records = std::move(shadowRecords);
    shadowRecords.clear();
    writeBase.clear(); // the re-read starts a snapshot of its own
    for (auto& record : records)
        PersistPage(false, std::move(record));
    return true;
}

void Read::MarkMetadataReady(size_t offset, size_t count) {
    if (count == 0) return;
    metadataReadyRanges[offset] = offset + count;
    for (;;) {
        const auto next = metadataReadyRanges.find(metadataFrontier);
        if (next == metadataReadyRanges.end()) break;
        metadataFrontier = next->second;
        metadataReadyRanges.erase(next);
    }
}

void Read::PumpEnrichment() {
    if (pumpingEnrichment) return;
    pumpingEnrichment = true;
    while (!stopping && valid && HasLeases() && activeEnrichments < kEnrichmentConcurrency &&
           !enrichmentQueue.empty() && NetworkAllowed()) {
        if (!ContextCurrent()) {
            StopForContextChange();
            pumpingEnrichment = false;
            return;
        }
        auto work = std::move(enrichmentQueue.front());
        enrichmentQueue.pop_front();
        if (work->generation != generation || work->cancelled->load()) continue;
        ++activeEnrichments;
        ListQuery requestQuery = query;
        requestQuery.cancelled = work->cancelled;
        try {
            const bool accepted = provider.EnrichListPage(
                requestQuery, work->page,
                [weak = weak_from_this(), work](ListPage page, std::wstring error) mutable {
                    fb2k::inMainThread(
                        [weak, work, page = std::move(page), error = std::move(error)]() mutable {
                            if (auto self = weak.lock())
                                self->Enriched(work, std::move(page), std::move(error));
                        });
                });
            if (!accepted) Enriched(work, work->page, {});
        } catch (...) {
            Enriched(work, {}, L"Optional list metadata could not be loaded.");
        }
    }
    pumpingEnrichment = false;
}

void Read::Enriched(const std::shared_ptr<EnrichmentWork>& work, ListPage page, std::wstring error) {
    if (stopping || !valid || work->finished || work->generation != generation || work->cancelled->load() ||
        !HasLeases() || cache::AccountRevision(identity.provider) != identity.accountRevision)
        return;
    if (!ContextCurrent()) {
        StopForContextChange();
        return;
    }
    if (!NetworkAllowed()) {
        Refresh(true);
        return;
    }
    work->finished = true;
    --activeEnrichments;
    // A failed/invalid optional response cannot replace or hide usable song rows. Keep
    // their original metadata and finish this attempt rather than holding layouts forever.
    const bool changed =
        error.empty() && SameOccurrences(work->page.rows, page.rows) && work->page.rows != page.rows;
    if (changed) work->page.rows = std::move(page.rows);
    work->page.enrichmentPending = false;
    auto& target = shadow ? *shadow : state.page;
    if (ReplaceMetadata(target, work->offset, work->page)) {
        MarkMetadataReady(work->offset, RowCount(work->page));
        target.enrichmentPending = metadataFrontier < RowCount(target);
        if (shadow) {
            // Not on disk yet: the page is written, described, when the re-read is published.
            for (auto& record : shadowRecords)
                if (record.pages == work->ordinal) record.page = work->page;
        } else {
            if (policy == ListCachePolicy::Persistent)
                PersistPage(true, {work->page, work->ordinal, cache::Store::Now()});
            state.metadataReadyRows = metadataFrontier;
            state.updatedRows.reset();
            if (changed) {
                state.updatedRows = ListRowRange{work->offset, RowCount(work->page)};
                state.appendOffset = 0;
                ++state.dataVersion;
            }
            Emit();
        }
    }
    // The re-read may have been waiting for this description (or for its failure).
    if (PublishShadowIfReady()) {
        validatedAt = cache::Store::Now();
        MarkRevalidated(cacheKey, fetchMutationSeq);
        Emit();
    }
    PumpEnrichment();
}

} // namespace

struct ListSession::Impl {
    std::shared_ptr<Read> read;
    std::shared_ptr<Lease> lease;
};
ListSession::ListSession(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
ListSession::~ListSession() {
    Cancel();
}
std::shared_ptr<ListSession> ListSession::Open(Provider& provider, ListQuery query, Changed changed,
                                               ListLoadMode mode) {
    PFC_ASSERT(core_api::is_main_thread());
    const auto policy = IsCatalogSearch(query.kind) && !query.cache ? ListCachePolicy::None
                                                                    : provider.ListPolicy(query);
    std::string cacheContext = provider.ListCacheContext(query);
    std::string logicalKey = LogicalKey(query, cacheContext);
    std::string key = Utf8(provider.Key()) + ";" + logicalKey;
    Part(key, std::to_string(cache::AccountRevision(Utf8(provider.Key()))));
    Part(key, std::to_string(static_cast<int>(policy)));
    auto& read = reads[key];
    if (!read || !read->valid)
        read = std::make_shared<Read>(provider, std::move(query), policy, std::move(logicalKey),
                                      std::move(cacheContext), key);
    else if (provider.RefreshCachedDisplay(read->state.page))
        ++read->state.dataVersion;
    auto impl = std::make_shared<Impl>();
    impl->read = read;
    impl->lease = std::make_shared<Lease>(Lease{std::move(changed), mode, false});
    read->leases.push_back(impl->lease);
    auto session = std::shared_ptr<ListSession>(new ListSession(std::move(impl)));
    fb2k::inMainThread([weak = std::weak_ptr<ListSession>(session)] {
        if (auto current = weak.lock(); current && current->m_impl->read) current->m_impl->read->Start();
    });
    Prune();
    return session;
}
void ListSession::RequestNext() {
    if (auto read = m_impl->read) read->Next(m_impl->lease);
}
void ListSession::SetMode(ListLoadMode mode) {
    if (!m_impl->read || m_impl->lease->cancelled) return;
    const bool changed = m_impl->lease->mode != mode;
    m_impl->lease->mode = mode;
    const auto read = m_impl->read;
    if (changed && mode == ListLoadMode::All && read->started && !read->initializing && !read->inFlight &&
        !read->cacheLoading && !read->state.complete && read->state.error.empty())
        read->Fetch();
}
void ListSession::Refresh() {
    if (auto read = m_impl->read) read->Refresh(false, true);
}
void ListSession::WaitForPersistentSave(PersistentSaveDone done) {
    if (!done) return;
    if (!m_impl || !m_impl->read) {
        done(false);
        return;
    }
    auto read = m_impl->read;
    if (!read->valid || stopping || !read->HasLeases() || m_impl->lease->cancelled) {
        done(false);
        return;
    }
    read->saveWaiters.push_back({read->saveSequence, std::move(done)});
    read->ResolveSaveWaiters();
}
void ListSession::Cancel() {
    if (!m_impl || !m_impl->read) return;
    auto read = std::move(m_impl->read);
    m_impl->lease->cancelled = true;
    m_impl->lease->changed = {};
    if (!read->HasLeases() && (read->inFlight || read->activeEnrichments || !read->enrichmentQueue.empty())) {
        if (read->cancelled) read->cancelled->store(true);
        read->enrichmentQueue.clear();
        read->activeEnrichments = 0;
        ++read->request;
        read->inFlight = false;
        read->state.loading = read->state.refreshing = false;
    }
    if (!read->HasLeases() && !read->saveWaiters.empty()) {
        auto waiters = std::move(read->saveWaiters);
        for (auto& waiter : waiters)
            if (waiter.done) waiter.done(false);
    }
    if (!read->HasLeases() && read->policy == ListCachePolicy::None) {
        read->Stop();
        std::erase_if(reads, [&read](const auto& pair) { return pair.second == read; });
    }
    Prune();
}
ListState ListSession::State() const {
    return m_impl->read ? m_impl->read->state : ListState{};
}
const ListState& ListSession::View() const {
    static const ListState empty;
    return m_impl->read ? m_impl->read->state : empty;
}

void InvalidateListCache(std::wstring_view provider, bool restart, ListCacheInvalidation mode) {
    if (stopping) return;
    std::vector<std::shared_ptr<Read>> affected;
    const std::string providerKey = Utf8(provider);
    const uint64_t accountRevision = cache::AccountRevision(providerKey);
    for (auto it = reads.begin(); it != reads.end();) {
        if (it->second->provider.Key() != provider) {
            ++it;
            continue;
        }
        affected.push_back(it->second);
        if (restart && it->second->HasLeases() && it->second->identity.accountRevision == accountRevision)
            ++it;
        else
            it = reads.erase(it);
    }
    if (mode == ListCacheInvalidation::PurgePersistent) {
        ClearProviderMutations(providerKey);
        cache::lists::InvalidateProvider(providerKey);
    } else {
        cache::lists::RetireProvider(providerKey);
    }
    for (const auto& read : affected) {
        if (restart && read->HasLeases() && read->identity.accountRevision == accountRevision)
            read->Refresh(mode == ListCacheInvalidation::RetireInMemory);
        else
            read->Stop();
    }
}
void ResumeLists(std::wstring_view provider) {
    if (stopping) return;
    std::vector<std::shared_ptr<Read>> waiting;
    for (const auto& [key, read] : reads) {
        (void)key;
        if (read->provider.Key() != provider || !read->valid || !read->HasLeases() || !read->started) continue;
        // A read that is already loading or was just confirmed carries on undisturbed;
        // restarting it would send the request it has in flight a second time.
        if (read->inFlight || read->cacheLoading) continue;
        // A read the closed gate interrupted starts again; so does one that was never
        // confirmed. Rows confirmed within their freshness window need nothing.
        const bool confirmed = !read->gated && read->generation != 0 && read->state.error.empty() &&
                               !read->shadow && read->validatedAt != 0 &&
                               cache::Store::Now() - read->validatedAt < FreshSecondsFor(read->query.kind);
        if (!confirmed) waiting.push_back(read);
    }
    for (const auto& read : waiting)
        read->Refresh(true);
}
void RevalidateLists(std::wstring_view provider, std::vector<ListTarget> targets) {
    if (stopping) return;
    const std::string providerKey = Utf8(provider);
    RecordMutations(providerKey, targets);
    std::vector<std::shared_ptr<Read>> affected;
    std::set<std::string> coveredKeys;
    for (const auto& [key, read] : reads) {
        if (read->provider.Key() == provider && read->valid && MatchesAnyTarget(read->query, targets)) {
            affected.push_back(read);
            coveredKeys.insert(key);
        }
    }
    for (const auto& read : affected) {
        // The rows stay where they are: a list on screen catches up behind them, the others
        // do so when they are opened next, and persisted snapshots still serve the first paint.
        read->validatedAt = 0;
        read->changedByWrite = true;
        if (read->HasLeases() && read->started && !read->cacheLoading)
            read->Refresh(true);
        else if (read->policy == ListCachePolicy::Persistent)
            cache::lists::MarkStale(read->identity);
    }
    MarkDiskTargetsStale(provider, targets, coveredKeys);
}
void ShutdownListSessions() {
    stopping = true;
    providerMutations.clear();
    completedRevalidations.clear();
    auto retired = std::move(reads);
    reads.clear();
    for (const auto& [key, read] : retired) {
        (void)key;
        read->Stop();
    }
    cache::lists::Shutdown();
}

} // namespace glasswave::stream

namespace {
class ListSessionLifetime : public initquit {
  public:
    void on_init() override {
        glasswave::cache::lists::SetDiagnostics([](std::string_view reason) {
            glasswave::core::Log(
                L"list cache: " +
                std::wstring(pfc::stringcvt::string_wide_from_utf8(reason.data(), reason.size()).get_ptr()));
        });
    }
    void on_quit() override { glasswave::stream::ShutdownListSessions(); }
};
FB2K_SERVICE_FACTORY(ListSessionLifetime);
} // namespace
