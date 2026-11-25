#include "pch.h"
#include "ui/remote_pages_parts.h"

// All Music > Favorites: the local Favorites playlist and every signed-in source's favorites.
namespace glasswave::ui {

using namespace remotepages;

namespace {

// ---- All Music > Favorites -------------------------------------------------------------------

class AllFavoritesPage final : public Page {
  public:
    AllFavoritesPage(PageHost& host, View view) : Page(host, std::move(view)) {}
    bool CanRefresh() const override { return true; }

    std::unique_ptr<media::ListSource> Start(bool) override {
        BuildLocalFavorites();
        auto source = std::make_unique<AllFavoritesSource>(m_local, AllFavoritesSource::RemoteFavorites{});
        m_source = source.get();
        ConfigureSource();
        for (auto* provider : stream::Providers())
            if (HasFavorites(*provider)) OpenFeed(*provider);
        Publish();
        return source;
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (auto* source = SongSource(); source && source->Count()) {
            AddBulkActions(actions, AllSongsFetch());
            actions.push_back({});
        }
        if (HasError()) {
            AppendSeparator(actions);
            actions.push_back({L"Retry", Guard([this] { Retry(); })});
        }
    }

    void NearEnd() override {
        for (auto& feed : m_feeds)
            if (feed.session && !feed.State().complete && feed.State().error.empty() && !feed.State().loading)
                feed.session->RequestNext();
    }

    void RefreshRequested() override {
        for (auto& feed : m_feeds)
            if (feed.session) feed.session->Refresh();
    }

    bool IsLoading() const override {
        return std::any_of(m_feeds.begin(), m_feeds.end(), [this](const Feed& feed) {
            const auto& state = feed.State();
            return state.loading ||
                   (!state.complete && state.error.empty() &&
                    (state.pages == 0 || m_allMode || m_drainRequested));
        });
    }

    void OnPresentationChanged() override {
        if (!m_source) return;
        m_allMode = m_source->NeedsCompleteList();
        UpdateSessionMode();
        UpdateStatus();
        m_host.ListChanged();
    }

    void OnLibraryChanged() override {
        // An account that signed in (or out) since the feeds were opened has none (or a stale
        // one): the page is built again with the providers there are now.
        std::vector<const stream::Provider*> now;
        for (auto* provider : stream::Providers())
            if (HasFavorites(*provider)) now.push_back(provider);
        if (!std::equal(now.begin(), now.end(), m_feeds.begin(), m_feeds.end(),
                        [](const stream::Provider* provider, const Feed& feed) {
                            return provider == feed.provider;
                        })) {
            m_host.Reload(); // destroys this page
            return;
        }
        UpdateStatus();
        m_host.ChromeChanged();
    }

    void OnFavoritesChanged() override {
        auto* source = SongSource();
        if (source &&
            std::any_of(m_feeds.begin(), m_feeds.end(), [](const Feed& f) { return f.State().pages != 0; })) {
            m_host.BeforeListChange();
            if (!source->RemoveUnfavorited()) return;
            m_host.ListChanged();
        } else {
            Page::OnFavoritesChanged();
        }
    }

  private:
    struct Feed {
        stream::Provider* provider = nullptr;
        std::shared_ptr<stream::ListSession> session;
        uint64_t dataVersion = 0; // last published
        uint64_t generation = 0;
        // The session's state, borrowed rather than copied on every change.
        const stream::ListState& State() const {
            static const stream::ListState empty;
            return session ? session->View() : empty;
        }
    };

    SongTableSource* SongSource() const { return dynamic_cast<SongTableSource*>(m_host.Source()); }

    // A signed-in (or restoring) account of a source with a Favorites section.
    static bool HasFavorites(stream::Provider& provider) {
        return FindSection(provider, stream::SectionKind::Favorites) &&
               (provider.SignedIn() || provider.SessionRestoring());
    }

    void BuildLocalFavorites() {
        if (const t_size playlist = media::FavoritesPlaylist(false); playlist != SIZE_MAX) {
            metadb_handle_list items;
            playlist_manager::get()->playlist_get_all_items(playlist, items);
            for (size_t i = 0; i < items.get_count(); ++i)
                if (!stream::ProviderOf(items[i])) m_local.push_back(items[i]);
        }
    }

    void ConfigureSource() {
        if (auto* source = dynamic_cast<AllFavoritesSource*>(m_source))
            source->SetActivateHandler(
                Guard([this](size_t row, const std::wstring& identity) { Activate(row, identity); }));
    }

