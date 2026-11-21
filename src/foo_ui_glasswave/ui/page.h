#pragma once

#include "media/list_sources.h"
#include "ui/main_menu.h"
#include "ui/view.h"

#include <memory>

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
    virtual HWND Owner() const = 0;
    // Expires when the window goes away; for callbacks that outlive the page.
    virtual std::weak_ptr<int> Lifetime() const = 0;

protected:
    ~PageHost() = default;
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
    // Pages that switch between cover tiles and a song list rebuild on a view style change.
    virtual bool StyleRebuildsPage() const { return false; }
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

    PageHost& m_host;
    View m_view;

private:
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

}  // namespace glasswave::ui
