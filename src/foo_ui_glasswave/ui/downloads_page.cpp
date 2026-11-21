#include "pch.h"
#include "ui/downloads_page.h"

#include "download/downloads.h"
#include "stream/provider.h"
#include "ui/i18n.h"
#include "ui/main_menu.h"
#include "ui/remote_grids.h"
#include "ui/text.h"
#include "ui/win7_menu.h"

#include <ShlObj.h>
#include <shobjidl.h>

#include <ctime>

namespace glasswave::ui {

namespace {

using download::Item;
using download::State;

constexpr const wchar_t* kDone = L"done"; // the Downloaded tab's key; Downloading has none

std::wstring Bytes(uint64_t bytes) {
    wchar_t text[32];
    if (bytes >= (1ull << 30))
        swprintf_s(text, L"%.2f GB", static_cast<double>(bytes) / (1ull << 30));
    else if (bytes >= (1ull << 20))
        swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1ull << 20));
    else
        swprintf_s(text, L"%.0f KB", static_cast<double>(bytes) / (1ull << 10));
    return text;
}

std::wstring Speed(double bytesPerSecond) {
    if (bytesPerSecond <= 0) return {};
    return Bytes(static_cast<uint64_t>(bytesPerSecond)) + L"/s";
}

std::wstring When(int64_t seconds) {
    if (seconds <= 0) return {};
    const time_t t = static_cast<time_t>(seconds);
    tm local{};
    if (localtime_s(&local, &t) != 0) return {};
    wchar_t text[32];
    swprintf_s(text, L"%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
               local.tm_min);
    return text;
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Selects the file in a new Explorer window.
void Reveal(const std::wstring& path) {
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
    if (!item) {
        ShellExecuteW(nullptr, L"open", FolderOf(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);
}

void OpenFolder(const std::wstring& folder) {
    CreateDirectoryW(folder.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

metadb_handle_ptr Handle(const Item& item) {
    stream::Provider* provider = stream::FindProvider(item.provider);
    return provider ? provider->TrackItem(item.track) : metadb_handle_ptr();
}

} // namespace

std::wstring PickDownloadFolder(HWND owner) {
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(Tr(L"Download to").c_str());
    winrt::com_ptr<IShellItem> start;
    const std::wstring folder = download::Folder();
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(start.put()))))
        dialog->SetFolder(start.get());
    if (dialog->Show(owner) != S_OK) return {};
    winrt::com_ptr<IShellItem> result;
    if (FAILED(dialog->GetResult(result.put()))) return {};
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

namespace {

// The common save dialog for one song, its name suggested by the naming rules. The extension
// is the file's own once the source has said what it delivers.
std::wstring PickFile(HWND owner, const metadb_handle_ptr& handle) {
    std::wstring name = L"Song";
    if (stream::Provider* provider = stream::ProviderOf(handle))
        if (const auto track = provider->FindTrack(provider->TrackId(handle))) {
            const std::wstring artists = stream::JoinArtists(track->artists);
            switch (download::GetNaming()) {
            case download::Naming::Title: name = track->title; break;
            case download::Naming::ArtistTitle: name = artists.empty() ? track->title : artists + L" - " + track->title; break;
            case download::Naming::TitleArtist: name = artists.empty() ? track->title : track->title + L" - " + artists; break;
            }
        }
    for (auto& c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    winrt::com_ptr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    const std::wstring filterName = Tr(L"Audio file");
    const COMDLG_FILTERSPEC filter[] = {{filterName.c_str(), L"*.*"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetFileName(name.c_str());
    winrt::com_ptr<IShellItem> start;
    const std::wstring folder = download::Folder();
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(start.put()))))
        dialog->SetFolder(start.get());
    if (dialog->Show(owner) != S_OK) return {};
    winrt::com_ptr<IShellItem> result;
    if (FAILED(dialog->GetResult(result.put()))) return {};
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

// Queues the songs; says so when none could be (all downloaded or queued already).
void Queue(HWND owner, const metadb_handle_list& items, const std::wstring& root) {
    if (!download::AnyDownloadable(items)) {
        MessageBoxW(owner, Tr(L"None of these songs can be downloaded.").c_str(), Tr(L"Download").c_str(),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (download::Add(items, root) == 0)
        MessageBoxW(owner, Tr(L"These songs are already downloaded or downloading.").c_str(), Tr(L"Download").c_str(),
                    MB_OK | MB_ICONINFORMATION);
}

// ---- the lists --------------------------------------------------------------------------------

enum Column : size_t { kArt, kTitle, kArtist, kAlbum, kSource, kProgress, kStatus, kSize, kFormat, kWhen, kWhere };

class DownloadsSource final : public GridSource {
  public:
    DownloadsSource(bool done, std::vector<download::Item> items, std::function<void(POINT)> menu)
        : GridSource(Layout(done), Tr(done ? L"No downloaded songs yet." : L"No songs are downloading.")),
          m_done(done), m_menu(std::move(menu)) {
        m_fields = done ? std::vector<Column>{kArt, kTitle, kArtist, kAlbum, kSource, kFormat, kSize, kWhen, kWhere}
                        : std::vector<Column>{kArt, kTitle, kArtist, kAlbum, kSource, kProgress, kStatus, kSize, kFormat};
        Update(std::move(items));
    }

    std::wstring ColumnSetKey() const override { return m_done ? L"downloaded-songs" : L"downloading-songs"; }
    std::wstring Cell(size_t row, size_t column) const override {
        return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
    }
    bool CanSort(size_t column) const override {
        return column < m_fields.size() && m_fields[column] != kProgress && m_fields[column] != kSize && m_fields[column] != kArt;
    }
    metadb_handle_ptr Item(size_t row) const override {
        return row < m_rows.size() ? Handle(m_items[m_rows[row]]) : metadb_handle_ptr();
    }
    // The song's cover, as every song list shows it, and the playing song marked in the gutter.
    std::wstring ArtKey(size_t row) const override {
        if (row >= m_rows.size()) return {};
        const auto handle = Handle(m_items[m_rows[row]]);
        stream::Provider* provider = handle.is_valid() ? stream::ProviderOf(handle) : nullptr;
        return provider ? provider->ArtKeyOf(handle) : std::wstring();
    }
    bool IsPlaying(size_t row) const override {
        metadb_handle_ptr now;
        return row < m_rows.size() && playback_control::get()->get_now_playing(now) && now == Item(row);
    }
    metadb_handle_list SelectedItems() const override {
        metadb_handle_list out;
        for (size_t row = 0; row < m_rows.size(); ++row)
            if (IsSelected(row))
                if (auto handle = Handle(m_items[m_rows[row]]); handle.is_valid()) out.add_item(handle);
        return out;
    }
    void Activate(size_t row) override {
        if (row >= m_rows.size()) return;
        if (!m_done) { // a stopped download starts again
            download::Resume({m_items[m_rows[row]].id});
            return;
        }
        metadb_handle_list items; // the list from this song on, as a song list plays
        size_t start = 0;
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (auto handle = Handle(m_items[m_rows[r]]); handle.is_valid()) {
                if (r == row) start = items.get_count();
                items.add_item(handle);
            }
        if (items.get_count()) media::PlayItems(items, start);
    }
    void ContextMenu(HWND owner, POINT screen) override {
        if (!m_done) {
            if (m_menu) m_menu(screen);
            return;
        }
        // Downloaded songs are songs: the menu of every song list, with the file's commands
        // (DownloadsPage::AddSongActions).
        if (const auto items = SelectedItems(); items.get_count()) RunContextMenu(owner, items, screen);
    }
    bool DeleteSelected() override {
        const auto ids = SelectedIds();
        if (ids.empty()) return false;
        if (m_done)
            download::Forget(ids, false);
        else
            download::Cancel(ids);
        return true;
    }

    std::vector<uint64_t> SelectedIds() const {
        std::vector<uint64_t> ids;
        for (size_t row = 0; row < m_rows.size(); ++row)
            if (IsSelected(row)) ids.push_back(m_items[m_rows[row]].id);
        if (ids.empty() && Focus() < m_rows.size()) ids.push_back(m_items[m_rows[Focus()]].id);
        return ids;
    }
    std::vector<download::Item> Selected() const {
        std::vector<download::Item> out;
        for (size_t row = 0; row < m_rows.size(); ++row)
            if (IsSelected(row)) out.push_back(m_items[m_rows[row]]);
        if (out.empty() && Focus() < m_rows.size()) out.push_back(m_items[m_rows[Focus()]]);
        return out;
    }
    const std::vector<download::Item>& Items() const { return m_items; }

    // New state from the service. True when the rows changed (not only their progress).
    bool Update(std::vector<download::Item> all) {
        std::vector<download::Item> items;
        for (auto& item : all)
            if ((item.state == State::Done) == m_done) items.push_back(std::move(item));
        if (m_done) std::reverse(items.begin(), items.end()); // newest first
        const bool same = items.size() == m_items.size() &&
                          std::equal(items.begin(), items.end(), m_items.begin(),
                                     [](const auto& a, const auto& b) { return a.id == b.id; });
        m_items = std::move(items);
        if (!same) Refilter();
        return !same;
    }

  private:
    static std::vector<media::Column> Layout(bool done) {
        std::vector<media::Column> columns{media::ArtColumn(false), {L"Title", 0, true}, {L"Artist", 160},
                                           {L"Album", 160}, {L"Source", 110}};
        if (done) {
            columns.push_back({L"Format", 130});
            columns.push_back({L"Size", 80, false, true});
            columns.push_back({L"Date downloaded", 130});
            columns.push_back({L"Location", 220});
        } else {
            columns.push_back({L"Progress", 140, false, false, false, media::kProgressFormat});
            columns.push_back({L"Status", 150});
            columns.push_back({L"Size", 130, false, true});
            columns.push_back({L"Format", 130});
        }
        return columns;
    }

    size_t ItemCount() const override { return m_items.size(); }
    std::wstring ItemCell(size_t index, size_t column) const override {
        if (index >= m_items.size() || column >= m_fields.size()) return {};
        const download::Item& i = m_items[index];
        switch (m_fields[column]) {
        case kTitle: return i.title.empty() ? i.track : i.title;
        case kArtist: return i.artist;
        case kAlbum: return i.album;
        case kSource:
            if (const stream::Provider* provider = stream::FindProvider(i.provider)) return Tr(provider->Name());
            return i.provider;
        case kFormat: return i.format;
        case kWhen: return When(i.finished);
        case kWhere: return FolderOf(i.path);
        case kSize:
            if (m_done || !i.size) return i.size ? Bytes(i.size) : std::wstring();
            return Bytes(i.received) + L" / " + Bytes(i.size);
        case kProgress: {
            const int permille = i.size ? static_cast<int>(i.received * 1000 / i.size) : 0;
            const wchar_t state = i.state == State::Failed    ? L'e'
                                  : i.state == State::Paused  ? L'p'
                                  : i.state == State::Running ? L'r'
                                                              : L'n';
            return std::to_wstring(permille) + L' ' + state;
        }
        case kStatus:
            switch (i.state) {
            case State::Waiting: return Tr(L"Waiting");
            case State::Paused: return Tr(L"Paused");
            case State::Failed: return Tr(i.error.empty() ? std::wstring(L"Failed") : i.error);
            case State::Done: return Tr(L"Downloaded");
            case State::Running:
                if (!i.size) return Tr(L"Starting...");
                return std::to_wstring(i.received * 100 / i.size) + L"%    " + Speed(i.speed);
            }
            return {};
        }
        return {};
    }
    std::wstring FilterText(size_t index) const override {
        const auto& i = m_items[index];
        return i.title + L" " + i.artist + L" " + i.album;
    }
    std::wstring Identity(size_t index) const override { return std::to_wstring(m_items[index].id); }

    bool m_done;
    std::vector<Column> m_fields;
    std::vector<download::Item> m_items;
    std::function<void(POINT)> m_menu;
};

class DownloadsPage final : public Page {
  public:
    DownloadsPage(PageHost& host, View view) : Page(host, std::move(view)) {
        m_token = download::Subscribe(Guard([this] { OnChanged(); }));
    }
    ~DownloadsPage() override { download::Unsubscribe(m_token); }

    std::unique_ptr<media::ListSource> Start(bool) override {
        auto source = std::make_unique<DownloadsSource>(Done(), download::Items(),
                                                        Guard([this](POINT at) { Menu(at); }));
        m_list = source.get();
        m_counts = Counts();
        return source;
    }

    std::vector<PageTab> Tabs() const override {
        const auto [running, done] = m_counts;
        const auto label = [](const wchar_t* text, size_t count) {
            return count ? Tr(text) + L" (" + std::to_wstring(count) + L")" : Tr(text);
        };
        return {{L"", label(L"Downloading", running)}, {kDone, label(L"Downloaded", done)}};
    }
    std::wstring Tab() const override { return m_view.sectionId; }
    void SelectTab(const std::wstring& key) override {
        if (key == m_view.sectionId) return;
        View v = m_view;
        v.sectionId = key;
        m_host.Open(v);
    }

    // The command bar's page command: Pause all / Start all while downloading, the folder after.
    DetailInfo Detail() const override {
        DetailInfo d;
        if (Done())
            d.saveLabel = Tr(L"Open download folder");
        else if (m_list && !m_list->Items().empty())
            d.saveLabel = Tr(AnyActive() ? L"Pause all" : L"Start all");
        return d;
    }
    void SaveAction() override {
        if (Done()) {
            OpenFolder(download::Folder());
            return;
        }
        if (!m_list) return;
        std::vector<uint64_t> ids;
        for (const auto& item : m_list->Items())
            ids.push_back(item.id);
        if (AnyActive())
            download::Pause(ids);
        else
            download::Resume(ids);
    }

    // The downloaded files' commands, after the song menu.
    void AddSongActions(std::vector<MenuAction>& actions, const metadb_handle_list&) override {
        if (!Done() || !m_list) return;
        const auto selected = m_list->Selected();
        if (selected.empty()) return;
        std::vector<uint64_t> ids;
        for (const auto& item : selected)
            ids.push_back(item.id);
        if (selected.size() == 1)
            actions.push_back({L"Open file location", [path = selected.front().path] { Reveal(path); }});
        actions.push_back({L"Remove from list", [ids] { download::Forget(ids, false); }});
        actions.push_back({L"Delete file", [ids, owner = m_host.Owner(), count = selected.size()] {
                               const std::wstring text = count == 1
                                                             ? Tr(L"Move the downloaded file to the Recycle Bin?")
                                                             : Tr(L"Move the downloaded files to the Recycle Bin?");
                               if (MessageBoxW(owner, text.c_str(), Tr(L"Delete file").c_str(),
                                               MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                                   download::Forget(ids, true);
                           }});
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        actions.push_back({L"Open download folder", [] { OpenFolder(download::Folder()); }});
        if (!Done() && m_list && !m_list->Items().empty()) {
            actions.push_back({L"Pause all", [this] { SaveAllIf(true); }});
            actions.push_back({L"Start all", [this] { SaveAllIf(false); }});
        }
    }

  private:
    bool Done() const { return m_view.sectionId == kDone; }

    std::pair<size_t, size_t> Counts() const {
        size_t running = 0, done = 0;
        for (const auto& item : download::Items())
            ++(item.state == State::Done ? done : running);
        return {running, done};
    }

    bool AnyActive() const {
        if (!m_list) return false;
        for (const auto& item : m_list->Items())
            if (item.state == State::Waiting || item.state == State::Running) return true;
        return false;
    }

    void SaveAllIf(bool pause) {
        if (!m_list) return;
        std::vector<uint64_t> ids;
        for (const auto& item : m_list->Items())
            ids.push_back(item.id);
        if (pause)
            download::Pause(ids);
        else
            download::Resume(ids);
    }

    void OnChanged() {
        if (!m_list || m_host.Source() != m_list) return;
        const bool active = AnyActive();
        m_host.BeforeListChange();
        m_list->Update(download::Items());
        m_host.ListChanged();
        const auto counts = Counts();
        if (counts != m_counts || active != AnyActive()) {
            m_counts = counts;
            m_host.ChromeChanged(); // the tab counts, Pause all / Start all
        }
    }

    void Menu(POINT at) {
        if (!m_list) return;
        const auto selected = m_list->Selected();
        if (selected.empty()) return;
        std::vector<uint64_t> ids;
        for (const auto& item : selected)
            ids.push_back(item.id);
        std::vector<MenuAction> actions;
        {
            bool canPause = false, canResume = false;
            for (const auto& item : selected) {
                canPause = canPause || item.state == State::Waiting || item.state == State::Running;
                canResume = canResume || item.state == State::Paused || item.state == State::Failed;
            }
            if (canResume)
                actions.push_back({selected.size() == 1 && selected.front().state == State::Failed ? L"Retry" : L"Start",
                                   [ids] { download::Resume(ids); }});
            if (canPause) actions.push_back({L"Pause", [ids] { download::Pause(ids); }});
            actions.push_back({L"Cancel download", [ids] { download::Cancel(ids); }});
            if (selected.size() == 1 && !selected.front().path.empty())
                actions.push_back({L"Open file location", [path = selected.front().path] {
                                       OpenFolder(FolderOf(path));
                                   }});
        }
        ShowActionMenu(m_host.Owner(), at, actions);
    }

    DownloadsSource* m_list = nullptr;
    uint64_t m_token = 0;
    std::pair<size_t, size_t> m_counts{};
};

} // namespace

std::unique_ptr<Page> MakeDownloadsPage(PageHost& host, const View& view) {
    return std::make_unique<DownloadsPage>(host, view);
}

void AddDownloadActions(std::vector<MenuAction>& actions, const metadb_handle_list& items, HWND owner) {
    if (!download::AnyDownloadable(items)) return;
    bool anyNew = false; // not downloaded nor downloading yet
    for (size_t i = 0; i < items.get_count() && !anyNew; ++i)
        anyNew = download::Downloadable(items[i]) && !download::StateOf(items[i]);
    if (anyNew) actions.push_back({L"Download", [items, owner] { Queue(owner, items, {}); }});
    if (items.get_count() == 1) {
        actions.push_back({L"Save as...", [handle = items[0], owner] {
                               if (const std::wstring path = PickFile(owner, handle); !path.empty())
                                   download::AddAs(handle, path);
                           }});
    } else {
        actions.push_back({L"Download to...", [items, owner] {
                               if (const std::wstring folder = PickDownloadFolder(owner); !folder.empty())
                                   Queue(owner, items, folder);
                           }});
    }
}

void AddBulkDownloadActions(std::vector<MenuAction>& actions, SongsFetch songs, HWND owner) {
    actions.push_back({L"Download", [songs, owner] { DownloadSongs(songs, owner); }});
    actions.push_back({L"Download to...", [songs, owner] {
                           const std::wstring folder = PickDownloadFolder(owner);
                           if (folder.empty()) return;
                           songs([owner, folder](metadb_handle_list items) { Queue(owner, items, folder); });
                       }});
}

void DownloadSongs(SongsFetch songs, HWND owner) {
    songs([owner](metadb_handle_list items) { Queue(owner, items, {}); });
}

} // namespace glasswave::ui
