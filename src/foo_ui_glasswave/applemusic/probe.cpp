#include "pch.h"
#include "core/text.h"

#include "applemusic/catalog.h"
#include "applemusic/client.h"
#include "applemusic/config.h"

#include <SDK/audio_chunk_impl.h>
#include <SDK/file_info_impl.h>
#include <SDK/initquit.h>
#include <SDK/input.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>

namespace glasswave::applemusic {
namespace {

namespace fs = std::filesystem;
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

constexpr size_t kMaxRequestBytes = 64u * 1024u;
constexpr size_t kMaxSongIdBytes = 1024u;
constexpr double kDecoderPaddingToleranceSeconds = 0.125;
constexpr double kAtmosDurationToleranceSeconds = 1.0;
constexpr std::wstring_view kMetadataFixtureId = L"glasswave-test";
constexpr std::wstring_view kMetadataTitle = L"Glasswave metadata title";
constexpr std::wstring_view kMetadataArtist = L"Glasswave metadata artist";
constexpr std::wstring_view kMappingFixtureCatalogId = L"1237064886";
constexpr std::wstring_view kMappingFixtureLibraryId = L"i.GlasswaveStaleFixture";
constexpr int kMetadataFixtureDurationMs = 3000;

struct ProbePaths {
    fs::path directory;
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct ProbeRequest {
    std::string input_path;
    bool requires_seek = false;
    bool verify_metadata = false;
    bool fixture_mapping = false;
    bool cancel_after_first_chunk = false;
    bool seek_stress = false;
    bool atmos = false;
    AtmosOutput atmos_output = AtmosOutput::StereoSpeakers;
};

struct MetadataSnapshot {
    std::wstring id;
    std::wstring catalog_id;
    std::wstring library_id;
    std::wstring reporting_id;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int duration_ms = 0;

    bool operator==(const MetadataSnapshot&) const = default;
};

struct ProbeResult {
    bool ok = false;
    std::optional<std::wstring> codec;
    std::optional<uint32_t> bit_depth;
    std::optional<uint32_t> sample_rate;
    std::optional<uint32_t> channels;
    std::optional<uint64_t> samples;
    std::optional<double> decoded_seconds;
    std::optional<double> declared_seconds;
    std::optional<double> first_decoded_ms;
    std::optional<int64_t> first_decoded_unix_ms;
    bool can_seek = false;
    bool seek_verified = false;
    uint32_t seek_stress_count = 0;
    bool metadata_verified = false;
    bool metadata_identity_verified = false;
    bool fixture_transaction_restored = false;
    std::string metadata_input_path;
    std::optional<MetadataSnapshot> metadata_source;
    std::optional<MetadataSnapshot> metadata_display;
    std::optional<MetadataSnapshot> metadata_get_info;
    std::optional<MetadataSnapshot> metadata_dynamic_info;
    bool canceled_after_first_chunk = false;
    std::optional<double> shutdown_ms;
    bool atmos_rendered = false;
    std::optional<uint32_t> atmos_objects;
    std::optional<std::wstring> render_output;
    std::optional<uint32_t> channel_mask;
    std::wstring decoder_error;
    std::wstring error;
};

using core::Utf8;
using core::Wide;

std::optional<fs::path> FinalPath(const fs::path& path, bool directory) {
    const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) return std::nullopt;

    std::wstring text(buffer.data(), length);
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        text = L"\\\\" + text.substr(8);
    } else if (text.rfind(L"\\\\?\\", 0) == 0) {
        text.erase(0, 4);
    }
    return fs::path(text);
}

bool SameName(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool IsWithin(const fs::path& candidate, const fs::path& root) {
    auto candidate_part = candidate.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part) {
        if (candidate_part == candidate.end() || !SameName(*candidate_part, *root_part)) return false;
        ++candidate_part;
    }
    return candidate_part != candidate.end();
}

std::optional<fs::path> NativeProfileDirectory() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return FinalPath(fs::path(Wide(native.get_ptr())), true);
}

std::optional<fs::path> RepositoryRootFromDevProfile(const fs::path& profile) {
    if (!SameName(profile.filename(), fs::path(L"profile"))) return std::nullopt;
    const fs::path instance = profile.parent_path();
    const fs::path local = instance.parent_path();
    if (!SameName(instance.filename(), fs::path(L"fb2k-dev")) ||
        !SameName(local.filename(), fs::path(L".local"))) {
        return std::nullopt;
    }
    return local.parent_path();
}

