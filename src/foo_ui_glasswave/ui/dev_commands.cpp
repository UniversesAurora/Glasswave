#include "pch.h"
#include "netease/log.h"
#include "stream/provider.h"
#include "ui/main_window.h"
#include "ui/remote_pages.h"
#include "ui/text.h"

// Developer switch for checking pages with a service's real data without driving the UI:
//
//   foobar2000.exe /glasswave:open:<source>:<artist|album|playlist>:<id>
//
// opens that page of a source ("apple", "ncm") in the running window. The details band of the
// page on screen is written to the log as it changes (MainWindow::LogDetail), so what the page
// shows can be read back. From Git Bash, run it with MSYS_NO_PATHCONV=1.

namespace glasswave::ui {
namespace {

class OpenPageCommand : public commandline_handler {
  public:
    result on_token(const char* token) override {
        static constexpr std::string_view kPrefix = "/glasswave:open:";
        const std::string_view text = token ? token : "";
        if (!text.starts_with(kPrefix)) return RESULT_NOT_OURS;
        std::vector<std::wstring> parts;
        std::wstring rest = Wide(std::string(text.substr(kPrefix.size())).c_str());
        for (int i = 0; i < 2; ++i) {
            const size_t colon = rest.find(L':');
            if (colon == std::wstring::npos) break;
            parts.push_back(rest.substr(0, colon));
            rest.erase(0, colon + 1);
        }
        parts.push_back(rest); // the id keeps any further colons
        auto* window = MainWindow::Instance();
        stream::Provider* provider = parts.size() == 3 ? stream::FindProvider(parts[0]) : nullptr;
        if (!window || !provider || parts[2].empty()) {
            netease::Log(L"dev: cannot open " + Wide(token));
            return RESULT_PROCESSED;
        }
        const std::wstring& kind = parts[1];
        const std::wstring& id = parts[2];
        if (kind == L"artist")
            window->Open(ArtistView(*provider, id, {}));
        else if (kind == L"album")
            window->Open(AlbumView(*provider, id, {}));
        else if (kind == L"playlist")
            window->Open(PlaylistView(*provider, id, {}, std::nullopt));
        else
            netease::Log(L"dev: unknown page kind " + kind);
        return RESULT_PROCESSED;
    }
};

commandline_handler_factory_t<OpenPageCommand> g_openPage;

} // namespace
} // namespace glasswave::ui
