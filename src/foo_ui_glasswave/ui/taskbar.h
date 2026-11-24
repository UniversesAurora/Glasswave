#pragma once
#include <shobjidl.h>

namespace glasswave::ui {

// Windows taskbar integration, like WMP 12:
//  - thumbnail toolbar under the taskbar preview: previous / play-pause / next, with WMP's
//    own glyphs (wmploc bitmaps 355-357) when the user's files are present;
//  - jump list tasks in the taskbar button's menu (play / pause, next, previous, stop),
//    running foobar2000's own command-line switches.
class Taskbar {
public:
    explicit Taskbar(HWND hwnd);
    ~Taskbar();

    // Feed every window message; true when it was a taskbar message we handled.
    bool OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void SetPlayState(bool playing, bool paused);
    void RefreshButtons();
    // Favorite button: a gold star when the playing song is in Favorites, else its outline.
    void SetFavorite(bool favorite);
    std::function<void()> onFavorite;

    // Taskbar thumbnail: the playing song's front cover, as WMP 12 shows it. An invalid
    // track, or one without a cover, gives the ordinary window thumbnail back.
    void SetCoverTrack(const metadb_handle_ptr& track);

    // Jump list tasks. Attached to Glasswave's own AppUserModelID when Win7 icon resources are
    // present, else written only when the process has no explicit AppUserModelID.
    static void SetupJumpList();

private:
    void ApplyWindowIdentity();
    void ClearWindowIdentity();
    void AddButtons();
    void UpdateButtons();
    void UseCover(bool on);
    void SendCoverThumbnail(int maxWidth, int maxHeight);
    void SendLivePreview();

    HWND m_hwnd;
    metadb_handle_ptr m_coverTrack;
    std::wstring m_coverKey;
    int m_coverSize = 0;              // the last size DWM asked for, in pixels
    uint64_t m_coverVersion = 0;      // ArtCache version of the picture last sent
    int m_artToken = -1;
    bool m_coverOn = false;
    UINT m_createdMsg = 0;
    winrt::com_ptr<ITaskbarList3> m_list;
    HIMAGELIST m_images = nullptr;
    bool m_added = false, m_playing = false, m_paused = false, m_favorite = false;
    bool m_identitySet = false;
    int m_starFull = -1, m_starOutline = -1;  // image list indices
};

}  // namespace glasswave::ui
