#ifndef GLASSWAVE_CACHE_STANDALONE
#include "pch.h"
#endif

#include "cache/provider_snapshot.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace glasswave::cache::provider_snapshot {
namespace {

constexpr std::uint32_t kSchema = 3;
constexpr std::uint64_t kMaxSnapshotBytes = 8ull * 1024 * 1024;
constexpr std::size_t kMaxPlaylists = 4096;
constexpr std::size_t kMaxBindings = 64;
constexpr std::size_t kMaxBindingKeyBytes = 256;
constexpr std::size_t kMaxBindingResourceBytes = 4096;
constexpr std::size_t kMaxStringBytes = 1024 * 1024;
constexpr char kMagic[] = "GWPS";

std::wstring Wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Provider snapshot contains invalid UTF-8.");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), size) != size)
        throw std::runtime_error("Provider snapshot UTF-8 conversion failed.");
    return result;
}

std::size_t Utf8Size(std::wstring_view value, std::size_t limit) {
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::uint32_t code = static_cast<std::uint16_t>(value[i]);
        std::size_t width = 0;
        if (code <= 0x7f) {
            width = 1;
        } else if (code <= 0x7ff) {
            width = 2;
        } else if (code >= 0xd800 && code <= 0xdbff) {
            if (++i >= value.size()) throw std::runtime_error("Provider snapshot contains unpaired text.");
            const std::uint32_t low = static_cast<std::uint16_t>(value[i]);
            if (low < 0xdc00 || low > 0xdfff)
                throw std::runtime_error("Provider snapshot contains unpaired text.");
            width = 4;
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            throw std::runtime_error("Provider snapshot contains unpaired text.");
        } else {
            width = 3;
        }
        if (width > limit - bytes) throw std::runtime_error("Provider snapshot string is too large.");
        bytes += width;
    }
    return bytes;
}

class Encoder {
  public:
    void Magic(std::string_view value) { Bytes(value); }
    void U32(std::uint32_t value) {
        Need(4);
        for (unsigned shift = 0; shift < 32; shift += 8)
            m_bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    }
    void I32(std::int32_t value) { U32(static_cast<std::uint32_t>(value)); }
    void Boolean(bool value) {
        Need(1);
        m_bytes.push_back(value ? 1 : 0);
    }
    void String(std::string_view value) {
        if (value.size() > kMaxStringBytes || !RemainingAtLeast(4 + value.size()))
            throw std::runtime_error("Provider snapshot string is too large.");
        U32(static_cast<std::uint32_t>(value.size()));
        Bytes(value);
    }
    void WideString(std::wstring_view value) {
        if (value.size() > kMaxStringBytes)
            throw std::runtime_error("Provider snapshot string is too large.");
        const std::size_t available = Remaining() >= 4 ? Remaining() - 4 : 0;
        const std::size_t expected = Utf8Size(value, std::min(kMaxStringBytes, available));
        Need(4 + expected);
        U32(static_cast<std::uint32_t>(expected));
        const std::size_t start = m_bytes.size();
        m_bytes.resize(start + expected);
        if (expected &&
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                m_bytes.data() + start, static_cast<int>(expected), nullptr,
                                nullptr) != static_cast<int>(expected))
            throw std::runtime_error("Provider snapshot text conversion failed.");
    }
    std::string Finish() && { return std::move(m_bytes); }

  private:
    std::size_t Remaining() const { return static_cast<std::size_t>(kMaxSnapshotBytes) - m_bytes.size(); }
    bool RemainingAtLeast(std::size_t size) const { return size <= Remaining(); }
    void Need(std::size_t size) {
        if (!RemainingAtLeast(size)) throw std::runtime_error("Provider snapshot is too large.");
    }
    void Bytes(std::string_view value) {
        Need(value.size());
        m_bytes.append(value);
    }

    std::string m_bytes;
};

void EncodePlaylist(Encoder& out, const stream::Playlist& playlist) {
    out.WideString(playlist.id);
    out.WideString(playlist.name);
    out.WideString(playlist.coverUrl);
    out.I32(playlist.trackCount);
    out.Boolean(playlist.own);
    out.Boolean(playlist.saved);
}

class Reader {
  public:
    explicit Reader(std::string_view bytes) : m_bytes(bytes) {}

