#include "pch.h"
#include "lyrics/scripts.h"
#include "lyrics/script_host.h"
#include "netease/http.h"
#include "stream/provider.h"
#include "app/settings.h"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace glasswave::lyrics::scripts {
namespace {

cfg_var_modern::cfg_string cfg_disabled(
    {0xded15511, 0x49c7, 0x41a9, {0x90, 0x5e, 0x83, 0xe1, 0x06, 0x64, 0x58, 0x22}}, "");
cfg_var_modern::cfg_string cfg_order(
    {0xded15512, 0x49c7, 0x41a9, {0x90, 0x5e, 0x83, 0xe1, 0x06, 0x64, 0x58, 0x22}}, "");

std::wstring Wide(std::string_view text) {
    return pfc::stringcvt::string_wide_from_utf8(std::string(text).c_str()).get_ptr();
}
std::string Utf8(std::wstring_view text) {
    return pfc::stringcvt::string_utf8_from_wide(std::wstring(text).c_str()).get_ptr();
}

std::vector<std::filesystem::path> Roots() {
    const std::filesystem::path own(Service::CacheFolder());
    return {own.parent_path().parent_path() / L"eslyric-data" / L"scripts", own / L"scripts"};
}

struct Catalog {
    std::vector<ScriptConfig> searchers, parsers;
    std::vector<std::filesystem::path> roots;
};

std::thread g_loader;
std::atomic<bool> g_stopping = false;
bool g_started = false, g_loading = false, g_loaded = false;
std::shared_ptr<int> g_alive;
std::mutex g_dataMutex;

std::string DefaultOrder(std::wstring_view provider) {
    if (provider == L"ncm")
        return "netease_ex.js,netease.js,qqmusic_ex.js,kugou.js,qqmusic.js,musixmatch.js,lrclib.js";
    return "qqmusic_ex.js,netease_ex.js,kugou.js,netease.js,qqmusic.js,musixmatch.js,lrclib.js";
}

std::map<std::string, std::string> Orders() {
    std::map<std::string, std::string> result;
    std::istringstream lines(cfg_order.get().c_str());
    std::string line;
    while (std::getline(lines, line))
        if (const size_t equal = line.find('='); equal != std::string::npos)
            result[line.substr(0, equal)] = line.substr(equal + 1);
    return result;
}

std::string Order(std::wstring_view provider) {
    const auto orders = Orders();
    const auto found = orders.find(Utf8(provider));
    return found == orders.end() ? DefaultOrder(provider) : found->second;
}

bool Enabled(std::wstring_view key) {
    return std::string(cfg_disabled.get().c_str()).find("\n" + Utf8(key) + "\n") == std::string::npos;
}

std::string ReadData(const std::filesystem::path& path) {
    std::lock_guard lock(g_dataMutex);
    std::ifstream file(path, std::ios::binary);
    return file ? std::string((std::istreambuf_iterator<char>(file)), {}) : std::string();
}

void WriteData(const std::filesystem::path& path, const std::string& text) {
    std::lock_guard lock(g_dataMutex);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto part = path;
    part += L".part";
    {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) return;
    }
    MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::filesystem::path DataFile(const std::string& script, const std::string& key) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : key) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return std::filesystem::path(Service::CacheFolder()) / L"script-data" / Wide(script) /
           (std::to_wstring(hash) + L".txt");
}

std::wstring Clean(std::wstring text) {
    std::wstring result;
    int depth = 0;
    for (wchar_t ch : text) {
        if (ch == L'(' || ch == L'[' || ch == L'（' || ch == L'【') {
            ++depth;
            continue;
        }
        if (ch == L')' || ch == L']' || ch == L'）' || ch == L'】') {
            depth = std::max(0, depth - 1);
            continue;
        }
        if (!depth) result += ch;
    }
    if (const size_t feat = result.find(L" feat."); feat != std::wstring::npos) result.resize(feat);
    while (!result.empty() && iswspace(result.back()))
        result.pop_back();
    return result.empty() ? text : result;
}

class ScriptSearcher : public Searcher {
  public:
    ScriptSearcher(ScriptConfig script, std::shared_ptr<const Catalog> catalog)
        : m_script(std::move(script)), m_catalog(std::move(catalog)) {}
    std::wstring Name() const override { return Wide(m_script.name); }
    std::wstring Key() const override { return m_script.path.filename().wstring(); }