std::optional<ProbePaths> GetProbePaths() {
    const auto profile = NativeProfileDirectory();
    if (!profile) return std::nullopt;
    const fs::path directory = *profile / L"glasswave" / L"applemusic";
    return ProbePaths{
        .directory = directory,
        .request = directory / L"probe-request.json",
        .claimed = directory / L"probe-request.claimed.json",
        .result = directory / L"probe-result.json",
    };
}

std::optional<std::string> ReadBoundedText(const fs::path& path, size_t limit) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > limit) return std::nullopt;

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) return std::nullopt;
    return bytes;
}

bool HasControl(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < 0x20 || c == 0x7f; });
}

std::optional<AtmosOutput> ParseAtmosOutput(std::wstring_view value) {
    if (value == L"StereoSpeakers") return AtmosOutput::StereoSpeakers;
    if (value == L"Headphones") return AtmosOutput::Headphones;
    if (value == L"Speakers51") return AtmosOutput::Speakers51;
    if (value == L"Speakers71") return AtmosOutput::Speakers71;
    return std::nullopt;
}

std::optional<ProbeRequest> ParseRequest(std::string_view bytes, const fs::path& profile,
                                         std::wstring& error) {
    try {
        const JsonObject object = JsonObject::Parse(winrt::to_hstring(bytes));
        const bool has_song = object.HasKey(L"songId");
        const bool has_fixture = object.HasKey(L"localFixture");
        if (has_song == has_fixture) {
            error = L"invalid_request";
            return std::nullopt;
        }

        bool verify_metadata = false;
        if (object.HasKey(L"verifyMetadata")) {
            const IJsonValue value = object.Lookup(L"verifyMetadata");
            if (!value || value.ValueType() != JsonValueType::Boolean) {
                error = L"invalid_verify_metadata";
                return std::nullopt;
            }
            verify_metadata = value.GetBoolean();
        }
        bool cancel_after_first_chunk = false;
        bool fixture_mapping = false;
        if (object.HasKey(L"fixtureMapping")) {
            const IJsonValue value = object.Lookup(L"fixtureMapping");
            if (!value || value.ValueType() != JsonValueType::Boolean) {
                error = L"invalid_fixture_mapping";
                return std::nullopt;
            }
            fixture_mapping = value.GetBoolean();
        }
        bool seek_stress = false;
        if (object.HasKey(L"seekStress")) {
            const auto value = object.Lookup(L"seekStress");
            if (!value || value.ValueType() != JsonValueType::Boolean) {
                error = L"invalid_seek_stress";
                return std::nullopt;
            }
            seek_stress = value.GetBoolean();
        }
        if (object.HasKey(L"cancelAfterFirstChunk")) {
            const IJsonValue value = object.Lookup(L"cancelAfterFirstChunk");
            if (!value || value.ValueType() != JsonValueType::Boolean) {
                error = L"invalid_cancel_stream";
                return std::nullopt;
            }
            cancel_after_first_chunk = value.GetBoolean();
        }
        bool atmos = false;
        if (object.HasKey(L"atmos")) {
            const IJsonValue value = object.Lookup(L"atmos");
            if (!value || value.ValueType() != JsonValueType::Boolean) {
                error = L"invalid_atmos_marker";
                return std::nullopt;
            }
            atmos = value.GetBoolean();
        }
        AtmosOutput atmos_output = AtmosOutput::StereoSpeakers;
        if (object.HasKey(L"atmosOutput")) {
            const IJsonValue value = object.Lookup(L"atmosOutput");
            if (!value || value.ValueType() != JsonValueType::String) {
                error = L"invalid_atmos_output";
                return std::nullopt;
            }
            const auto parsed = ParseAtmosOutput(value.GetString().c_str());
            if (!parsed || !atmos) {
                error = L"invalid_atmos_output";
                return std::nullopt;
            }
            atmos_output = *parsed;
        } else if (atmos) {
            error = L"atmos_output_required";
            return std::nullopt;
        }

        if (has_song) {
            const IJsonValue value = object.Lookup(L"songId");
            if (!value || value.ValueType() != JsonValueType::String) {
                error = L"invalid_song_id";
                return std::nullopt;
            }
            const std::wstring id(value.GetString().c_str());
            const std::string id_utf8 = Utf8(id);
            if (id.empty() || id_utf8.size() > kMaxSongIdBytes || HasControl(id)) {
                error = L"invalid_song_id";
                return std::nullopt;
            }
            const std::string path = Catalog::Get().Records().SongPath(id);
            if (path.empty()) {
                error = L"invalid_song_id";
                return std::nullopt;
            }
            if (verify_metadata && std::wstring_view(id) != kMetadataFixtureId &&
                !Catalog::Get().FindSongRaw(id)) {
                error = L"metadata_catalog_entry_required";
                return std::nullopt;
            }
            if (fixture_mapping && (std::wstring_view(id) != kMetadataFixtureId || !verify_metadata ||
                                    atmos || cancel_after_first_chunk)) {
                error = L"mapping_fixture_required";
                return std::nullopt;
            }
            if (cancel_after_first_chunk && std::wstring_view(id) != kMetadataFixtureId) {
                error = L"cancel_fixture_required";
                return std::nullopt;
            }
            return ProbeRequest{
                .input_path = path,
                .requires_seek = true,
                .verify_metadata = verify_metadata,
                .fixture_mapping = fixture_mapping,
                .cancel_after_first_chunk = cancel_after_first_chunk,
                .seek_stress = seek_stress,
                .atmos = atmos,
                .atmos_output = atmos_output,
            };
        }

        if (verify_metadata || fixture_mapping || cancel_after_first_chunk || atmos) {
            error = L"fixture_only_option_forbidden_for_local_files";
            return std::nullopt;
        }
        const IJsonValue value = object.Lookup(L"localFixture");
        if (!value || value.ValueType() != JsonValueType::String) {
            error = L"invalid_fixture_path";
            return std::nullopt;
        }
        const auto repository = RepositoryRootFromDevProfile(profile);
        if (!repository) {
            error = L"fixture_profile_not_dev_instance";
            return std::nullopt;
        }
        const auto fixture = FinalPath(fs::path(value.GetString().c_str()), false);
        const auto allowed = FinalPath(*repository / L".local" / L"tests", true);
        if (!fixture || !allowed || !fs::is_regular_file(*fixture) || !IsWithin(*fixture, *allowed)) {
            error = L"fixture_outside_allowed_root";
            return std::nullopt;
        }
        return ProbeRequest{
            .input_path = Utf8(fixture->native()),
            .requires_seek = false,
            .verify_metadata = false,
            .cancel_after_first_chunk = false,
            .atmos = false,
            .atmos_output = AtmosOutput::StereoSpeakers,
        };
    } catch (...) {
        error = L"invalid_json";
        return std::nullopt;
    }
}