    void OpenFeed(stream::Provider& provider) {
        Feed& feed = m_feeds.emplace_back();
        feed.provider = &provider;
        feed.generation = ++m_generation;
        const uint64_t generation = feed.generation;
        auto changed = Guard([this, providerPtr = &provider, generation](const stream::ListState& state) {
            const auto found = std::find_if(m_feeds.begin(), m_feeds.end(), [providerPtr](const Feed& feed) {
                return feed.provider == providerPtr;
            });
            if (found == m_feeds.end() || found->generation != generation) return;
            const bool dataChanged = found->dataVersion != state.dataVersion;
            found->dataVersion = state.dataVersion;
            if (dataChanged) {
                m_host.BeforeListChange();
                Publish();
            } else {
                UpdateStatus();
            }
            if ((Complete() || (!state.error.empty() && !state.loading)) && m_drainRequested) {
                m_drainRequested = false;
                UpdateSessionMode();
            }
            SettleWaiters();
            m_host.ListChanged();
            if (dataChanged) m_host.ChromeChanged();
        });
        auto query = Query(stream::ListKind::Favorites);
        feed.session = stream::ListSession::Open(provider, PreparedQuery(provider, std::move(query)),
                                                 std::move(changed), stream::ListLoadMode::OnDemand);
        feed.dataVersion = feed.session->View().dataVersion;
    }

    AllFavoritesSource::RemoteFavorites RemoteRows() const {
        AllFavoritesSource::RemoteFavorites rows;
        for (const auto& feed : m_feeds)
            rows.push_back({feed.provider, Rows<stream::Track>(feed.State().page.rows)});
        return rows;
    }

    bool Complete() const {
        return std::all_of(m_feeds.begin(), m_feeds.end(),
                           [](const Feed& feed) { return feed.State().complete; });
    }

    bool HasError() const {
        return std::any_of(m_feeds.begin(), m_feeds.end(),
                           [](const Feed& feed) { return !feed.State().error.empty() && !feed.State().loading; });
    }

    std::wstring LoadingText() const {
        const bool loading =
            std::any_of(m_feeds.begin(), m_feeds.end(), [](const Feed& feed) { return VisiblyLoading(feed.State()); }) ||
            ((m_allMode || m_drainRequested) && !Complete() && !HasError());
        const auto failed = std::find_if(m_feeds.begin(), m_feeds.end(),
                                         [](const Feed& feed) { return !feed.State().error.empty(); });
        return LoadingStatus(loading, failed == m_feeds.end() ? std::wstring() : failed->State().error);
    }

    void Publish() {
        if (!m_source) return;
        if (auto* source = dynamic_cast<AllFavoritesSource*>(m_source))
            source->ReplaceFavorites(m_local, RemoteRows());
        UpdateStatus();
    }

    void UpdateStatus() {
        if (m_source) m_source->SetLoadingText(LoadingText());
    }

    void UpdateSessionMode() {
        const auto mode =
            m_allMode || m_drainRequested ? stream::ListLoadMode::All : stream::ListLoadMode::OnDemand;
        for (auto& feed : m_feeds)
            if (feed.session) feed.session->SetMode(mode);
    }

    void Retry() {
        for (auto& feed : m_feeds)
            if (!feed.State().error.empty() && feed.session) feed.session->Refresh();
    }

    void EnsureAllSongs(std::function<void(bool)> done) {
        if (Complete()) {
            done(true);
            return;
        }
        if (HasError()) {
            done(false);
            return;
        }
        m_drain.Wait(std::move(done));
        m_drainRequested = true;
        UpdateStatus();
        m_host.ListChanged();
        UpdateSessionMode();
        for (auto& feed : m_feeds) {
            if (!feed.State().loading) feed.session->RequestNext();
        }
    }

    void SettleWaiters() {
        if (m_drain.Waiting() && (Complete() || HasError())) m_drain.Settle(Complete() && !HasError());
    }

    ReadAllSongs ReadAll() {
        return [this](SongDrain::Waiter done) { EnsureAllSongs(std::move(done)); };
    }
    SongsFetch AllSongsFetch() { return AllSongs(ReadAll()); }

    void Activate(size_t row, const std::wstring& identity) {
        auto* source = SongSource();
        if (!source) return;
        if (Complete()) {
            source->PlayFrom(row);
            return;
        }
        PlayWhenRead(ReadAll(), identity); // a failed read plays what there is
    }

    std::vector<metadb_handle_ptr> m_local;
    std::vector<Feed> m_feeds;
    media::ListSource* m_source = nullptr;
    uint64_t m_generation = 0;
    bool m_allMode = false;
    bool m_drainRequested = false;
    SongDrain m_drain;
};

} // namespace

std::unique_ptr<Page> MakeAllFavoritesPage(PageHost& host, const View& view) {
    return std::make_unique<AllFavoritesPage>(host, view);
}

} // namespace glasswave::ui
