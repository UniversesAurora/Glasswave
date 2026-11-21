#pragma once

#include "stream/provider.h"
#include "ui/main_menu.h"

#include <functional>
#include <optional>
#include <variant>
#include <vector>

// The context menu of a service's record - an album, an artist or a playlist - wherever it is
// shown: a tile, a home shelf, a search result, a recently played list, the navigation pane. The
// same kind of thing offers the same commands on every page and from every source (owner,
// 2026-11-21: a recently played album on Home offered only View and Refresh). A page adds only
// its own commands, after the record's. Songs have one menu everywhere already
// (RunContextMenu with MainWindow::BuildContextActions).
//
// Order, each group present when the source and the account allow it:
//   View
//   Play, Play next, Add to queue, Add to playlist, [Download], Add to <source> playlist
//   Collect / Uncollect (album, playlist), Follow / Unfollow (artist), Rename / Delete (own)
//   Go to artist
//   Search for "<name>"
//   the page's own commands (Remove from Recently Played, Refresh...)
namespace glasswave::ui {

class PageHost;

using MenuRecord = std::variant<stream::Album, stream::Artist, stream::Playlist>;

struct RecordMenuContext {
    stream::Id sectionId;               // the source's section the page belongs to
    std::optional<stream::Shelf> shelf; // a playlist's shelf, when the page knows it
    bool onItsPage = false;             // shown on the record's own page: no View
    // The record left the page's list (uncollected on the collected shelf...).
    std::function<void(const stream::Id&)> removed;
    std::vector<MenuAction> page; // the page's own commands, last
};

std::vector<MenuAction> RecordActions(PageHost& host, stream::Provider& provider, const MenuRecord& record,
                                      const RecordMenuContext& context = {});
void ShowRecordMenu(PageHost& host, stream::Provider& provider, const MenuRecord& record, POINT screen,
                    const RecordMenuContext& context = {});

} // namespace glasswave::ui
