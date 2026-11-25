#pragma once

#include "media/list_sources.h"
#include "ui/i18n.h"
#include "ui/main_menu.h"
#include "ui/song_actions.h"
#include "ui/view.h"

#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

// Pages whose content arrives asynchronously (a streaming provider's library, the combined
// favorites). A page owns its loading state; the window owns at most one page, the one on
// screen, and destroys it on navigation, which drops every result still in flight.
namespace glasswave::ui {

// The detail band above artist, album and playlist pages.
struct DetailInfo {
    enum class Action { None, Follow, Save, Play };
    // One property of the page's subject, as Windows 7's details pane lists them: a label and
    // its value, both ready to show. Pages list only what the source knows.
    struct Fact {
        std::wstring label, value;
    };
    bool visible = false;
    std::wstring title, subtitle, brief;
    std::wstring tagline;     // one line about the subject, above the brief
    std::vector<Fact> facts;  // in order of importance; the band lays them in columns
    std::wstring artKey;
    metadb_handle_ptr artItem;
    bool artIsArtist = false;
    // A cover made for a list that has none of its own (每日推荐): the picture above (its first
    // song's cover; a gradient without one) under a shade, with a big mark at its top left (the
    // day of the month) and a caption at its foot. Empty: the picture alone.
    std::wstring artBadge, artCaption;
    Action action = Action::None;
    bool actionOn = false;    // following / in the library
    bool actionBusy = false;  // a write is in flight
    std::wstring actionLabel;
    // A page command on the command bar's right (owner, 2026-11-15: an album's or a playlist's
    // Collect moves there, its Play takes the band's button). Empty label: none.
    std::wstring saveLabel;
    bool saveBusy = false;
    // Download all, right of it, on pages whose songs can be downloaded. Empty label: none.
    std::wstring downloadLabel;
};

// One of a page's tabs (the search page's categories), drawn above its list.
struct PageTab {
    std::wstring key;
    std::wstring label; // translated
};

// What a page needs from the window.
class PageHost {
public:
    virtual const media::LibraryModel& Library() const = 0; // the local library's snapshot
    virtual const View& CurrentView() const = 0;
    virtual void Open(const View& view) = 0;     // navigate, adding to history
    virtual void Replace(const View& view) = 0;  // navigate without a new history entry
    virtual void Reload() = 0;                   // rebuild the current page
    // Searches every source for `term` (an album's name from its menu), as the search box does.
    virtual void Search(const std::wstring& term) = 0;
    virtual void Refresh() = 0;                  // re-read the current page from its service
    virtual void SetPageName(const std::wstring& name) = 0;  // the page learned its subject's name
    // Shows `source` in place of the current list. The page's saved view style and the
    // search filter are applied; the selection and the top song are kept where they exist.
    virtual void ShowSource(std::unique_ptr<media::ListSource> source) = 0;
    virtual media::ListSource* Source() const = 0;
    // Called immediately before a page replaces the currently published rows. The window
    // records stable RowKey/occurrence anchors here for the following ListChanged call.
    virtual void BeforeListChange() {}
    virtual void ListChanged() = 0;    // rows changed in place: repaint, keep the top song
    virtual void ChromeChanged() = 0;  // breadcrumb, detail band and navigation lists
    virtual media::ViewStyle SavedStyle(media::ViewStyle fallback) const = 0;
    // Per-view group keys expanded in place (e.g. an artist overview's "Show all" sections),
    // remembered across navigation so returning to a page restores its expanded sections.
    virtual std::set<std::wstring> SavedExpandedGroups() const { return {}; }
    virtual void SaveExpandedGroups(std::set<std::wstring> groups) { (void)groups; }
    virtual HWND Owner() const = 0;
    // Expires when the window goes away; for callbacks that outlive the page.
    virtual std::weak_ptr<int> Lifetime() const = 0;

protected:
    ~PageHost() = default;
};

// "Loading..." on the list showing when some work began (reading a whole list to play it),
// given back when the work ends if that same list is still showing it. The list is known by
// its id, not its address, and the text taken back is the one put there, so a source made
// in between (at the same address, or in another language) is left alone.
class LoadingBadge {
  public:
    LoadingBadge() = default;
    LoadingBadge(const LoadingBadge&) = delete;
    LoadingBadge& operator=(const LoadingBadge&) = delete;
    ~LoadingBadge() { End(); }

    void Begin(PageHost& host, media::ListSource* source) {
        End();
        if (!source) return;
        m_host = &host;
        m_alive = host.Lifetime();
        m_id = source->Id();
        m_previous = source->LoadingText();
        m_text = Tr(L"Loading...");
        source->SetLoadingText(m_text);
        host.ListChanged();
    }
    void End() {
        if (!m_host) return;
        PageHost* host = std::exchange(m_host, nullptr);
        if (m_alive.expired()) return;
        auto* source = host->Source();
        if (!source || source->Id() != m_id || source->LoadingText() != m_text) return;
        source->SetLoadingText(std::move(m_previous));
        host->ListChanged();
    }