    std::uint32_t U32() {
        Require(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(m_bytes[m_at++])) << shift;
        return value;
    }
    std::int32_t I32() { return static_cast<std::int32_t>(U32()); }
    bool Boolean() {
        Require(1);
        const auto value = m_bytes[m_at++];
        if (value > 1) throw std::runtime_error("Provider snapshot boolean is invalid.");
        return value != 0;
    }
    std::string String() {
        const std::uint32_t size = U32();
        if (size > kMaxStringBytes) throw std::runtime_error("Provider snapshot string is too large.");
        Require(size);
        const std::string value(m_bytes.substr(m_at, size));
        m_at += size;
        return value;
    }
    std::string Magic() {
        Require(sizeof(kMagic) - 1);
        const std::string value(m_bytes.substr(m_at, sizeof(kMagic) - 1));
        m_at += sizeof(kMagic) - 1;
        return value;
    }
    bool Empty() const { return m_at == m_bytes.size(); }

  private:
    void Require(std::size_t size) const {
        if (size > m_bytes.size() - m_at) throw std::runtime_error("Provider snapshot is truncated.");
    }

    std::string_view m_bytes;
    std::size_t m_at = 0;
};

void EncodePlaylists(Encoder& out, const std::vector<stream::Playlist>& playlists) {
    if (playlists.size() > kMaxPlaylists)
        throw std::runtime_error("Provider snapshot has too many playlists.");
    out.U32(static_cast<std::uint32_t>(playlists.size()));
    for (const auto& playlist : playlists)
        EncodePlaylist(out, playlist);
}

std::vector<stream::Playlist> ReadPlaylists(Reader& reader) {
    const std::uint32_t count = reader.U32();
    if (count > kMaxPlaylists) throw std::runtime_error("Provider snapshot has too many playlists.");
    std::vector<stream::Playlist> playlists;
    playlists.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        stream::Playlist playlist;
        playlist.id = Wide(reader.String());
        playlist.name = Wide(reader.String());
        playlist.coverUrl = Wide(reader.String());
        playlist.trackCount = reader.I32();
        playlist.own = reader.Boolean();
        playlist.saved = reader.Boolean();
        playlists.push_back(std::move(playlist));
    }
    return playlists;
}

void EncodeBindings(Encoder& out, const std::vector<NavigationTargetBinding>& bindings) {
    if (bindings.size() > kMaxBindings)
        throw std::runtime_error("Provider snapshot has too many navigation bindings.");
    out.U32(static_cast<std::uint32_t>(bindings.size()));
    std::string_view previous;
    for (const auto& binding : bindings) {
        if (binding.key.empty() || binding.key.size() > kMaxBindingKeyBytes || binding.resourceId.empty() ||
            binding.resourceId.size() > kMaxBindingResourceBytes ||
            (!previous.empty() && binding.key <= previous))
            throw std::runtime_error("Provider snapshot navigation binding is invalid.");
        out.String(binding.key);
        out.String(binding.resourceId);
        previous = binding.key;
    }
}

std::vector<NavigationTargetBinding> ReadBindings(Reader& reader) {
    const std::uint32_t count = reader.U32();
    if (count > kMaxBindings) throw std::runtime_error("Provider snapshot has too many navigation bindings.");
    std::vector<NavigationTargetBinding> bindings;
    bindings.reserve(count);
    std::string previous;
    for (std::uint32_t i = 0; i < count; ++i) {
        NavigationTargetBinding binding;
        binding.key = reader.String();
        binding.resourceId = reader.String();
        if (binding.key.empty() || binding.key.size() > kMaxBindingKeyBytes || binding.resourceId.empty() ||
            binding.resourceId.size() > kMaxBindingResourceBytes ||
            (!previous.empty() && binding.key <= previous))
            throw std::runtime_error("Provider snapshot navigation binding is invalid.");
        previous = binding.key;
        bindings.push_back(std::move(binding));
    }
    return bindings;
}

std::string Encode(const Snapshot& snapshot) {
    Encoder out;
    out.Magic(std::string_view(kMagic, sizeof(kMagic) - 1));
    out.U32(kSchema);
    out.String(snapshot.provider);
    out.String(snapshot.sourceOrigin);
    out.String(snapshot.context);
    out.String(snapshot.locale);
    out.String(snapshot.accountId);
    out.String(snapshot.accountName);
    out.String(snapshot.sourceStorefront);
    out.I32(snapshot.displayRegion);
    EncodePlaylists(out, snapshot.created);
    EncodePlaylists(out, snapshot.saved);
    EncodeBindings(out, snapshot.bindings);
    return std::move(out).Finish();
}

