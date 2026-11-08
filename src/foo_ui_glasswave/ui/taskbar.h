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
    // Favorite button: a gold star when the playing song is in Favorites, else its outline.
    void SetFavorite(bool favorite);
    std::function<void()> onFavorite;

    // Jump list tasks. Written only when this process has no explicit AppUserModelID, i.e.
    // the jump list belongs to this foobar2000 installation alone.
    static void SetupJumpList();

private:
    void AddButtons();
    void UpdateButtons();

    HWND m_hwnd;
    UINT m_createdMsg = 0;
    winrt::com_ptr<ITaskbarList3> m_list;
    HIMAGELIST m_images = nullptr;
    bool m_added = false, m_playing = false, m_paused = false, m_favorite = false;
    int m_starFull = -1, m_starOutline = -1;  // image list indices
};

}  // namespace glasswave::ui