std::optional<std::wstring> ActualCodec(const file_info& info) {
    const char* value = info.info_get("codec");
    if (!value || !*value) {
        pfc::string8 codec;
        if (info.info_get_codec_long(codec)) value = codec.get_ptr();
    }
    if (!value || !*value) return std::nullopt;
    return Wide(value);
}

std::wstring UpperAscii(std::wstring_view text) {
    std::wstring upper(text);
    for (wchar_t& c : upper) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return upper;
}

bool Contains(std::wstring_view text, std::wstring_view needle) {
    return UpperAscii(text).find(UpperAscii(needle)) != std::wstring::npos;
}

uint32_t ExpectedAtmosChannels(AtmosOutput output) {
    switch (output) {
    case AtmosOutput::StereoSpeakers:
    case AtmosOutput::Headphones:
        return 2;
    case AtmosOutput::Speakers51:
        return 6;
    case AtmosOutput::Speakers71:
        return 8;
    }
    return 0;
}

uint32_t ExpectedAtmosChannelMask(AtmosOutput output) {
    switch (output) {
    case AtmosOutput::StereoSpeakers:
    case AtmosOutput::Headphones:
        return 0x000003;
    case AtmosOutput::Speakers51:
        return 0x00060F;
    case AtmosOutput::Speakers71:
        return 0x00063F;
    }
    return 0;
}

std::wstring_view ExpectedAtmosOutputName(AtmosOutput output) {
    switch (output) {
    case AtmosOutput::StereoSpeakers:
        return L"stereo speakers";
    case AtmosOutput::Headphones:
        return L"headphones";
    case AtmosOutput::Speakers51:
        return L"5.1 speakers";
    case AtmosOutput::Speakers71:
        return L"7.1 speakers";
    }
    return {};
}

Song MetadataFixture(bool mapping) {
    Song song;
    song.track.id = std::wstring(kMetadataFixtureId);
    song.track.title = std::wstring(kMetadataTitle);
    song.track.durationMs = kMetadataFixtureDurationMs;
    if (mapping) {
        song.catalogId = std::wstring(kMappingFixtureCatalogId);
        song.libraryId = std::wstring(kMappingFixtureLibraryId);
    }
    stream::ArtistRef artist;
    artist.id = L"glasswave-test-artist";
    artist.name = std::wstring(kMetadataArtist);
    song.track.artists.push_back(std::move(artist));
    return song;
}