Snapshot Decode(std::string_view bytes) {
    if (bytes.size() > kMaxSnapshotBytes) throw std::runtime_error("Provider snapshot is too large.");
    Reader reader(bytes);
    if (reader.Magic() != std::string_view(kMagic, sizeof(kMagic) - 1))
        throw std::runtime_error("Provider snapshot magic is invalid.");
    if (reader.U32() != kSchema) throw std::runtime_error("Provider snapshot schema is unsupported.");
    Snapshot snapshot;
    snapshot.provider = reader.String();
    snapshot.sourceOrigin = reader.String();
    snapshot.context = reader.String();
    snapshot.locale = reader.String();
    snapshot.accountId = reader.String();
    snapshot.accountName = reader.String();
    snapshot.sourceStorefront = reader.String();
    snapshot.displayRegion = reader.I32();
    snapshot.created = ReadPlaylists(reader);
    snapshot.saved = ReadPlaylists(reader);
    snapshot.bindings = ReadBindings(reader);
    if (!reader.Empty()) throw std::runtime_error("Provider snapshot has trailing data.");
    return snapshot;
}

Key SnapshotKey(std::string_view provider, std::string_view accountScope, std::string_view sourceOrigin,
                std::string_view context) {
    const std::array<std::string_view, 3> identityParts{provider, sourceOrigin, context};
    return Key{Kind::Data, std::string(provider), std::string(accountScope), "provider-bootstrap/v1",
               Context(identityParts)};
}

std::string ReadPayload(const Snapshot& snapshot) {
    if (snapshot.provider.empty() || snapshot.sourceOrigin.empty() || snapshot.context.empty() ||
        snapshot.accountId.empty())
        throw std::runtime_error("Provider snapshot identity is incomplete.");
    return Encode(snapshot);
}

} // namespace

std::string Context(std::span<const std::string_view> parts) {
    std::string out = "provider-context-v1;";
    for (const auto part : parts) {
        out += std::to_string(part.size());
        out.push_back(':');
        out.append(part);
    }
    return out;
}

std::optional<Snapshot> Load(Store& store, std::string accountScope, std::string_view provider,
                             std::string_view sourceOrigin, std::string_view context) {
    if (accountScope.empty() || provider.empty() || sourceOrigin.empty() || context.empty())
        return std::nullopt;
    const auto key = SnapshotKey(provider, accountScope, sourceOrigin, context);
    try {
        const auto stored = store.Lookup(key);
        if (!stored || stored->Size() > kMaxSnapshotBytes) return std::nullopt;
        std::ifstream input(stored->Path(), std::ios::binary);
        if (!input) return std::nullopt;
        std::string bytes(static_cast<std::size_t>(stored->Size()), '\0');
        if (!bytes.empty()) input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input || input.peek() != std::char_traits<char>::eof()) return std::nullopt;
        Snapshot snapshot = Decode(bytes);
        if (snapshot.provider != provider || snapshot.sourceOrigin != sourceOrigin ||
            snapshot.context != context) {
            store.Erase(key);
            return std::nullopt;
        }
        return snapshot;
    } catch (...) {
        try {
            store.Erase(key);
        } catch (...) {
        }
        return std::nullopt;
    }
}

bool Save(Store& store, std::string accountScope, const Snapshot& snapshot) {
    if (accountScope.empty()) return false;
    try {
        const auto bytes = ReadPayload(snapshot);
        const auto key =
            SnapshotKey(snapshot.provider, accountScope, snapshot.sourceOrigin, snapshot.context);
        auto writer = store.Begin(key);
        if (!writer) return false;
        {
            std::ofstream output(writer->Path(), std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) return false;
        }
        Metadata metadata;
        metadata.contentType = "application/vnd.glasswave.provider-bootstrap";
        metadata.extra = "schema=3";
        metadata.validatedAt = Store::Now();
        return writer->Commit(std::move(metadata)).has_value();
    } catch (...) {
        return false;
    }
}

void Erase(Store& store, std::string accountScope, std::string_view provider, std::string_view sourceOrigin,
           std::string_view context) {
    if (accountScope.empty() || provider.empty() || sourceOrigin.empty() || context.empty()) return;
    try {
        store.Erase(SnapshotKey(provider, accountScope, sourceOrigin, context));
    } catch (...) {
    }
}

} // namespace glasswave::cache::provider_snapshot
