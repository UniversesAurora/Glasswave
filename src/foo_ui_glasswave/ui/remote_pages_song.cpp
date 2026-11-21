#include "pch.h"
#include "ui/remote_pages_parts.h"

// One song's page: its details and credits.
namespace glasswave::ui::remotepages {

namespace {

// ---- one song --------------------------------------------------------------------------------

// A song's page: the details band holds the song (cover, title, artist and album, every detail
// the service has, Play); the list under it the credits in the service's groups and the
// similar songs. A source shows only what it has: Apple Music credits, NetEase similar songs.
class SongPage final : public Page {
  public:
    SongPage(PageHost& host, stream::Provider& provider, View view)
        : Page(host, std::move(view)), m_provider(provider) {}

    std::unique_ptr<media::ListSource> Start(bool) override {
        AfterStart([this] {
            m_provider.FetchSongDetails(m_view.remoteId,
                                        Guard([this](stream::SongDetails details, std::wstring error) {
                                            m_error = std::move(error);
                                            if (m_error.empty()) {
                                                m_details = std::move(details);
                                                if (!m_details->track.title.empty())
                                                    m_host.SetPageName(m_details->track.title);
                                            }
                                            m_host.ShowSource(Source());
                                            m_host.ChromeChanged();
                                        }));
        });
        return Source();
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.title = m_details && !m_details->track.title.empty() ? m_details->track.title : m_view.name;
        if (d.title.empty()) d.title = Tr(L"Song");
        const metadb_handle_ptr item = m_provider.TrackItem(m_view.remoteId);
        if (item.is_valid()) {
            d.artItem = item;
            d.artKey = ArtCache::SourceKey(item);
        }
        d.action = DetailInfo::Action::Play;
        d.actionLabel = Tr(L"Play");
        if (!m_details) return d;
        const stream::Track& track = m_details->track;
        const std::wstring released = m_details->released.size() >= 4 ? m_details->released.substr(0, 4) : L"";
        d.subtitle = Joined({stream::JoinArtists(track.artists), track.album, released});
        d.facts = Facts(*m_details);
        return d;
    }

    void DetailAction() override {
        const metadb_handle_ptr item = m_provider.TrackItem(m_view.remoteId);
        if (!item.is_valid()) return;
        metadb_handle_list items;
        items.add_item(item);
        media::PlayItems(items, 0);
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (!m_details) return;
        const stream::Track& track = m_details->track;
        AddGoToArtist(actions, m_host, m_provider, track.artists, {});
        if (!track.albumId.empty())
            actions.push_back({L"Go to album", [this, track] {
                                   View v = AlbumView(m_provider, track.albumId, track.album);
                                   v.select = m_provider.TrackItem(track.id); // shown selected there
                                   m_host.Open(v);
                               }});
    }

  private:
    std::unique_ptr<media::ListSource> Source() {
        const std::wstring empty = !m_error.empty() ? m_error
                                   : !m_details     ? Tr(L"Loading...")
                                                    : Tr(L"The service lists no credits for this song.");
        return std::make_unique<SongInfoSource>(
            m_provider, m_details ? *m_details : stream::SongDetails{}, empty,
            [this](const stream::Id& artist, const std::wstring& name) {
                m_host.Open(ArtistView(m_provider, artist, name));
            });
    }

    static std::vector<DetailInfo::Fact> Facts(const stream::SongDetails& song) {
        std::wstring position;
        if (song.number > 0) {
            position = Fill(Tr(L"Track {0}"), {std::to_wstring(song.number)});
            if (song.disc > 0) position = Fill(Tr(L"Disc {0}"), {std::to_wstring(song.disc)}) + L" · " + position;
        }
        return FactList()
            .Add(L"Length", song.track.durationMs > 0 ? FormatTime(song.track.durationMs / 1000.0) : L"")
            .Add(L"Position", position)
            .Date(L"Released", song.released)
            .List(L"Genre", song.genres)
            .List(L"Tags", song.tags)
            .Add(L"Language", LanguageText(song.language))
            .Add(L"BPM", song.bpm > 0 ? std::to_wstring(song.bpm) : L"")
            .Key(L"Version", song.version)
            .List(L"Audio", song.traits, true)
            .Key(L"Lyrics", song.lyrics)
            .Add(L"Composer", song.credits.empty() ? song.composer : std::wstring()) // else among the credits
            .List(L"Also known as", song.aliases)
            .Add(L"ISRC", song.isrc)
            .Take();
    }

    // A language tag ("zh-Hans-CN") as the system names the language; other text as it is.
    static std::wstring LanguageText(const std::wstring& language) {
        if (language.find(L'-') == std::wstring::npos) return language;
        wchar_t name[LOCALE_NAME_MAX_LENGTH * 2]{};
        if (GetLocaleInfoEx(language.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, name, static_cast<int>(std::size(name))))
            return name;
        return language;
    }

    stream::Provider& m_provider;
    std::optional<stream::SongDetails> m_details;
    std::wstring m_error;
};

} // namespace

std::unique_ptr<Page> MakeSongPage(PageHost& host, stream::Provider& provider, const View& view) {
    switch (view.kind) {
    case View::Kind::RemoteSong:
        return std::make_unique<SongPage>(host, provider, view);
    default:
        return nullptr;
    }
}

} // namespace glasswave::ui::remotepages
