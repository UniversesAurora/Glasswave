#include "pch.h"
#include "lyrics/native_sources.h"

#include "cache/store.h"
#include "lyrics/lyrics_service.h"
#include "stream/provider.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace glasswave::lyrics::native_sources {
namespace {

constexpr wchar_t kSourceOrder[] = L"source";
constexpr wchar_t kSidecarOrder[] = L"sidecar";

std::wstring Wide(std::string_view value) {
    return value.empty() ? std::wstring()
                         : pfc::stringcvt::string_wide_from_utf8(std::string(value).c_str()).get_ptr();
}

std::string Utf8(std::wstring_view value) {
    return value.empty() ? std::string()
                         : pfc::stringcvt::string_utf8_from_wide(std::wstring(value).c_str()).get_ptr();
}

struct Scope {
    std::wstring value;
    uint64_t revision = 0;

    bool Current(abort_callback& abort) const {
        if (abort.is_aborting()) return false;
        try {
            return cache::AccountRevision(Utf8(provider)) == revision &&
                   cache::AccountScope(Utf8(provider)) == Utf8(value);
        } catch (...) {
            return false;
        }
    }

    std::wstring provider;
};

class EmbeddedSearcher final : public Searcher {
  public:
    std::wstring Name() const override { return L"File embedded (Source)"; }
    std::wstring Key() const override { return L"native:source:file"; }
    std::wstring OrderKey(std::wstring_view provider) const override {
        (void)provider;
        return kSourceOrder;
    }
    bool AppliesTo(std::wstring_view provider) const override { return provider == L"local"; }
    bool Cacheable() const override { return false; }

    void Search(const Query& query, abort_callback& abort,
                const std::function<void(Candidate)>& found) override {
        if (query.provider != L"local" || query.embeddedLyrics.empty() || abort.is_aborting()) return;
        const auto parsed = ParseLrc(query.embeddedLyrics);
        if (parsed.lines.empty()) return;
        Candidate candidate;
        candidate.title = query.title;
        candidate.artist = query.artist;
        candidate.album = query.album;
        candidate.source = Name();
        candidate.sourceKey = Key();
        candidate.lrc = query.embeddedLyrics;
        candidate.confidence = 100;
        found(std::move(candidate));
    }
};

class SidecarSearcher final : public Searcher {
  public:
    std::wstring Name() const override { return L"Same-name lyric file (Source)"; }
    std::wstring Key() const override { return L"native:sidecar"; }
    std::wstring OrderKey(std::wstring_view provider) const override {
        (void)provider;
        return kSidecarOrder;
    }
    bool AppliesTo(std::wstring_view provider) const override { return provider == L"local"; }
    bool Cacheable() const override { return false; }

    void Search(const Query& query, abort_callback& abort,
                const std::function<void(Candidate)>& found) override {
        // A same-stem sidecar is file-level, not track-specific. Do not apply it to CUE or
        // other subsongs and never guess an offset; embedded lyrics and explicit selections
        // remain available per subsong through their separate paths.
        if (query.provider != L"local" || query.subsong != 0 || query.nativePath.empty() ||
            abort.is_aborting())
            return;
        std::filesystem::path path(query.nativePath);
        path.replace_extension(L".lrc");
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return;
        if (error || std::filesystem::file_size(path, error) > 4ull * 1024 * 1024 || error) {
            console::printf("Glasswave lyrics [sidecar]: cannot read the companion lyric file");
            return;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            console::printf("Glasswave lyrics [sidecar]: cannot open the companion lyric file");
            return;
        }
        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        if (input.bad() || abort.is_aborting()) return;
        std::wstring text = DecodeText(bytes);
        if (ParseLrc(text).lines.empty()) return;
        Candidate candidate;
        candidate.title = query.title;
        candidate.artist = query.artist;
        candidate.album = query.album;
        candidate.source = Name();
        candidate.sourceKey = Key();
        candidate.lrc = std::move(text);
        candidate.confidence = 100;
        found(std::move(candidate));
    }
};

class ProviderSearcher final : public Searcher {
  public:
    explicit ProviderSearcher(stream::Provider& provider) : m_providerKey(provider.Key()) {
        m_name = provider.Name() + L" (Source)";
        m_key = L"native:provider:" + std::wstring(m_providerKey);
    }
    std::wstring Name() const override { return m_name; }
    std::wstring Key() const override { return m_key; }
    std::wstring OrderKey(std::wstring_view provider) const override {
        return provider == m_providerKey ? kSourceOrder : m_key;
    }
    bool AppliesTo(std::wstring_view provider) const override { return provider == m_providerKey; }

    void Search(const Query& query, abort_callback& abort,
                const std::function<void(Candidate)>& found) override {
        auto* provider = stream::FindProvider(m_providerKey);
        if (!provider || provider->Key() != query.provider || query.providerId.empty() || abort.is_aborting())
            return;
        Scope scope;
        scope.provider = m_providerKey;
        try {
            scope.value = Wide(cache::AccountScope(Utf8(m_providerKey)));
            scope.revision = cache::AccountRevision(Utf8(m_providerKey));
        } catch (const std::exception& error) {
            console::printf("Glasswave lyrics [%s]: %s", Utf8(m_key).c_str(), error.what());
            return;
        }
        if (!scope.Current(abort)) return;
        std::wstring text;
        try {
            text = provider->Lyrics(query.providerId, abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& error) {
            if (!abort.is_aborting())
                console::printf("Glasswave lyrics [%s]: %s", Utf8(m_key).c_str(), error.what());
            return;
        }
        abort.check();
        if (!scope.Current(abort) || ParseLrc(text).lines.empty()) return;
        Candidate candidate;
        candidate.title = query.title;
        candidate.artist = query.artist;
        candidate.album = query.album;
        candidate.source = Name();
        candidate.sourceKey = Key();
        candidate.lrc = std::move(text);
        candidate.confidence = 100;
        candidate.cacheProvider = m_providerKey;
        candidate.cacheScope = scope.value;
        candidate.accountRevision = scope.revision;
        found(std::move(candidate));
    }

  private:
    std::wstring m_providerKey;
    std::wstring m_name;
    std::wstring m_key;
};

std::vector<std::shared_ptr<Searcher>> g_builtins;

} // namespace

void Start() {
    if (!g_builtins.empty()) return;
    g_builtins.push_back(std::make_shared<EmbeddedSearcher>());
    g_builtins.push_back(std::make_shared<SidecarSearcher>());
    for (auto* provider : stream::Providers()) {
        if (!provider || !provider->HasNativeLyrics()) continue;
        g_builtins.push_back(std::make_shared<ProviderSearcher>(*provider));
    }
    for (const auto& searcher : g_builtins)
        RegisterSearcher(searcher);
}

void Shutdown() {
    for (const auto& searcher : g_builtins)
        UnregisterSearcher(searcher.get());
    g_builtins.clear();
}

} // namespace glasswave::lyrics::native_sources
