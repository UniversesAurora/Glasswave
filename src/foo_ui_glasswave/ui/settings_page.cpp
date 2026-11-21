#include "pch.h"
#include "aero/aero_colorization.h"
#include "app/setting_fields.h"
#include "app/settings.h"
#include "cache/config.h"
#include "cache/store.h"
#include "cd/discs.h"
#include "media/play_stats.h"
#include "lyrics/scripts.h"
#include "stream/provider.h"
#include "stream/list_session.h"
#include "download/downloads.h"
#include "ui/downloads_page.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"
#include "ui/main_window.h"

#include <commctrl.h>

// Preferences > Display > Glasswave: Glasswave's own settings and one section per streaming
// provider, built from settings::Field rows. Plain Win32 controls laid out in code; the page
// scrolls when it is taller than the preferences dialog.
namespace glasswave::ui {

namespace {

// {4F2B7C61-8E3A-4D95-B1C7-2A6E9D0F3B58}
constexpr GUID kPageGuid = {0x4f2b7c61, 0x8e3a, 0x4d95, {0xb1, 0xc7, 0x2a, 0x6e, 0x9d, 0x0f, 0x3b, 0x58}};
constexpr wchar_t kClass[] = L"GlasswavePreferencesPage";

// Layout in DIP.
constexpr int kMargin = 12, kTitleGap = 14, kRowH = 26, kLabelW = 230, kControlW = 220, kNumberW = 80;

using settings::Field;
using settings::Section;

Field Number(std::wstring label, std::wstring unit, const settings::Range& range, int (*get)(), void (*set)(int),
             bool restart = false) {
    Field f;
    f.label = std::move(label);
    f.unit = std::move(unit);
    f.min = range.min;
    f.max = range.max;
    f.defaultValue = range.defaultValue;
    f.restart = restart;
    f.get = get;
    f.set = set;
    return f;
}

Field Button(std::wstring label, std::function<void(HWND)> press) {
    Field f;
    f.kind = Field::Kind::Button;
    f.label = std::move(label);
    f.press = std::move(press);
    return f;
}

std::vector<Section> BuildSections() {
    std::vector<Section> sections;
    Field language;
    language.kind = Field::Kind::Choice;
    language.label = L"Language";
    language.choices = {L"Follow Windows", L"English", L"中文 (Chinese)"};  // Language values in order
    language.get = [] { return static_cast<int>(CurrentLanguageSetting()); };
    language.set = [](int value) { SetLanguageSetting(static_cast<Language>(value)); };
    sections.push_back({L"General",
                        {std::move(language), Number(L"Volume bubble delay", L"ms", settings::kVolumeTipDelay,
                                                     settings::VolumeTipDelayMs, settings::SetVolumeTipDelayMs)}});
    sections.push_back(
        {L"Covers",
         {Number(L"Covers kept in memory", L"covers", settings::kThumbnailCache, settings::ThumbnailCacheEntries,
                 settings::SetThumbnailCacheEntries),
          Number(L"Covers kept per list", L"covers", settings::kListBitmaps, settings::ListBitmapCache,
                 settings::SetListBitmapCache),
          Number(L"Cover loading threads", L"", settings::kCoverThreads, settings::CoverThreads,
                 settings::SetCoverThreads, true),
          Button(L"Reload all covers", [](HWND) { ArtCache::Get().Invalidate(L""); })}});
    sections.push_back(
        {L"Shared cache",
         {Number(L"Song disk cache", L"MiB",
                 {cache::kAudioMinMiB, cache::kAudioMaxMiB, cache::kAudioDefaultMiB},
                 cache::AudioLimitMiB, cache::SetAudioLimitMiB),
          Number(L"Image disk cache", L"MiB",
                 {cache::kImagesMinMiB, cache::kImagesMaxMiB, cache::kImagesDefaultMiB},
                 cache::ImageLimitMiB, cache::SetImageLimitMiB),
          Number(L"List disk cache", L"MiB",
                 {cache::kListsMinMiB, cache::kListsMaxMiB, cache::kListsDefaultMiB},
                 cache::ListLimitMiB, cache::SetListLimitMiB),
          Button(L"Clear song cache", [](HWND) {
              try { cache::GetStore().Clear(cache::Kind::Audio); } catch (...) {}
          }),
          Button(L"Clear image cache", [](HWND) {
              try { cache::GetStore().Clear(cache::Kind::Image); } catch (...) {}
              ArtCache::Get().Invalidate(L"");
          }),
          Button(L"Clear list cache", [](HWND) {
              for (auto* provider : stream::Providers())
                  stream::InvalidateListCache(provider->Key(), true,
                                              stream::ListCacheInvalidation::PurgePersistent);
              try { cache::GetStore().Clear(cache::Kind::Data); } catch (...) {}
          })}});
    sections.push_back(
        {L"Play history",
         {Number(L"Songs remembered", L"songs", settings::kPlayHistory, settings::PlayHistoryLimit,
                 settings::SetPlayHistoryLimit),
          Number(L"Songs shown in Recently Played and Most Played", L"songs", settings::kPlayHistoryRows,
                 settings::PlayHistoryRows, settings::SetPlayHistoryRows),
          Button(L"Clear play history...", [](HWND owner) {
              if (MessageBoxW(owner, Tr(L"Clear Recently Played and Most Played? This cannot be undone.").c_str(),
                              Tr(L"Play history").c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                  return;
              media::PlayStats::Get().ForgetAllRecent();
              media::PlayStats::Get().ClearAllCounts();
          })}});
    {
        Field autoLoad;
        autoLoad.kind = Field::Kind::Choice;
        autoLoad.label = L"When an audio CD is inserted";
        autoLoad.choices = {L"Read it at once", L"Read it when opened"};
        autoLoad.get = [] { return settings::AutoLoadCds() ? 0 : 1; };
        autoLoad.set = [](int value) {
            settings::SetAutoLoadCds(value == 0);
            if (value == 0) cd::ReadUnreadDrives(); // discs already in a drive load now
        };
        sections.push_back({L"Audio CDs", {std::move(autoLoad)}});
    }
    {
        Field tags;
        tags.kind = Field::Kind::Choice;
        tags.label = L"Song tags (such as Original) before the artist";
        tags.choices = {L"Show", L"Hide"};
        tags.get = [] { return settings::SongTagsShown() ? 0 : 1; };
        tags.set = [](int value) { settings::SetSongTagsShown(value == 0); };
        sections.push_back({L"Song lists", {std::move(tags)}});
    }
    {
        // How the lyrics look, then where they come from.
        std::vector<Field> fields{Number(L"Lyrics text size", L"%", settings::kLyricSize, settings::LyricSizePercent,
                                         settings::SetLyricSizePercent),
                                  Number(L"Translation text size", L"%", settings::kTranslationSize,
                                         settings::TranslationSizePercent, settings::SetTranslationSizePercent)};
        Field fonts;
        fonts.kind = Field::Kind::Text;
        fonts.label = L"Lyrics fonts, in order (comma-separated)";
        fonts.getText = [] { return settings::LyricFonts(); };
        fonts.setText = [](const std::wstring& text) { settings::SetLyricFonts(text); };
        fields.push_back(std::move(fonts));
        Field swap;
        swap.kind = Field::Kind::Choice;
        swap.label = L"Original and translation";
        swap.choices = {L"Original as the lyric, translation under it", L"Translation as the lyric, original under it"};
        swap.get = [] { return settings::SwapTranslation() ? 1 : 0; };
        swap.set = [](int value) {
            if ((value == 1) == settings::SwapTranslation()) return;
            settings::SetSwapTranslation(value == 1);
            if (auto* window = MainWindow::Instance()) window->ReapplyLyrics();
        };
        fields.push_back(std::move(swap));
        for (auto& field : lyrics::scripts::SettingFields()) fields.push_back(std::move(field));
        sections.push_back({L"Lyrics", std::move(fields)});
    }
    {
        // Where downloads go and how they are named (NetEase's 下载目录, 音乐命名格式 and
        // 文件智能分类); each source sets its own download quality below.
        Field folder;
        folder.kind = Field::Kind::Note;
        folder.label = L"Download folder";
        folder.note = [] { return download::Folder(); };
        Field naming;
        naming.kind = Field::Kind::Choice;
        naming.label = L"File names";
        naming.choices = {L"Title", L"Artist - Title", L"Title - Artist"}; // download::Naming in order
        naming.defaultValue = static_cast<int>(download::Naming::ArtistTitle);
        naming.get = [] { return static_cast<int>(download::GetNaming()); };
        naming.set = [](int value) { download::SetNaming(static_cast<download::Naming>(value)); };
        Field folders;
        folders.kind = Field::Kind::Choice;
        folders.label = L"Sort into folders";
        folders.choices = {L"No folders", L"By artist", L"By artist and album"}; // download::Folders in order
        folders.get = [] { return static_cast<int>(download::GetFolders()); };
        folders.set = [](int value) { download::SetFolders(static_cast<download::Folders>(value)); };
        Field lyricsMode;
        lyricsMode.kind = Field::Kind::Choice;
        lyricsMode.label = L"Lyrics in downloaded files";
        lyricsMode.choices = {L"Don't save", L"Embed in the file", L"Save a .lrc file beside it",
                              L"Embed and save a .lrc file"}; // download::LyricsMode in order
        lyricsMode.defaultValue = static_cast<int>(download::LyricsMode::Embed);
        lyricsMode.get = [] { return static_cast<int>(download::GetLyricsMode()); };
        lyricsMode.set = [](int value) { download::SetLyricsMode(static_cast<download::LyricsMode>(value)); };
        Field engine;
        engine.kind = Field::Kind::Note;
        engine.label = L"Download engine";
        engine.note = [] { return download::Engine(); };
        sections.push_back({L"Downloads",
                            {std::move(folder), Button(L"Change folder...", [](HWND owner) {
                                 if (const auto chosen = PickDownloadFolder(owner); !chosen.empty())
                                     download::SetFolder(chosen);
                             }),
                             Button(L"Open download folder", [](HWND) {
                                 const std::wstring dir = download::Folder();
                                 CreateDirectoryW(dir.c_str(), nullptr);
                                 ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                             }),
                             std::move(naming), std::move(folders), std::move(lyricsMode),
                             Number(L"Songs downloaded at once", L"songs", download::kSongsAtOnce,
                                    download::SongsAtOnce, download::SetSongsAtOnce),
                             Number(L"Connections per song", L"", download::kConnections, download::Connections,
                                    download::SetConnections),
                             engine}});
    }
    for (auto* provider : stream::Providers())
        if (auto fields = provider->SettingFields(); !fields.empty()) sections.push_back({provider->Name(), std::move(fields)});
    return sections;
}

class SettingsPage : public preferences_page_instance {
public:
    SettingsPage(HWND parent, preferences_page_callback::ptr callback)
        : m_callback(std::move(callback)), m_sections(BuildSections()) {
        RegisterClassOnce();
        RECT rc{};
        GetClientRect(parent, &rc);
        m_hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0,
                                 rc.right, rc.bottom, parent, nullptr, core_api::get_my_instance(), this);
    }

    ~SettingsPage() {
        if (m_hwnd) DestroyWindow(m_hwnd);
    }

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable;
        if (m_changed) state |= preferences_state::changed;
        if (m_restart) state |= preferences_state::needs_restart;
        return state;
    }

    fb2k::hwnd_t get_wnd() override { return m_hwnd; }

    void apply() override {
        for (const auto& row : m_rows) {
            const Field& f = *row.field;
            switch (f.kind) {
            case Field::Kind::Number:
                if (const int value = std::clamp(ReadNumber(row), f.min, f.max); value != f.get()) f.set(value);
                break;
            case Field::Kind::Choice: {
                const int value = static_cast<int>(SendMessageW(row.control, CB_GETCURSEL, 0, 0));
                if (value >= 0 && value != f.get()) f.set(value);
                break;
            }
            case Field::Kind::Text:
                if (const std::wstring text = ReadText(row.control); text != f.getText()) f.setText(text);
                break;
            default: break;
            }
        }
        m_changed = m_restart = false;
        Load(false);  // shows clamped values
        NotifyWindow();
        m_callback->on_state_changed();
    }

    void reset() override {
        Load(true);
        m_changed = true;
        m_callback->on_state_changed();
    }

private:
    struct Row {
        const Field* field = nullptr;
        HWND label = nullptr, control = nullptr, unit = nullptr;
    };

    static void RegisterClassOnce() {
        WNDCLASSEXW wc{sizeof(wc)};
        if (GetClassInfoExW(core_api::get_my_instance(), kClass, &wc)) return;
        wc.lpfnWndProc = Proc;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<SettingsPage*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_hwnd = hwnd;
        }
        auto* self = reinterpret_cast<SettingsPage*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_hwnd = nullptr;
            if (self->m_bold) DeleteObject(self->m_bold);
            self->m_bold = nullptr;
        }
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            Build();
            Load(false);
            return 0;
        case WM_SIZE: UpdateScrollRange(); return 0;
        case WM_VSCROLL: OnScroll(LOWORD(wp)); return 0;
        case WM_MOUSEWHEEL: ScrollTo(m_scroll - GET_WHEEL_DELTA_WPARAM(wp) * Px(kRowH) * 3 / WHEEL_DELTA); return 0;
        case WM_CTLCOLORSTATIC:  // labels on the page's own background
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_COMMAND: OnCommand(HIWORD(wp), reinterpret_cast<HWND>(lp)); return 0;
        default: return DefWindowProcW(m_hwnd, msg, wp, lp);
        }
    }

    int Px(int dip) const { return MulDiv(dip, static_cast<int>(GetDpiForWindow(m_hwnd)), 96); }

    HWND Make(const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, HFONT font) {
        HWND child = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, w, h, m_hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(++m_nextId)), core_api::get_my_instance(),
                                     nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        return child;
    }

    void Build() {
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(GetParent(m_hwnd), WM_GETFONT, 0, 0));
        if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        LOGFONTW lf{};
        GetObjectW(font, sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        m_bold = CreateFontIndirectW(&lf);

        const int x = Px(kMargin), labelW = Px(kLabelW), controlX = x + labelW + Px(8), rowH = Px(kRowH);
        const int controlH = Px(22), textY = Px(4);
        int y = Px(kMargin);
        for (const auto& section : m_sections) {
            Make(L"STATIC", Tr(section.title), SS_LEFT, x, y, Px(kLabelW + kControlW), Px(18), m_bold);
            y += Px(18) + Px(kTitleGap) / 2;
            for (const auto& field : section.fields) {
                Row row;
                row.field = &field;
                switch (field.kind) {
                case Field::Kind::Number: {
                    row.label = Make(L"STATIC", Tr(field.label), SS_LEFT, x, y + textY, labelW, controlH, font);
                    row.control = Make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
                                       controlX, y, Px(kNumberW), controlH, font);
                    HWND spin = Make(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                     0, 0, 0, 0, font);
                    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(row.control), 0);
                    SendMessageW(spin, UDM_SETRANGE32, static_cast<WPARAM>(field.min), static_cast<LPARAM>(field.max));
                    std::wstring unit = field.unit.empty() ? std::wstring() : Tr(field.unit);
                    if (field.restart) unit += (unit.empty() ? L"" : L"  ") + Tr(L"(after a restart)");
                    if (!unit.empty())
                        row.unit = Make(L"STATIC", unit, SS_LEFT, controlX + Px(kNumberW + 8), y + textY,
                                        Px(kControlW), controlH, font);
                    break;
                }
                case Field::Kind::Choice: {
                    row.label = Make(L"STATIC", Tr(field.label), SS_LEFT, x, y + textY, labelW, controlH, font);
                    row.control = Make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, controlX, y,
                                       Px(kControlW), Px(200), font);
                    for (const auto& choice : field.choices)
                        SendMessageW(row.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(choice).c_str()));
                    break;
                }
                case Field::Kind::Text: {
                    std::wstring label = Tr(field.label);
                    if (field.restart) label += L" " + Tr(L"(after a restart)");
                    row.label = Make(L"STATIC", label, SS_LEFT, x, y + textY, labelW, controlH, font);
                    row.control = Make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, controlX, y,
                                       Px(kControlW), controlH, font);
                    break;
                }
                case Field::Kind::Button:
                    row.control = Make(L"BUTTON", Tr(field.label), WS_TABSTOP | BS_PUSHBUTTON, controlX, y,
                                       Px(kControlW), controlH, font);
                    break;
                case Field::Kind::Note:
                    row.label = Make(L"STATIC", Tr(field.label), SS_LEFT, x, y + textY, labelW, controlH, font);
                    row.control = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, controlX, y + textY, Px(kControlW + 80),
                                       controlH, font);
                    break;
                }
                m_rows.push_back(row);
                y += rowH;
            }
            y += Px(kTitleGap);
        }
        m_contentHeight = y;
        UpdateScrollRange();
    }

    // Fills the controls from the stored settings, or with the defaults (Reset).
    void Load(bool defaults) {
        m_loading = true;
        for (const auto& row : m_rows) {
            const Field& f = *row.field;
            switch (f.kind) {
            case Field::Kind::Number:
                SetWindowTextW(row.control, std::to_wstring(defaults ? f.defaultValue : f.get()).c_str());
                break;
            case Field::Kind::Choice:
                SendMessageW(row.control, CB_SETCURSEL, static_cast<WPARAM>(defaults ? f.defaultValue : f.get()), 0);
                break;
            case Field::Kind::Text:
                SetWindowTextW(row.control, (defaults ? f.defaultText : f.getText()).c_str());
                break;
            default: break;
            }
        }
        m_loading = false;
        RefreshNotes();
    }

    void RefreshNotes() {
        for (const auto& row : m_rows)
            if (row.field->kind == Field::Kind::Note) SetWindowTextW(row.control, row.field->note().c_str());
    }

    static std::wstring ReadText(HWND control) {
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(control, text.data(), static_cast<int>(text.size()))));
        return text;
    }

    static int ReadNumber(const Row& row) {
        const std::wstring text = ReadText(row.control);
        return text.empty() ? row.field->defaultValue : _wtoi(text.c_str());
    }

    void OnCommand(UINT code, HWND control) {
        const auto it = std::find_if(m_rows.begin(), m_rows.end(), [control](const Row& r) { return r.control == control; });
        if (it == m_rows.end()) return;
        const Field& f = *it->field;
        if (f.kind == Field::Kind::Button && code == BN_CLICKED) {
            service_ptr_t<SettingsPage> keep = this;  // a modal box may close the dialog around us
            f.press(m_hwnd);
            if (!m_hwnd) return;
            RefreshNotes();
            NotifyWindow();
            return;
        }
        const bool edited = (f.kind == Field::Kind::Choice && code == CBN_SELCHANGE) ||
                            ((f.kind == Field::Kind::Number || f.kind == Field::Kind::Text) && code == EN_CHANGE);
        if (!edited || m_loading) return;
        m_changed = true;
        if (f.restart) m_restart = true;
        m_callback->on_state_changed();
    }

    // The window re-reads what it shows (language, covers, history pages).
    static void NotifyWindow() {
        if (HWND main = core_api::get_main_window()) PostMessageW(main, settings::kChangedMessage, 0, 0);
    }

    void UpdateScrollRange() {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL};
        si.nMin = 0;
        si.nMax = m_contentHeight;
        si.nPage = static_cast<UINT>(std::max<LONG>(rc.bottom, 0));
        SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
        ScrollTo(m_scroll);
    }

    void OnScroll(WORD code) {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(m_hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (code) {
        case SB_LINEUP: pos -= Px(kRowH); break;
        case SB_LINEDOWN: pos += Px(kRowH); break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: return;
        }
        ScrollTo(pos);
    }

    void ScrollTo(int pos) {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        pos = std::clamp(pos, 0, std::max(0, m_contentHeight - static_cast<int>(rc.bottom)));
        if (pos == m_scroll) return;
        ScrollWindowEx(m_hwnd, 0, m_scroll - pos, nullptr, nullptr, nullptr, nullptr,
                       SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        m_scroll = pos;
        SetScrollPos(m_hwnd, SB_VERT, pos, TRUE);
    }

    HWND m_hwnd = nullptr;
    preferences_page_callback::ptr m_callback;
    std::vector<Section> m_sections;  // the rows point into these
    std::vector<Row> m_rows;
    HFONT m_bold = nullptr;
    int m_nextId = 1000;
    int m_contentHeight = 0;
    int m_scroll = 0;
    bool m_loading = false;
    bool m_changed = false;
    bool m_restart = false;
};

class SettingsPageFactory : public preferences_page_v3 {
public:
    const char* get_name() override { return "Glasswave"; }
    GUID get_guid() override { return kPageGuid; }
    GUID get_parent_guid() override { return preferences_page::guid_display; }
    preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override {
        return fb2k::service_new<SettingsPage>(parent, callback);
    }
};

preferences_page_factory_t<SettingsPageFactory> g_settingsPage;

}  // namespace

}  // namespace glasswave::ui