    void Search(const Query& query, abort_callback& abort,
                const std::function<void(Candidate)>& found) override {
        ScriptEnvironment env;
        env.roots = m_catalog->roots;
        env.parsers = m_catalog->parsers;
        env.aborted = [&] { return abort.is_aborting() || g_stopping; };
        const std::string key = Utf8(Key());
        env.log = [key](const std::string& text) {
            console::printf("Glasswave lyrics [%s]: %s", key.c_str(), text.c_str());
        };
        env.getData = [key](const std::string& name) { return ReadData(DataFile(key, name)); };
        env.setData = [key](const std::string& name, const std::string& value) {
            WriteData(DataFile(key, name), value);
        };
        env.request = [&abort](const ScriptRequest& request) {
            netease::HttpOptions options;
            options.method = Wide(request.method);
            std::transform(options.method.begin(), options.method.end(), options.method.begin(), towupper);
            options.timeoutMs = request.timeoutMs;
            options.body = request.body;
            options.maxBodyBytes = 8 * 1024 * 1024;
            for (const auto& [name, value] : request.headers)
                options.headers.emplace_back(Wide(name), Wide(value));
            const auto reply = netease::SendHttp(Wide(request.url), options, abort);
            ScriptResponse result;
            result.status = static_cast<int>(reply.status);
            result.body = reply.body;
            result.error = Utf8(reply.error);
            result.message = Utf8(reply.statusMessage);
            for (const auto& [name, value] : reply.headers)
                result.headers[Utf8(name)] = Utf8(value);
            return result;
        };
        env.found = [&](ScriptLyric lyric) {
            if (abort.is_aborting()) return;
            Candidate candidate;
            candidate.title = Wide(lyric.title);
            candidate.artist = Wide(lyric.artist);
            candidate.album = Wide(lyric.album);
            candidate.source = lyric.source.empty() ? Name() : Wide(lyric.source);
            candidate.lrc = Wide(lyric.text);
            candidate.confidence = lyric.confidence;
            found(std::move(candidate));
        };
        ScriptHost host(std::move(env));
        const auto cleaned = [&](const std::wstring& text) {
            return Utf8(m_script.rawMeta ? text : Clean(text));
        };
        std::map<std::string, std::string> meta{
            {"title", cleaned(query.title)},   {"artist", cleaned(query.artist)},
            {"album", cleaned(query.album)},   {"rawTitle", Utf8(query.title)},
            {"rawArtist", Utf8(query.artist)}, {"rawAlbum", Utf8(query.album)},
            {"path", Utf8(query.path)},        {"rawPath", Utf8(query.path)}};
        try {
            host.Search(m_script, meta, query.subsong, query.duration);
        } catch (const std::exception& error) {
            if (!abort.is_aborting()) console::printf("Glasswave lyrics [%s]: %s", key.c_str(), error.what());
        }
    }

  private:
    ScriptConfig m_script;
    std::shared_ptr<const Catalog> m_catalog;
};

} // namespace

void Start() {
    if (!g_started) {
        g_started = true;
        g_alive = std::make_shared<int>(0);
        Reload();
    }
}

void Reload() {
    if (g_loading || g_stopping) return;
    if (g_loader.joinable()) g_loader.join();
    g_loading = true;
    const auto roots = Roots();
    g_loader = std::thread([roots, alive = std::weak_ptr<int>(g_alive)] {
        auto catalog = std::make_shared<Catalog>();
        catalog->roots = roots;
        for (const auto* folder : {L"parser", L"searcher"}) {
            std::map<std::wstring, std::filesystem::path> files;
            for (const auto& root : roots) {
                std::error_code ec;
                for (const auto& file : std::filesystem::directory_iterator(root / folder, ec))
                    if (file.is_regular_file(ec) && file.path().extension() == L".js")
                        files[file.path().filename().wstring()] = file.path();
            }
            for (const auto& [name, path] : files) {
                if (g_stopping) return;
                try {
                    ScriptEnvironment env;
                    env.roots = roots;
                    env.aborted = [] { return g_stopping.load(); };
                    ScriptHost host(std::move(env));
                    auto cfg = host.Configure(path);
                    if (cfg.name.empty()) cfg.name = Utf8(name);
                    if (std::wstring_view(folder) == L"parser")
                        catalog->parsers.push_back(std::move(cfg));
                    else
                        catalog->searchers.push_back(std::move(cfg));
                } catch (const std::exception& error) {
                    console::printf("Glasswave lyrics [%s]: %s", Utf8(name).c_str(), error.what());
                }
            }
        }
        fb2k::inMainThread([catalog, alive] {
            if (alive.expired()) return;
            for (const auto& old : Searchers())
                if (dynamic_cast<ScriptSearcher*>(old.get())) UnregisterSearcher(old.get());
            for (const auto& script : catalog->searchers) {
                RegisterSearcher(std::make_shared<ScriptSearcher>(script, catalog));
                console::printf("Glasswave lyrics: loaded %s", script.name.c_str());
            }
            g_loading = false;
            if (g_loaded) Service::Get().ClearCache();
            g_loaded = true;
            if (HWND main = core_api::get_main_window()) PostMessageW(main, settings::kChangedMessage, 0, 0);
        });
    });
}

