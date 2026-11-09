#include "pch.h"
#include "stream/provider.h"
#include "stream/playback_source.h"

namespace glasswave::stream {

namespace {

std::vector<Provider*>& Registry() {
    static std::vector<Provider*> providers;
    return providers;
}

const wchar_t* KindName(ArtKind kind) {
    switch (kind) {
    case ArtKind::Track: return L"song";
    case ArtKind::Album: return L"album";
    case ArtKind::Artist: return L"artist";
    case ArtKind::Playlist: return L"playlist";
    }
    return L"";
}

}  // namespace

std::wstring JoinArtists(const std::vector<ArtistRef>& artists) {
    std::wstring out;
    for (const auto& artist : artists) {
        if (artist.name.empty()) continue;
        if (!out.empty()) out += L"; ";
        out += artist.name;
    }
    return out;
}

std::wstring Provider::ArtKey(ArtKind kind, const Id& id) const { return ArtKeyPrefix(kind) + id; }

std::wstring Provider::ArtKeyPrefix(ArtKind kind) const {
    return std::wstring(Key()) + L':' + KindName(kind) + L':';
}

void Provider::AddListener(Listener* listener) {
    if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end())
        m_listeners.push_back(listener);
}

void Provider::RemoveListener(Listener* listener) {
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
}

// Listeners may remove themselves while being notified, so iterate over a copy.
void Provider::NotifyAccountChanged() {
    ResetSourceAvailability();
    for (Listener* listener : std::vector<Listener*>(m_listeners)) listener->OnAccountChanged(*this);
}

void Provider::NotifyLibraryChanged() {
    for (Listener* listener : std::vector<Listener*>(m_listeners)) listener->OnLibraryChanged(*this);
}

void Provider::NotifyFavoritesChanged() {
    for (Listener* listener : std::vector<Listener*>(m_listeners)) listener->OnFavoritesChanged(*this);
}

const std::vector<Provider*>& Providers() { return Registry(); }

void StartProviders() {
    for (Provider* provider : Registry()) provider->Start();
}

void Register(Provider& provider) {
    auto& providers = Registry();
    if (std::find(providers.begin(), providers.end(), &provider) == providers.end()) providers.push_back(&provider);
}

void Unregister(Provider& provider) {
    auto& providers = Registry();
    providers.erase(std::remove(providers.begin(), providers.end(), &provider), providers.end());
}

Provider* FindProvider(std::wstring_view key) {
    for (Provider* provider : Registry())
        if (provider->Key() == key) return provider;
    return nullptr;
}

Provider* ProviderOf(const metadb_handle_ptr& item) {
    if (!item.is_valid()) return nullptr;
    for (Provider* provider : Registry())
        if (provider->Owns(item)) return provider;
    return nullptr;
}

}  // namespace glasswave::stream
