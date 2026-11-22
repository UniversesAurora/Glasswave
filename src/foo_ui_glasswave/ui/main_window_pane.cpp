#include "pch.h"
#include "ui/main_window_parts.h"

// The now-playing pane's lyrics and queue as the library window feeds them, and their menus.
namespace glasswave::ui {

using namespace mainwin;

void MainWindow::ToggleQueuePane() {
    if (!m_pane) return;
    if (!m_lyricsOpen) {
        m_pane->SetMode(NowPlayingPane::Mode::Queue);
        ToggleLyrics();
        return;
    }
    m_pane->SetMode(m_pane->CurrentMode() == NowPlayingPane::Mode::Queue ? NowPlayingPane::Mode::Lyrics
                                                                         : NowPlayingPane::Mode::Queue);
}

void MainWindow::RequestLyrics(const metadb_handle_ptr& track, bool again) {
    const std::wstring identity = lyrics::Service::TrackIdentity(track);
    if (identity != m_lyricsTrackIdentity) ResetLyrics();
    m_lyricsTrack = track;
    m_lyricsTrackIdentity = identity;
    const std::uint64_t request = ++m_lyricsRequestGeneration;
    auto done = [this, alive = std::weak_ptr<int>(m_alive), request, identity](const lyrics::Result& result) {
        if (alive.expired()) return;
        if (!lyrics::IsCurrentLyricsRequest(request, m_lyricsRequestGeneration, identity,
                                            m_lyricsTrackIdentity))
            return;
        OnLyrics(result);
    };
    if (again)
        lyrics::Service::Get().Research(track, done);
    else
        lyrics::Service::Get().Request(track, done);
}

void MainWindow::ResetLyrics() {
    ++m_lyricsRequestGeneration;
    lyrics::Service::Get().Cancel();
    CloseLyricsSearch();
    m_lyricsTrack.release();
    m_lyricsTrackIdentity.clear();
    OnLyrics({});
}

void MainWindow::OnLyrics(const lyrics::Result& result) {
    // Keep the current document and its animated position visible while a same-track
    // refresh is in flight. Actual results replace it only when their content differs.
    if (result.status == lyrics::Status::Searching && m_npLyrics) {
        if (m_pane) m_pane->SetResult(result);
        return;
    }
    // Every view shows the same text: with Swap original and translation on, the translation
    // is the lyric (and is sung word by word) and the original the line under it.
    // The owner's offset for the song (Adjust offset) moves every time first.
    m_lyricsResult = result;
    lyrics::Result shown = result;
    if (shown.status == lyrics::Status::Found && shown.lyrics) {
        const int offset = lyrics::Service::Get().Offset(m_lyricsTrack, *shown.lyrics);
        const bool translated = std::any_of(shown.lyrics->lines.begin(), shown.lyrics->lines.end(),
                                            [](const lyrics::Line& l) { return !l.translation.empty(); });
        const bool swap = translated && settings::SwapTranslation();
        // Hide translation hides the small line, whichever text it holds after a swap.
        const bool hide = translated && settings::HideTranslation();
        if (offset != 0 || swap || hide) {
            lyrics::Lyrics adjusted = *shown.lyrics;
            lyrics::ApplyOffset(adjusted, offset);
            if (swap) adjusted = lyrics::SwapTranslation(adjusted);
            if (hide)
                for (auto& line : adjusted.lines) line.translation.clear();
            shown.lyrics = std::make_shared<const lyrics::Lyrics>(std::move(adjusted));
        }
    }
    m_npLyrics = shown.status == lyrics::Status::Found ? shown.lyrics : nullptr;
    m_npLyricLine = -2;
    if (m_pane) m_pane->SetResult(shown);
    RepaintNowPlaying();
    if (m_player) m_player->OnPlayback();
    if (m_npLyrics && m_npLyrics->synced && m_playing && !m_paused) StartLyricsFrames();
}

void MainWindow::StartLyricsFrames() {
    if (m_paneLent) {
        if (m_player) m_player->StartFrames();
        return;
    }
    if (!PlaybackPaintAllowed()) {
        m_playbackPresentationSuspended = true;
        if (m_frameClock) m_frameClock->Stop();
        return;
    }
    if (!m_frameClock) m_frameClock = std::make_unique<gfx::FrameClock>(m_hwnd, kFrameMessage);
    m_frameClock->Start();
}

// Up Next follows Now Playing after its current song, wrapping once when repeat-all is on.
// History uses the cross-source play history, oldest first.
void MainWindow::SchedulePaneQueueRefresh(bool playbackChanged) {
    m_queueFollowPending |= playbackChanged;
    if (m_queueRefreshPending) return;
    m_queueRefreshPending = true;
    // Playlist batches and playback callbacks can run before the core commits the new
    // playing location. Read one complete snapshot after those callbacks have returned.
    fb2k::inMainThread([this, alive = std::weak_ptr<int>(m_alive)] {
        if (alive.expired()) return;
        m_queueRefreshPending = false;
        const bool follow = std::exchange(m_queueFollowPending, false);
        RefreshPaneQueue(follow);
    });
}

void MainWindow::RefreshPaneQueue(bool playbackChanged) {
    if (!m_pane || !(m_lyricsOpen || m_paneLent)) return;
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    auto pm = playlist_manager::get();
    auto& scheduler = media::PlaybackScheduler::Get();
    const uint64_t currentRevision = scheduler.ScheduleRevision();
    metadb_handle_ptr now;
    const bool loaded = playback_control::get()->get_now_playing(now);
    if (m_paneStartingPreview &&
        (m_paneStartingPreview->scheduleRevision != currentRevision ||
         (loaded && now == m_paneStartingPreview->track && scheduler.CurrentTrack() == now))) {
        m_paneStartingPreview.reset();
    }

    const bool previewing = m_paneStartingPreview.has_value();
    if (!previewing) {
        if (loaded && m_panePreviewedTrack.is_valid() && m_panePreviewedTrack == now) {
            m_panePreviewedTrack.release();
            playbackChanged = false;
        } else if (loaded && now != m_panePreviewedTrack) {
            m_panePreviewedTrack.release();
        }
    }

    std::vector<QueueView::Row> upNext;
    m_paneUpcoming.clear();
    m_paneScheduleRevision = currentRevision;
    const t_size queue = media::QueuePlaylist(false);
    size_t repeatCycle = 0;
    if (queue != SIZE_MAX) {
        const t_size count = pm->playlist_get_item_count(queue);
        t_size index = SIZE_MAX;
        metadb_handle_ptr cursor;
        if (previewing) {
            index = m_paneStartingPreview->playingIndex < count ? m_paneStartingPreview->playingIndex : SIZE_MAX;
            cursor = m_paneStartingPreview->track;
        } else {
            t_size playing = SIZE_MAX;
            const bool fromQueue = pm->get_playing_item_location(&playing, &index) && playing == queue;
            if (!fromQueue) index = playback_control::get()->is_playing() ? SIZE_MAX : m_panePlayingIndex;
            if (index >= count) index = SIZE_MAX;
            if (index != SIZE_MAX) cursor = pm->playlist_get_item_handle(queue, index);
            if (!fromQueue && cursor != m_panePlayingTrack) {
                // A stopped cursor must not survive replacing or removing its old item.
                index = SIZE_MAX;
                cursor.release();
            }
        }
        playbackChanged |= index != m_panePlayingIndex;
        m_panePlayingIndex = index;
        m_panePlayingTrack = cursor;
        const auto upcoming = previewing ? m_paneStartingPreview->upcoming : scheduler.DisplayUpcomingSnapshot();
        repeatCycle = previewing ? m_paneStartingPreview->repeatCycle : scheduler.RepeatCycleSize();
        std::unordered_set<size_t> shownPhysical;
        for (const auto& next : upcoming) {
            if (!next.handle.is_valid()) continue;
            if (next.source == media::ScheduledUpcomingSource::Playlist && next.playlistIndex >= count)
                continue;
            QueueView::Row row;
            row.handle = next.handle;
            pfc::string8 text;
            row.handle->format_title(nullptr, text, s_title, nullptr);
            row.title = Wide(text);
            row.handle->format_title(nullptr, text, s_artist, nullptr);
            row.artist = Wide(text);
            TrackText(row.handle, row.title, row.artist);
            row.badges = stream::TrackBadges(row.handle);
            row.playlistIndex = next.playlistIndex < count && shownPhysical.insert(next.playlistIndex).second
                                    ? next.playlistIndex
                                    : QueueView::kExplicitQueueTag | next.queueIndex;
            m_paneUpcoming.emplace(row.playlistIndex, next);
            upNext.push_back(std::move(row));
        }
    }
    std::vector<QueueView::Row> history;
    auto records = media::PlayStats::Get().Recent(50);
    for (auto it = records.rbegin(); it != records.rend(); ++it) { // oldest first
        QueueView::Row row;
        if (!it->provider.empty()) {
            if (auto* provider = stream::FindProvider(it->provider))
                row.handle = provider->TrackItem(it->trackId);
        } else {
            row.handle = metadb::get()->handle_create(it->path.c_str(), it->subsong);
        }
        if (!row.handle.is_valid()) continue;
        row.title = Wide(it->title.c_str());
        row.artist = Wide(it->artist.c_str());
        row.badges = stream::TrackBadges(row.handle);
        history.push_back(std::move(row));
    }
    if (previewing) {
        const auto& startingTrack = m_paneStartingPreview->track;
        const auto& previousTrack = m_paneStartingPreview->previousTrack;
        // The song now opening moves onto the top card, not history.
        std::erase_if(history, [&](const QueueView::Row& r) { return r.handle == startingTrack; });
        // The song leaving the card moves immediately to the bottom of history (just above the card).
        if (previousTrack.is_valid() && previousTrack != startingTrack) {
            std::erase_if(history, [&](const QueueView::Row& r) { return r.handle == previousTrack; });
            QueueView::Row prevRow;
            prevRow.handle = previousTrack;
            pfc::string8 text;
            previousTrack->format_title(nullptr, text, s_title, nullptr);
            prevRow.title = Wide(text);
            previousTrack->format_title(nullptr, text, s_artist, nullptr);
            prevRow.artist = Wide(text);
            TrackText(previousTrack, prevRow.title, prevRow.artist);
            prevRow.badges = stream::TrackBadges(previousTrack);
            history.push_back(std::move(prevRow));
        }
        m_panePreviewedTrack = startingTrack;
    } else {
        // The newest record is the song playing now: it is the card, not history.
        if (loaded && !history.empty() && history.back().handle == now) history.pop_back();
    }
    m_pane->Queue().SetHistory(std::move(history));
    m_pane->Queue().SetUpNext(std::move(upNext), playbackChanged);
    m_pane->Queue().SetShuffleRepeat(m_shuffle, static_cast<int>(m_repeat), media::radio::Active());
    m_pane->Queue().SetRepeatCycle(repeatCycle);
}

void MainWindow::ConnectPaneQueue() {
    auto& queue = m_pane->Queue();
    queue.onPlay = [this](size_t index) {
        if (media::PlaybackScheduler::Get().ScheduleRevision() != m_paneScheduleRevision) {
            OnCoreQueueChanged();
            return;
        }
        auto pm = playlist_manager::get();
        if (QueueView::IsExplicitQueue(index)) {
            pfc::list_t<t_playback_queue_item> queued;
            pm->queue_get_contents(queued);
            const size_t at = QueueView::ExplicitQueueIndex(index);
            if (at >= queued.get_count()) return;
            const auto target = queued[at];
            if (target.m_playlist != SIZE_MAX && target.m_item != SIZE_MAX) {
                if (target.m_playlist == media::QueuePlaylist(false))
                    media::PlaybackScheduler::Get().ExpectExactStart(target.m_item);
                pm->set_playing_playlist(target.m_playlist);
                pm->playlist_execute_default_action(target.m_playlist, target.m_item);
            } else if (target.m_handle.is_valid()) {
                metadb_handle_list items;
                items.add_item(target.m_handle);
                const bool wasPlaying = playback_control::get()->is_playing();
                media::PlayNext(items);
                if (wasPlaying) playback_control::get()->start(play_control::track_command_next);
            }
            return;
        }
        const t_size p = media::QueuePlaylist(false);
        if (p == SIZE_MAX || index >= pm->playlist_get_item_count(p)) return;
        media::PlaybackScheduler::Get().ExpectExactStart(index);
        pm->set_playing_playlist(p);
        pm->playlist_execute_default_action(p, index);
    };
    queue.onPlayHistory = [](const metadb_handle_ptr& track) {
        // History activation uses the same priority insertion as every other Play Next action.
        metadb_handle_list items;
        items.add_item(track);
        const bool wasPlaying = playback_control::get()->is_playing();
        media::PlayNext(items);
        if (wasPlaying) playback_control::get()->start(play_control::track_command_next);
    };
    queue.onClearHistory = [this] {
        ClearPlayStats(true);
        RefreshPaneQueue();
    };
    queue.onShuffle = [this] {
        if (!media::radio::Active()) SetShuffleRepeat(!m_shuffle, m_repeat); // no shuffle on a station
    };
    queue.onRepeat = [this] { CycleRepeat(); };
    queue.onReorder = [this](const std::vector<size_t>& slots, const std::vector<size_t>& items) {
        std::vector<media::ScheduledUpcomingEntry> order;
        std::unordered_set<size_t> seen;
        bool valid = slots.size() == items.size() && items.size() == m_paneUpcoming.size();
        order.reserve(items.size());
        for (const size_t item : items) {
            const auto found = m_paneUpcoming.find(item);
            if (found == m_paneUpcoming.end() || !seen.insert(item).second) {
                valid = false;
                break;
            }
            order.push_back(found->second);
        }
        if (valid) media::PlaybackScheduler::Get().ReorderUpcomingSnapshot(m_paneScheduleRevision, order);
        // A stale drag is discarded and repainted from the current committed schedule.
        OnCoreQueueChanged();
    };
    queue.onRowMenu = [this](size_t index, POINT screen) {
        std::vector<MenuAction> actions;
        actions.push_back({Tr(L"Remove from Now Playing"), [this, index, revision = m_paneScheduleRevision] {
                               if (media::PlaybackScheduler::Get().ScheduleRevision() != revision) {
                                   OnCoreQueueChanged();
                                   return;
                               }
                               auto pm = playlist_manager::get();
                               if (QueueView::IsExplicitQueue(index)) {
                                   const size_t at = QueueView::ExplicitQueueIndex(index);
                                   if (at < pm->queue_get_count())
                                       pm->queue_remove_mask(pfc::bit_array_one(at));
                                   return;
                               }
                               const t_size p = media::QueuePlaylist(false);
                               if (p == SIZE_MAX || index >= pm->playlist_get_item_count(p)) return;
                               pm->playlist_undo_backup(p);
                               pm->playlist_remove_items(p, pfc::bit_array_one(index));
                           }});
        ShowActionMenu(m_hwnd, screen, actions);
    };
}

void MainWindow::ShowNowPlayingSongMenu(POINT screen) {
    const HWND owner = m_player && m_player->Shown() ? m_player->Hwnd() : m_hwnd;
    metadb_handle_ptr now;
    if (!playback_control::get()->get_now_playing(now)) return;
    RunContextMenu(owner, pfc::list_single_ref_t<metadb_handle_ptr>(now), screen, true);
}

void MainWindow::ShowLyricsMenu(POINT screen) {
    // While the player window shows (the library window is hidden), it owns the menu and dialogs.
    const HWND owner = m_player && m_player->Shown() ? m_player->Hwnd() : m_hwnd;
    metadb_handle_ptr now;
    std::vector<MenuAction> actions;
    if (m_npLyrics && !m_npLyrics->source.empty())
        actions.push_back({Tr(L"Lyric source") + L"  " + Tr(m_npLyrics->source), nullptr});
    if (playback_control::get()->get_now_playing(now)) {
        AddLyricsDocumentActions(actions, owner, now);
        const auto search = [this, now, owner](std::wstring source) {
            ShowLyricsSearch(
                owner, now, std::move(source),
                [this, now, alive = std::weak_ptr<int>(m_alive)](const lyrics::Candidate& candidate) {
                    if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity)
                        return;
                    OnLyrics(lyrics::Service::Get().Select(now, candidate));
                });
        };
        actions.push_back({Tr(L"Search lyrics..."), [search] { search(L""); }});
        std::vector<MenuAction> sources;
        for (const auto& source : lyrics::scripts::Ordered(lyrics::Service::QueryFor(now).provider))
            sources.push_back({Tr(source->Name()), [search, key = source->Key()] { search(key); }});
        if (!sources.empty()) actions.push_back({Tr(L"Search lyric source"), nullptr, std::move(sources)});
        actions.push_back({Tr(L"Load lyrics from file..."), [this, now, owner, alive = std::weak_ptr<int>(m_alive)] {
                               const auto path = PickLyricsFile(owner);
                               if (alive.expired() || path.empty() ||
                                   lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity)
                                   return;
                               const auto result = lyrics::Service::Get().LoadFile(now, path);
                               if (result.status == lyrics::Status::Found)
                                   OnLyrics(result);
                               else
                                   Notice(owner, Tr(L"Lyrics"), Tr(L"The file contains no readable lyrics."));
                           }});
        actions.push_back({Tr(L"Reload lyrics"), [this, now] { RequestLyrics(now, false); }});
        actions.push_back({Tr(L"Search for lyrics again"), [this, now] { RequestLyrics(now, true); }});
        actions.push_back({Tr(L"Use automatic lyrics"), [this, now] {
                               lyrics::Service::Get().UseAutomatic(now);
                               RequestLyrics(now, false);
                           }});
        actions.push_back({});
    }
    // The single line under the song, everywhere it shows; switched back on from here too (the
    // menu also opens on the song's title and artist when the line is hidden).
    const bool lineShown = settings::LyricLineShown();
    actions.push_back({Tr(lineShown ? L"Hide lyric line" : L"Show lyric line"),
                       [this, lineShown] { SetLyricLineShown(!lineShown); }});
    // How the lyrics look, as in the preferences: their size in steps of 10 %, and the side
    // their translations stand on.
    const auto restyle = [this] {
        if (m_pane) m_pane->RestyleLyrics();
    };
    std::vector<MenuAction> size{
        {Tr(L"Larger"), [restyle] {
             settings::SetLyricSizePercent(settings::LyricSizePercent() + 10);
             restyle();
         }},
        {Tr(L"Smaller"), [restyle] {
             settings::SetLyricSizePercent(settings::LyricSizePercent() - 10);
             restyle();
         }},
        {Tr(L"Default size"), [restyle] {
             settings::SetLyricSizePercent(settings::kLyricSize.defaultValue);
             restyle();
         }}};
    actions.push_back({Tr(L"Lyrics text size"), nullptr, std::move(size)});
    const bool swapped = settings::SwapTranslation();
    MenuAction swap{Tr(L"Swap original and translation"), [this, swapped] {
                        settings::SetSwapTranslation(!swapped);
                        OnLyrics(m_lyricsResult);
                    }};
    swap.checked = swapped;
    swap.checkbox = true;
    actions.push_back(std::move(swap));
    // The small line under each lyric: the translation, or the original when swapped.
    const bool hidden = settings::HideTranslation();
    MenuAction hide{Tr(swapped ? L"Hide original" : L"Hide translation"), [this, hidden] {
                        settings::SetHideTranslation(!hidden);
                        OnLyrics(m_lyricsResult);
                    }};
    hide.checked = hidden;
    hide.checkbox = true;
    actions.push_back(std::move(hide));
    const auto subHighlight = settings::GetSecondaryLyricHighlight();
    std::vector<MenuAction> subHighlightMenu;
    for (const auto& [mode, label] : {std::pair{settings::SecondaryLyricHighlight::Off, L"Off"},
                                      std::pair{settings::SecondaryLyricHighlight::Line, L"Highlight whole line"},
                                      std::pair{settings::SecondaryLyricHighlight::WordProgress,
                                                L"Follow word-by-word progress"}}) {
        MenuAction item{Tr(label), [mode, restyle] {
                            settings::SetSecondaryLyricHighlight(mode);
                            restyle();
                        }};
        item.checked = subHighlight == mode;
        subHighlightMenu.push_back(std::move(item));
    }
    actions.push_back({Tr(L"Secondary lyric line highlight"), nullptr, std::move(subHighlightMenu)});
    if (m_lyricsOpen && owner == m_hwnd) actions.push_back({Tr(L"Close lyrics pane"), [this] { ToggleLyrics(); }});
    ShowActionMenu(owner, screen, actions);
}