MetadataSnapshot Snapshot(const Song& song) {
    return {.id = song.track.id,
            .catalog_id = song.catalogId,
            .library_id = song.libraryId,
            .reporting_id = song.reportingId,
            .title = song.track.title,
            .artist = stream::JoinArtists(song.track.artists),
            .album = song.track.album,
            .duration_ms = song.track.durationMs};
}

MetadataSnapshot Snapshot(const file_info& info) {
    const auto tag = [&info](const char* name) {
        const char* value = info.meta_get(name, 0);
        return value ? Wide(value) : std::wstring();
    };
    return {.title = tag("title"), .artist = tag("artist"), .album = tag("album")};
}

bool SameIdentity(const MetadataSnapshot& left, const MetadataSnapshot& right) {
    return left.id == right.id && left.catalog_id == right.catalog_id &&
           left.library_id == right.library_id && left.reporting_id == right.reporting_id;
}

bool MetadataMatches(const MetadataSnapshot& actual, const MetadataSnapshot& expected) {
    return actual.title == expected.title && actual.artist == expected.artist &&
           (expected.album.empty() || actual.album == expected.album);
}

bool CatalogMetadataUnchanged(const ProbeResult& result) {
    const auto source = Catalog::Get().FindSongRaw(result.metadata_source->id);
    const auto display = Catalog::Get().FindSong(result.metadata_source->id);
    return source && display && Snapshot(*source) == *result.metadata_source &&
           Snapshot(*display) == *result.metadata_display;
}

void SetFailure(ProbeResult& result, std::wstring error) {
    result.ok = false;
    result.error = std::move(error);
}