  private:
    PageHost* m_host = nullptr;
    std::weak_ptr<int> m_alive;
    uint64_t m_id = 0;
    std::wstring m_previous, m_text;
};

// The menus' "Refresh": the current page read again from its service.
inline MenuAction RefreshAction(PageHost& host) {
    return {L"Refresh", [&host] { host.Refresh(); }};
}

class SongTableSource;

// "Read the whole list, then act" (play from a song, add or download them all) on a page whose
// songs arrive in pages: callers wait until the page has every song (true) or gave up on an
// error (false). The page settles them when its list completes or fails.
class SongDrain {
  public:
    using Waiter = std::function<void(bool complete)>;
    void Wait(Waiter done) { m_waiters.push_back(std::move(done)); }
    bool Waiting() const { return !m_waiters.empty(); }
    // Calls and forgets every waiter (one may wait again).
    void Settle(bool complete) {
        auto waiters = std::exchange(m_waiters, {});
        for (auto& waiter : waiters) waiter(complete);
    }

  private:
    std::vector<Waiter> m_waiters;
};

class Page {
public:
    virtual ~Page() = default;
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    // The list shown at once (cached rows or a loading placeholder); starts the fetches.
    // reuse: Back/Forward or a view style switch, so state kept from an earlier visit may
    // be shown instead of reading the service again.
    virtual std::unique_ptr<media::ListSource> Start(bool reuse) = 0;
    virtual void NearEnd() {}  // the list scrolled close to its last row
    // The source's local filter, sort, grouping or view style changed. Paged pages use this
    // to switch between demand loading and a background full-list drain.
    virtual void OnPresentationChanged() {}
    virtual DetailInfo Detail() const { return {}; }
    // Tabs over the list; empty: none. Tab(): the key of the one shown.
    virtual std::vector<PageTab> Tabs() const { return {}; }
    virtual std::wstring Tab() const { return {}; }
    virtual void SelectTab(const std::wstring& key) { (void)key; }
    virtual void DetailAction() {}
    virtual void SaveAction() {} // the command bar's page command (DetailInfo::saveLabel)
    virtual void DownloadAction() {} // DetailInfo::downloadLabel
    // Commands for the page as a whole (empty list area, detail band).
    virtual void AddPageActions(std::vector<MenuAction>& actions) { (void)actions; }
    // Extra commands for songs selected on this page.
    virtual void AddSongActions(std::vector<MenuAction>& actions, const metadb_handle_list& items) {
        (void)actions, (void)items;
    }
    virtual bool CanRefresh() const { return false; }
    // Refresh was asked for (F5, the menu): called just before the page is built again, so a page
    // that keeps what it read across visits reads it anew this time.
    virtual void RefreshRequested() {}
    // Pages that switch between cover tiles and a song list rebuild on a view style change.
    virtual bool StyleRebuildsPage() const { return false; }
    // True while the page still has initial or expanded-section reads in flight.
    virtual bool IsLoading() const { return false; }
    // A provider's names, covers, counts or library lists changed.
    virtual void OnLibraryChanged() {}
    // Favorite flags changed; song tables refresh their stars by default.
    virtual void OnFavoritesChanged();

protected:
    Page(PageHost& host, View view) : m_host(host), m_view(std::move(view)) {}

    // Wraps a callback so that it does nothing once this page is gone.
    template <class F>
    auto Guard(F f) {
        return [alive = std::weak_ptr<int>(m_alive), f = std::move(f)](auto&&... args) mutable {
            if (!alive.expired()) f(std::forward<decltype(args)>(args)...);
        };
    }
    std::weak_ptr<int> LifetimeToken() const { return m_alive; }

    // The song list on screen, when it is one.
    SongTableSource* SongTable() const;
    // A page's way to read its whole song list: done(true) once every song is there,
    // done(false) when they cannot all be read.
    using ReadAllSongs = std::function<void(SongDrain::Waiter done)>;
    // The songs shown (in their order, through the filter) once the whole list is read; nothing
    // when it cannot be. For the page's bulk actions.
    SongsFetch AllSongs(ReadAllSongs read);
    // Plays the list from the song `identity` (ui/song_table.h) once the whole list is read, or
    // from what there is when it cannot be.
    void PlayWhenRead(ReadAllSongs read, std::wstring identity);
    // A provider is allowed to complete from its cache synchronously. Run requests after
    // Start() has returned so a completed result cannot be overwritten by its placeholder.
    void AfterStart(std::function<void()> request) {
        auto guarded = Guard(std::move(request));
        fb2k::inMainThread([request = std::move(guarded)]() mutable { request(); });
    }

    PageHost& m_host;
    View m_view;

private:
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

}  // namespace glasswave::ui
