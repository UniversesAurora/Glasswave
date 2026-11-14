#include "pch.h"
#include "stream/playback_source.h"

#include <SDK/skipTrack.h>

namespace glasswave::stream {
namespace {

SourceAvailability g_availability;

// The core applies this predicate while choosing tracks, including native shuffle,
// repeat and its playback queue. Once an input confirms a missing source, subsequent
// selections skip it before input initialization. The core handles an exhausted list.
class UnavailableTrackFilter : public fb2k::skipTrack {
public:
    bool testTrack(const fb2k::skipTrackParam& item) override {
        return item.track.is_empty() || CanPlaySource(item.track->get_path());
    }
};
service_factory_single_t<UnavailableTrackFilter> g_filter;

class SourcePlaybackEvents : public play_callback_static {
public:
    unsigned get_flags() override { return flag_on_playback_starting | flag_on_playback_stop; }
    void on_playback_starting(play_control::t_track_command, bool) override { ResetSourceAvailability(); }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason != play_control::stop_reason_starting_another) ResetSourceAvailability();
    }
    void on_playback_new_track(metadb_handle_ptr) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};
play_callback_static_factory_t<SourcePlaybackEvents> g_playbackEvents;

std::function<void(const std::string&)>& OpeningListener() {  // main thread only
    static std::function<void(const std::string&)> listener;
    return listener;
}

}  // namespace

void AnnouncePlaybackOpening(std::string_view path, unsigned flags) {
    if ((flags & input_flag_playback) == 0 || (flags & input_flag_testing_integrity) != 0) return;
    fb2k::inMainThread([path = std::string(path)] {
        if (auto& listener = OpeningListener()) listener(path);
    });
}

void SetPlaybackOpeningListener(std::function<void(const std::string& path)> listener) {
    OpeningListener() = std::move(listener);
}

uint64_t SourcePlaybackEpoch() { return g_availability.Epoch(); }

void ResetSourceAvailability() { g_availability.Reset(); }

bool CanPlaySource(std::string_view path) { return g_availability.CanPlay(path); }

bool SkipUnavailableSource(uint64_t epoch, std::string_view path, std::wstring_view reason,
                           unsigned flags, abort_callback& abort) {
    abort.check();
    if ((flags & input_flag_playback) == 0 || (flags & input_flag_testing_integrity) != 0) return false;
    g_availability.MarkUnavailable(epoch, path);
    const std::string location(path);
    const auto message = pfc::stringcvt::string_utf8_from_wide(reason.data(), reason.size());
    console::print("Glasswave: skipped unavailable stream ", location.c_str(), " (", message.get_ptr(), ")");
    return true;
}

}  // namespace glasswave::stream