ProbeResult DecodeRequest(const ProbeRequest& request, abort_callback_impl& abort) {
    const auto decode_started = std::chrono::steady_clock::now();
    ProbeResult result;
    service_ptr_t<input_decoder> decoder;
    bool metadata_fixture = false;
    if (request.verify_metadata) {
        result.metadata_input_path = request.input_path;
        stream::Id id;
        if (!Catalog::Get().Records().ParseSongPath(request.input_path.c_str(), id)) {
            SetFailure(result, L"metadata_song_path_required");
            return result;
        }
        metadata_fixture = id == kMetadataFixtureId;
        const auto source = metadata_fixture ? std::optional<Song>(MetadataFixture(request.fixture_mapping))
                                             : Catalog::Get().FindSongRaw(id);
        const auto display = metadata_fixture ? source : Catalog::Get().FindSong(id);
        if (!source || !display) {
            SetFailure(result, L"metadata_catalog_entry_required");
            return result;
        }
        // Only the reserved fixture identity is seeded; owner catalog records remain read-only.
        if (metadata_fixture) Catalog::Get().Put(*source);
        result.metadata_source = Snapshot(*source);
        result.metadata_display = Snapshot(*display);
        if (source->track.id != id || display->track.id != id ||
            !SameIdentity(*result.metadata_source, *result.metadata_display) ||
            result.metadata_source->title.empty() || result.metadata_source->artist.empty() ||
            result.metadata_display->title.empty() || result.metadata_display->artist.empty()) {
            SetFailure(result, L"metadata_catalog_entry_invalid");
            return result;
        }
    }

    try {
        input_entry::g_open_for_decoding(decoder, nullptr, request.input_path.c_str(), abort);
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        SetFailure(result, L"decoder_open_failed");
        return result;
    }
    if (!decoder.is_valid()) {
        SetFailure(result, L"decoder_open_failed");
        return result;
    }

    constexpr unsigned decode_flags = input_flag_no_looping | input_flag_testing_integrity |
                                      input_flag_no_postproc | input_flag_canonical_decode;
    try {
        decoder->initialize(0, decode_flags, abort);
    } catch (const exception_aborted&) {
        throw;
    } catch (const std::exception& error) {
        result.decoder_error = Wide(error.what());
        SetFailure(result, L"decoder_initialize_failed");
        return result;
    } catch (...) {
        SetFailure(result, L"decoder_initialize_failed");
        return result;
    }

    if (metadata_fixture) Catalog::Get().Put(MetadataFixture(request.fixture_mapping));
    file_info_impl info;
    try {
        decoder->get_info(0, info, abort);
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        SetFailure(result, L"decoder_info_failed");
        return result;
    }
    if (request.verify_metadata) {
        result.metadata_get_info = Snapshot(info);
        if (!MetadataMatches(*result.metadata_get_info, *result.metadata_display) ||
            !CatalogMetadataUnchanged(result)) {
            SetFailure(result, L"metadata_get_info_mismatch");
            return result;
        }
    }
    if (request.verify_metadata) {
        file_info_impl dynamic;
        double delta = 0;
        bool changed = false;
        if (metadata_fixture) Catalog::Get().Put(MetadataFixture(request.fixture_mapping));
        try {
            changed = decoder->get_dynamic_info(dynamic, delta);
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            SetFailure(result, L"metadata_dynamic_info_failed");
            return result;
        }
        if (!changed) {
            SetFailure(result, L"metadata_dynamic_info_not_reported");
            return result;
        }
        result.metadata_dynamic_info = Snapshot(dynamic);
        if (!MetadataMatches(*result.metadata_dynamic_info, *result.metadata_display) ||
            !CatalogMetadataUnchanged(result)) {
            SetFailure(result, L"metadata_dynamic_info_mismatch");
            return result;
        }
        result.metadata_verified = true;
        result.metadata_identity_verified = true;
    }

    result.codec = ActualCodec(info);
    if (request.atmos) {
        if (!result.codec || (!Contains(*result.codec, L"E-AC-3") && !Contains(*result.codec, L"EAC3")) ||
            !Contains(*result.codec, L"JOC")) {
            SetFailure(result, L"atmos_source_codec_mismatch");
            return result;
        }
        const char* renderer = info.info_get("atmos_renderer");
        const char* render_output = info.info_get("atmos_output");
        const int64_t objects = info.info_get_int("atmos_objects");
        if (!renderer || std::string_view(renderer) != "Cavern") {
            SetFailure(result, L"atmos_renderer_mismatch");
            return result;
        }
        const std::wstring actual_output = render_output ? Wide(render_output) : std::wstring();
        if (actual_output != std::wstring(ExpectedAtmosOutputName(request.atmos_output))) {
            SetFailure(result, L"atmos_output_mismatch");
            return result;
        }
        if (objects <= 0 || objects > std::numeric_limits<uint32_t>::max()) {
            SetFailure(result, L"atmos_objects_missing");
            return result;
        }
        result.atmos_objects = static_cast<uint32_t>(objects);
        result.render_output = actual_output;
    }
    const int64_t bit_depth = info.info_get_int("bitspersample");
    if (bit_depth > 0 && bit_depth <= std::numeric_limits<uint32_t>::max()) {
        result.bit_depth = static_cast<uint32_t>(bit_depth);
    }
    const double declared = info.get_length();
    if (std::isfinite(declared) && declared > 0) result.declared_seconds = declared;

    if (request.seek_stress) {
        if (!result.declared_seconds || !decoder->can_seek()) {
            SetFailure(result, L"early_seek_unavailable");
            return result;
        }
        try {
            audio_chunk_impl seekChunk;
            for (const double fraction : {0.9, 0.15, 0.99, 0.5, 0.98, 0.0}) {
                decoder->seek(fraction * *result.declared_seconds, abort);
                for (unsigned i = 0; i < 8 && decoder->run(seekChunk, abort); ++i)
                    abort.check();
                ++result.seek_stress_count;
            }
            decoder->seek(0, abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            SetFailure(result, L"early_seek_failed");
            return result;
        }
    }

    uint64_t samples = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t channel_mask = 0;
    bool have_audio = false;
    audio_chunk_impl chunk;
    try {
        while (decoder->run(chunk, abort)) {
            abort.check();
            const size_t count = chunk.get_sample_count();
            if (count == 0) continue;
            if (!have_audio) {
                result.first_decoded_ms = std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - decode_started)
                                              .count();
                result.first_decoded_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::system_clock::now().time_since_epoch())
                                                   .count();
                sample_rate = chunk.get_sample_rate();
                channels = chunk.get_channel_count();
                channel_mask = chunk.get_channel_config();
                if (sample_rate == 0 || channels == 0) {
                    SetFailure(result, L"invalid_audio_format");
                    return result;
                }
                if (request.atmos && (channels != ExpectedAtmosChannels(request.atmos_output) ||
                                      channel_mask != ExpectedAtmosChannelMask(request.atmos_output))) {
                    SetFailure(result, L"atmos_render_output_mismatch");
                    return result;
                }
                have_audio = true;
                result.channel_mask = channel_mask;
                if (request.atmos) result.atmos_rendered = true;
                if (request.cancel_after_first_chunk) {
                    result.sample_rate = sample_rate;
                    result.channels = channels;
                    result.samples = count;
                    result.decoded_seconds = static_cast<double>(count) / static_cast<double>(sample_rate);
                    const auto shutdown_started = std::chrono::steady_clock::now();
                    decoder.release();
                    result.shutdown_ms = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - shutdown_started)
                                             .count();
                    result.canceled_after_first_chunk = true;
                    if (*result.shutdown_ms >= 2000.0) {
                        SetFailure(result, L"shutdown_timeout");
                    } else {
                        result.ok = true;
                    }
                    return result;
                }
            } else if (chunk.get_sample_rate() != sample_rate || chunk.get_channel_count() != channels ||
                       chunk.get_channel_config() != channel_mask) {
                SetFailure(result, L"audio_format_changed");
                return result;
            }
            if (samples > std::numeric_limits<uint64_t>::max() - count) {
                SetFailure(result, L"sample_count_overflow");
                return result;
            }
            samples += count;
        }
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        SetFailure(result, L"decoder_read_failed");
        return result;
    }

    if (!have_audio || samples == 0 || sample_rate == 0) {
        SetFailure(result, L"no_decoded_audio");
        return result;
    }
    result.sample_rate = sample_rate;
    result.channels = channels;
    result.channel_mask = channel_mask;
    result.samples = samples;
    const double decoded = static_cast<double>(samples) / static_cast<double>(sample_rate);
    result.decoded_seconds = decoded;

    if (!result.codec) {
        SetFailure(result, L"codec_unavailable");
        return result;
    }
    if (request.atmos) {
        if (!Contains(*result.codec, L"E-AC-3") || !Contains(*result.codec, L"JOC")) {
            SetFailure(result, L"atmos_source_codec_mismatch");
            return result;
        }
        result.atmos_rendered = true;
    } else {
        if (!Contains(*result.codec, L"ALAC") && !Contains(*result.codec, L"AAC") &&
            !Contains(*result.codec, L"MP3")) {
            SetFailure(result, L"unexpected_codec");
            return result;
        }
    }
    if (Contains(*result.codec, L"ALAC") && !result.bit_depth) {
        SetFailure(result, L"bit_depth_unavailable");
        return result;
    }
    if (!result.declared_seconds) {
        SetFailure(result, L"declared_duration_unavailable");
        return result;
    }
    const double duration_tolerance =
        request.atmos ? kAtmosDurationToleranceSeconds : kDecoderPaddingToleranceSeconds;
    if (std::abs(decoded - *result.declared_seconds) > duration_tolerance) {
        SetFailure(result, L"duration_mismatch");
        return result;
    }

    try {
        result.can_seek = decoder->can_seek();
        if (!result.can_seek && request.requires_seek) {
            SetFailure(result, L"seek_unavailable");
            return result;
        }
        if (result.can_seek) {
            constexpr double seekRemainderSeconds = 1.0;
            const double target = std::max(0.0, decoded - seekRemainderSeconds);
            decoder->seek(target, abort);
            uint64_t remaining_samples = 0;
            bool read_after_seek = false;
            for (;;) {
                abort.check();
                if (!decoder->run(chunk, abort)) break;
                const size_t count = chunk.get_sample_count();
                if (count == 0) continue;
                if (chunk.get_sample_rate() != sample_rate || chunk.get_channel_count() != channels ||
                    chunk.get_channel_config() != channel_mask) {
                    SetFailure(result, L"seek_format_mismatch");
                    return result;
                }
                read_after_seek = true;
                if (remaining_samples > std::numeric_limits<uint64_t>::max() - count) {
                    SetFailure(result, L"sample_count_overflow");
                    return result;
                }
                remaining_samples += count;
                const double remaining = static_cast<double>(remaining_samples) / sample_rate;
                if (remaining > seekRemainderSeconds + kDecoderPaddingToleranceSeconds) {
                    SetFailure(result, L"seek_position_mismatch");
                    return result;
                }
            }
            if (!read_after_seek) {
                SetFailure(result, L"seek_failed");
                return result;
            }
            const double remaining = static_cast<double>(remaining_samples) / sample_rate;
            if (std::abs(remaining - seekRemainderSeconds) > kDecoderPaddingToleranceSeconds) {
                SetFailure(result, L"seek_position_mismatch");
                return result;
            }
            result.seek_verified = true;
        }
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        SetFailure(result, L"seek_failed");
        return result;
    }

    result.ok = true;
    return result;
}