// ESLyric's tools for the lyrics on show (owner, 2026-11-13): save (to the tag, the profile,
// the track's folder or a chosen file), select another found lyric, copy, read the file in
// another code page, the document itself, and the timing offset. They act on the lyrics as
// found; the offset is saved with them as [offset:] and copying writes the adjusted times.
void MainWindow::AddLyricsDocumentActions(std::vector<MenuAction>& actions, HWND owner, const metadb_handle_ptr& now) {
    using lyrics_tools::SaveTarget;
    const bool current = lyrics::Service::TrackIdentity(now) == m_lyricsTrackIdentity;
    const auto found = current && m_lyricsResult.status == lyrics::Status::Found ? m_lyricsResult.lyrics : nullptr;
    const int offset = found ? lyrics::Service::Get().Offset(now, *found) : 0;
    const auto alive = std::weak_ptr<int>(m_alive);
    // Each action checks that the song (and its lyrics) are still the ones the menu was for.
    const auto still = [this, alive, now, found] {
        return !alive.expired() && lyrics::Service::TrackIdentity(now) == m_lyricsTrackIdentity &&
               m_lyricsResult.lyrics == found;
    };
    const auto document = [found, offset] {
        if (!found) return std::wstring();
        lyrics::WriteOptions all{true, true, false, true, false};
        return lyrics::AddOffsetTag(found->document.empty() ? lyrics::WriteLrc(*found, all) : found->document, offset);
    };
    const auto save = [owner, now, document, still](SaveTarget target) {
        if (still()) lyrics_tools::Save(owner, now, document(), target);
    };
    const auto disabled = [](MenuAction a, bool off) {
        a.disabled = off;
        return a;
    };

    actions.push_back(disabled({Tr(L"Save lyrics"), [save] { save(SaveTarget::Default); }}, !found));
    std::vector<MenuAction> saveTo;
    for (const auto& [target, label] : {std::pair{SaveTarget::Tag, L"Tag"}, std::pair{SaveTarget::Profile, L"Profile folder"},
                                        std::pair{SaveTarget::TrackFolder, L"Track's folder"},
                                        std::pair{SaveTarget::Browse, L"Browse..."}})
        saveTo.push_back(disabled({Tr(label), [save, target] { save(target); }},
                                  !found || !lyrics_tools::CanSave(now, target)));
    actions.push_back(disabled({Tr(L"Save lyrics to"), nullptr, std::move(saveTo)}, !found));

    // Select lyrics: what the searches for this song found, the shown one checked.
    std::vector<MenuAction> select;
    for (const auto& c : lyrics::Service::Get().Candidates(now)) {
        std::wstring label = c.title.empty() ? Tr(L"(untitled)") : c.title;
        if (!c.artist.empty()) label += L" - " + c.artist;
        if (label.size() > 60) label = label.substr(0, 57) + L"...";
        label += L"  (" + Tr(c.source) + L")";
        MenuAction pick{std::move(label), [this, now, c, alive] {
                            if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity) return;
                            OnLyrics(lyrics::Service::Get().Select(now, c));
                        }};
        pick.checked = found && found->document == c.lrc;
        select.push_back(std::move(pick));
    }
    if (select.empty()) select.push_back(disabled({Tr(L"No other lyrics found yet"), nullptr}, true));
    {
        // None at all, until "Use automatic lyrics" (owner, 2026-11-14).
        MenuAction none{Tr(L"No lyrics"), [this, now, alive] {
                            if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity) return;
                            OnLyrics(lyrics::Service::Get().SelectNone(now));
                        }};
        none.checked = current && m_lyricsResult.status == lyrics::Status::NotFound;
        select.push_back(std::move(none));
    }
    select.push_back({});
    select.push_back({Tr(L"Search lyrics..."), [this, now, owner, alive] {
                          ShowLyricsSearch(owner, now, L"", [this, now, alive](const lyrics::Candidate& candidate) {
                              if (alive.expired() || lyrics::Service::TrackIdentity(now) != m_lyricsTrackIdentity)
                                  return;
                              OnLyrics(lyrics::Service::Get().Select(now, candidate));
                          });
                      }});
    actions.push_back({Tr(L"Select lyrics"), nullptr, std::move(select)});

    // Copy: with the remembered options at once, or choosing them first. The times are the
    // ones shown (offset applied), the lines the original's.
    const auto copy = [owner, found, offset, still](bool ask) {
        if (!still() || !found) return;
        const int flags = settings::LyricsCopyOptions();
        lyrics::WriteOptions options{(flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0, (flags & 8) != 0,
                                     (flags & 16) != 0};
        if (ask) {
            if (!lyrics_tools::ShowCopyDialog(owner, options, found->karaoke)) return;
            settings::SetLyricsCopyOptions((options.metadata ? 1 : 0) | (options.timestamps ? 2 : 0) |
                                           (options.mergeSame ? 4 : 0) | (options.references ? 8 : 0) |
                                           (options.standard ? 16 : 0));
        }
        lyrics::Lyrics adjusted = *found;
        lyrics::ApplyOffset(adjusted, offset);
        lyrics_tools::CopyText(owner, lyrics::WriteLrc(adjusted, options));
    };
    actions.push_back(disabled({Tr(L"Copy lyrics"), nullptr,
                                {{Tr(L"Copy lyrics"), [copy] { copy(false); }}, {Tr(L"Copy..."), [copy] { copy(true); }}}},
                               !found));

    // Encoding: only lyrics read from a file can be read again.
    std::vector<MenuAction> encodings;
    const auto reencode = [this, now, found, owner, still](unsigned codePage) {
        if (!still()) return;
        const auto result = lyrics::Service::Get().Reencode(now, *found, codePage);
        if (result.status == lyrics::Status::Found)
            OnLyrics(result);
        else
            Notice(owner, Tr(L"Lyrics"), Tr(L"The lyric file could not be read in this encoding."));
    };
    MenuAction autoDetect{Tr(L"Auto detect"), [reencode] { reencode(0); }};
    autoDetect.checked = found && found->codePage == 0;
    encodings.push_back(std::move(autoDetect));
    encodings.push_back({});
    for (const auto& [codePage, name] : lyrics_tools::CodePages()) {
        MenuAction item{name, [reencode, cp = codePage] { reencode(cp); }};
        item.checked = found && found->codePage == codePage;
        encodings.push_back(std::move(item));
    }
    actions.push_back(disabled({Tr(L"Lyrics encoding"), nullptr, std::move(encodings)}, !found || found->file.empty()));

    actions.push_back(disabled({Tr(L"Lyric info"), [owner, found, offset, still] {
                                    if (!still()) return;
                                    std::wstring header = Tr(L"Source") + L": " +
                                                          (found->source.empty() ? L"-" : Tr(found->source));
                                    if (!found->file.empty()) header += L"\n" + Tr(L"File") + L": " + found->file;
                                    if (!found->file.empty())
                                        header += L"\n" + Tr(L"Encoding") + L": " +
                                                  (found->codePage ? std::to_wstring(found->codePage) : Tr(L"Auto detect"));
                                    header += L"\n" + Tr(L"Offset") + L": " + std::to_wstring(offset) + L" ms";
                                    lyrics_tools::ShowTextWindow(owner, Tr(L"Lyric info"), header, found->document);
                                }},
                               !found));

    // Offset in steps of 100 ms or 1 s, or as typed; positive shows the lyrics earlier ([offset:]).
    const auto setOffset = [this, now, found, still](int ms) {
        if (!still() || !found) return;
        lyrics::Service::Get().SetOffset(now, *found, ms);
        OnLyrics(m_lyricsResult);
    };
    std::vector<MenuAction> offsets{
        {Tr(L"+100 ms (earlier)"), [setOffset, offset] { setOffset(offset + 100); }},
        {Tr(L"-100 ms (later)"), [setOffset, offset] { setOffset(offset - 100); }},
        {Tr(L"+1 s (earlier)"), [setOffset, offset] { setOffset(offset + 1000); }},
        {Tr(L"-1 s (later)"), [setOffset, offset] { setOffset(offset - 1000); }},
        {Tr(L"Custom offset..."), [setOffset, offset, owner] {
             int ms = offset;
             if (lyrics_tools::AskOffset(owner, ms)) setOffset(ms);
         }},
        disabled({Tr(L"Reset offset"), [setOffset] { setOffset(0); }}, offset == 0)};
    std::wstring offsetLabel = Tr(L"Adjust offset");
    if (offset != 0) offsetLabel += L"  (" + std::wstring(offset > 0 ? L"+" : L"") + std::to_wstring(offset) + L" ms)";
    actions.push_back(disabled({std::move(offsetLabel), nullptr, std::move(offsets)}, !found || !found->synced));
    actions.push_back({});
}

} // namespace glasswave::ui