void Shutdown() {
    g_stopping = true;
    g_alive.reset();
    if (g_loader.joinable()) g_loader.join();
    Service::Get().Shutdown();
}

std::vector<std::shared_ptr<Searcher>> Ordered(std::wstring_view provider) {
    auto searchers = Searchers();
    std::erase_if(searchers, [](const auto& searcher) { return !Enabled(searcher->Key()); });
    std::vector<std::wstring> order;
    std::istringstream entries(Order(provider));
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        entry.erase(std::remove_if(entry.begin(), entry.end(), [](unsigned char ch) { return isspace(ch); }),
                    entry.end());
        order.push_back(Wide(entry));
    }
    const auto rank = [&](const auto& searcher) {
        return std::find(order.begin(), order.end(), searcher->Key()) - order.begin();
    };
    std::stable_sort(searchers.begin(), searchers.end(),
                     [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    return searchers;
}

std::vector<settings::Field> SettingFields() {
    using settings::Field;
    std::vector<Field> fields;
    Field note;
    note.kind = Field::Kind::Note;
    note.label = L"Lyrics scripts";
    note.note = [] { return std::to_wstring(Searchers().size()); };
    fields.push_back(std::move(note));
    for (size_t i = 0; i < Roots().size(); ++i) {
        Field folder;
        folder.kind = Field::Kind::Note;
        folder.label = i == 0 ? L"ESLyric script folder" : L"Glasswave script folder";
        folder.note = [i] { return Roots()[i].wstring(); };
        fields.push_back(std::move(folder));
    }
    const auto button = [&](std::wstring label, std::function<void(HWND)> press) {
        Field field;
        field.kind = Field::Kind::Button;
        field.label = std::move(label);
        field.press = std::move(press);
        fields.push_back(std::move(field));
    };
    button(L"Reload lyrics scripts", [](HWND) { Reload(); });
    button(L"Open lyrics script folder", [](HWND owner) {
        const auto path = Roots().back();
        std::error_code ec;
        std::filesystem::create_directories(path / L"searcher", ec);
        ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    button(L"Clear lyrics cache", [](HWND) { Service::Get().ClearCache(); });
    std::vector<std::pair<std::wstring, std::wstring>> contexts{{L"local", L"Local lyric source order"}};
    for (auto* provider : stream::Providers())
        contexts.emplace_back(provider->Key(), provider->Name() + L" lyric source order");
    for (const auto& [key, label] : contexts) {
        Field field;
        field.kind = Field::Kind::Text;
        field.label = label;
        field.defaultText = Wide(DefaultOrder(key));
        field.getText = [key] { return Wide(Order(key)); };
        field.setText = [key](const std::wstring& text) {
            auto orders = Orders();
            orders[Utf8(key)] = Utf8(text);
            std::string serialized;
            for (const auto& [name, value] : orders)
                serialized += name + "=" + value + "\n";
            cfg_order = serialized.c_str();
            Service::Get().ClearCache();
        };
        fields.push_back(std::move(field));
    }
    for (const auto& searcher : Searchers()) {
        const auto key = searcher->Key();
        Field field;
        field.kind = Field::Kind::Choice;
        field.label = searcher->Name();
        field.choices = {L"Enabled", L"Disabled"};
        field.get = [key] { return Enabled(key) ? 0 : 1; };
        field.set = [key](int value) {
            std::string disabled = cfg_disabled.get().c_str(), tag = "\n" + Utf8(key) + "\n";
            const size_t at = disabled.find(tag);
            if (value && at == std::string::npos) disabled += tag;
            if (!value && at != std::string::npos) disabled.erase(at, tag.size());
            cfg_disabled = disabled.c_str();
            Service::Get().ClearCache();
        };
        fields.push_back(std::move(field));
    }
    return fields;
}

namespace {
class ScriptsInit : public initquit {
    void on_init() override { Start(); }
    void on_quit() override { Shutdown(); }
};
FB2K_SERVICE_FACTORY(ScriptsInit);
} // namespace
} // namespace glasswave::lyrics::scripts