JsonObject MakeMetadataJson(const MetadataSnapshot& metadata, bool include_identity) {
    JsonObject object;
    object.Insert(L"title", JsonValue::CreateStringValue(metadata.title));
    object.Insert(L"artist", JsonValue::CreateStringValue(metadata.artist));
    object.Insert(L"album", JsonValue::CreateStringValue(metadata.album));
    if (include_identity) {
        object.Insert(L"id", JsonValue::CreateStringValue(metadata.id));
        object.Insert(L"catalogId", JsonValue::CreateStringValue(metadata.catalog_id));
        object.Insert(L"libraryId", JsonValue::CreateStringValue(metadata.library_id));
        object.Insert(L"reportingId", JsonValue::CreateStringValue(metadata.reporting_id));
        object.Insert(L"durationMs", JsonValue::CreateNumberValue(metadata.duration_ms));
    }
    return object;
}

JsonObject MakeResultJson(const ProbeResult& result) {
    JsonObject object;
    object.Insert(L"ok", JsonValue::CreateBooleanValue(result.ok));
    object.Insert(L"codec",
                  result.codec ? JsonValue::CreateStringValue(*result.codec) : JsonValue::CreateNullValue());
    object.Insert(L"bitDepth", result.bit_depth
                                   ? JsonValue::CreateNumberValue(static_cast<double>(*result.bit_depth))
                                   : JsonValue::CreateNullValue());
    object.Insert(L"sampleRate", result.sample_rate
                                     ? JsonValue::CreateNumberValue(static_cast<double>(*result.sample_rate))
                                     : JsonValue::CreateNullValue());
    object.Insert(L"channels", result.channels
                                   ? JsonValue::CreateNumberValue(static_cast<double>(*result.channels))
                                   : JsonValue::CreateNullValue());
    object.Insert(L"samples", result.samples
                                  ? JsonValue::CreateNumberValue(static_cast<double>(*result.samples))
                                  : JsonValue::CreateNullValue());
    object.Insert(L"decodedSeconds", result.decoded_seconds
                                         ? JsonValue::CreateNumberValue(*result.decoded_seconds)
                                         : JsonValue::CreateNullValue());
    object.Insert(L"declaredSeconds", result.declared_seconds
                                          ? JsonValue::CreateNumberValue(*result.declared_seconds)
                                          : JsonValue::CreateNullValue());
    object.Insert(L"firstDecodedMs", result.first_decoded_ms
                                         ? JsonValue::CreateNumberValue(*result.first_decoded_ms)
                                         : JsonValue::CreateNullValue());
    object.Insert(L"firstDecodedUnixMs",
                  result.first_decoded_unix_ms
                      ? JsonValue::CreateNumberValue(static_cast<double>(*result.first_decoded_unix_ms))
                      : JsonValue::CreateNullValue());
    object.Insert(L"canSeek", JsonValue::CreateBooleanValue(result.can_seek));
    object.Insert(L"seekVerified", JsonValue::CreateBooleanValue(result.seek_verified));
    object.Insert(L"seekStressCount", JsonValue::CreateNumberValue(result.seek_stress_count));
    object.Insert(L"metadataVerified", JsonValue::CreateBooleanValue(result.metadata_verified));
    object.Insert(L"metadataIdentityVerified",
                  JsonValue::CreateBooleanValue(result.metadata_identity_verified));
    object.Insert(L"fixtureTransactionRestored",
                  JsonValue::CreateBooleanValue(result.fixture_transaction_restored));
    object.Insert(L"metadataInputPath", JsonValue::CreateStringValue(Wide(result.metadata_input_path)));
    const auto metadata = [&object](const wchar_t* key, const std::optional<MetadataSnapshot>& value,
                                    bool include_identity) {
        object.Insert(key, value ? IJsonValue(MakeMetadataJson(*value, include_identity))
                                 : JsonValue::CreateNullValue());
    };
    metadata(L"metadataSource", result.metadata_source, true);
    metadata(L"metadataDisplay", result.metadata_display, true);
    metadata(L"metadataGetInfo", result.metadata_get_info, false);
    metadata(L"metadataDynamicInfo", result.metadata_dynamic_info, false);
    object.Insert(L"canceledAfterFirstChunk",
                  JsonValue::CreateBooleanValue(result.canceled_after_first_chunk));
    object.Insert(L"shutdownMs", result.shutdown_ms ? JsonValue::CreateNumberValue(*result.shutdown_ms)
                                                    : JsonValue::CreateNullValue());
    object.Insert(L"atmosRendered", JsonValue::CreateBooleanValue(result.atmos_rendered));
    object.Insert(L"atmosObjects", result.atmos_objects ? JsonValue::CreateNumberValue(
                                                              static_cast<double>(*result.atmos_objects))
                                                        : JsonValue::CreateNullValue());
    object.Insert(L"renderOutput", result.render_output ? JsonValue::CreateStringValue(*result.render_output)
                                                        : JsonValue::CreateNullValue());
    object.Insert(L"channelMask", result.channel_mask ? JsonValue::CreateNumberValue(
                                                            static_cast<double>(*result.channel_mask))
                                                      : JsonValue::CreateNullValue());
    object.Insert(L"decoderError", JsonValue::CreateStringValue(result.decoder_error));
    object.Insert(L"error", result.error.empty() ? JsonValue::CreateNullValue()
                                                 : JsonValue::CreateStringValue(result.error));
    return object;
}

bool WriteAll(HANDLE file, std::string_view bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD remaining = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, remaining, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

bool WriteResult(const fs::path& result_path, const ProbeResult& result) {
    const fs::path temporary = result_path.wstring() + L".tmp";
    const std::string bytes = winrt::to_string(MakeResultJson(result).Stringify());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = WriteAll(file, bytes) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written || !MoveFileExW(temporary.c_str(), result_path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void RunProbe(const ProbePaths paths, std::shared_ptr<abort_callback_impl> abort) {
    SetThreadDescription(GetCurrentThread(), L"Glasswave Apple Music decoder probe");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    ProbeResult result;
    std::wstring error;
    const auto bytes = ReadBoundedText(paths.claimed, kMaxRequestBytes);
    DeleteFileW(paths.claimed.c_str());
    const auto profile = NativeProfileDirectory();
    std::optional<ProbeRequest> request;
    if (!bytes) {
        error = L"request_read_failed";
    } else if (!profile) {
        error = L"profile_path_unavailable";
    } else {
        request = ParseRequest(*bytes, *profile, error);
    }

    if (!request) {
        SetFailure(result, error.empty() ? L"invalid_request" : error);
    } else {
        const auto previousTransaction = ProbeTransactionTag();
        try {
            abort->check();
            std::optional<ScopedFixtureProbeTransaction> transaction;
            if (request->verify_metadata && request->input_path == Catalog::Get().Records().SongPath(stream::Id(kMetadataFixtureId)))
                transaction.emplace();
            if (request->atmos) {
                const ScopedAtmosPlaybackOverride preference(true, request->atmos_output);
                result = DecodeRequest(*request, *abort);
            } else {
                result = DecodeRequest(*request, *abort);
            }
        } catch (const exception_aborted&) {
            SetFailure(result, L"cancelled");
        } catch (...) {
            SetFailure(result, L"unexpected_failure");
        }
        if (request->verify_metadata && request->input_path == Catalog::Get().Records().SongPath(stream::Id(kMetadataFixtureId)))
            result.fixture_transaction_restored = ProbeTransactionTag() == previousTransaction;
    }
    WriteResult(paths.result, result);
    winrt::uninit_apartment();
}

class ProbeLifecycle : public initquit {
  public:
    ~ProbeLifecycle() { Stop(); }

    void on_init() override {
        const auto paths = GetProbePaths();
        if (!paths) return;
        std::error_code ec;
        if (!fs::is_regular_file(paths->request, ec) || ec) return;

        DeleteFileW(paths->result.c_str());
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return;
        }

        auto abort = std::make_shared<abort_callback_impl>();
        std::lock_guard lock(m_mutex);
        if (m_worker.joinable()) {
            if (m_abort) m_abort->set();
            m_worker.join();
        }
        m_abort = abort;
        m_worker = std::thread([paths = *paths, abort = std::move(abort)] { RunProbe(paths, abort); });
    }

    void on_quit() override { Stop(); }

  private:
    void Stop() noexcept {
        std::lock_guard lock(m_mutex);
        if (m_abort) m_abort->set();
        if (m_worker.joinable()) m_worker.join();
        m_abort.reset();
    }

    std::mutex m_mutex;
    std::shared_ptr<abort_callback_impl> m_abort;
    std::thread m_worker;
};

FB2K_SERVICE_FACTORY(ProbeLifecycle);

} // namespace
} // namespace glasswave::applemusic
