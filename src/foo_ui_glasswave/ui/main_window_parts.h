#pragma once

// What MainWindow's source files share (ui/main_window*.cpp only): the includes, the
// namespace aliases, widget and timer ids, layout constants, settings and helpers.

#include <filesystem>
#include <fstream>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <dbt.h>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "ui/main_window.h"
#include "gfx/motion.h"
#include "download/downloads.h"
#include "ui/downloads_page.h"
#include "ui/home_page.h"
#include "media/radio.h"
#include "media/transport.h"
#include "core/log.h"
#include "core/text.h"
#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "gfx/composition_host.h"
#include "media/source_identity.h"
#include "ui/caption_buttons.h"
#include "ui/main_menu.h"
#include "ui/shell_glue.h"
#include "ui/taskbar.h"
#include "ui/win7_menu.h"
#include "ui/i18n.h"
#include "media/favorites.h"
#include "cd/playback.h"
#include "cd/discs.h"
#include "media/play_stats.h"
#include "media/special_playlists.h"
#include "media/queue_order.h"
#include "media/playback_scheduler.h"
#include "stream/playback_source.h"
#include "stream/provider.h"
#include "ui/win32.h"
#include "ui/art_cache.h"
#include "ui/cover_tint.h"
#include "ui/pane_common.h"
#include "ui/text_prompt.h"
#include "ui/lyrics_search.h"
#include "ui/lyrics_tools.h"
#include "gfx/effects.h"
#include "lyrics/scripts.h"
#include "app/settings.h"
#include "ui/remote_pages.h"
#include "ui/recent_page.h"
#include "ui/search_page.h"
#include "media/recent_lists.h"
#include "ui/search_popup.h"
#include "ui/song_actions.h"
#include "ui/song_table.h"
#include "ui/text.h"
#include "wmp/wmp_painter.h"
#include "wmp/wmp_transport.h"
#include "win7/win7_resources.h"

namespace glasswave::ui {

using core::Wide;

namespace m = aero::metrics;
namespace paint = aero::paint;
namespace wp = wmp::paint;
namespace wt = wmp::transport;
namespace wuc = winrt::Windows::UI::Composition;
using paint::Layer;
using paint::TransportGlyph;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace mainwin {

constexpr wchar_t kAppName[] = L"Glasswave";

constexpr UINT_PTR kPaneRevealTimer = 5;

constexpr UINT_PTR kDetailTipTimer = 6;

constexpr UINT_PTR kArtHoldTimer = 8;  // a new song's cover kept it waiting long enough

constexpr UINT kFrameMessage = WM_APP + 0x32; // FrameClock: the lyrics pane draws a frame

constexpr float kLyricsPaneWidth = 440.0f;    // DIP (new UI, no Win7 measurement)

enum WidgetId {
    kCapMin = 1,
    kCapMax,
    kCapClose,
    kBack,
    kForward,
    kOrganize,
    kAddFiles,
    kAddFolder,
    kNewPlaylist,
    kSeek,
    kShuffle,
    kRepeat,
    kStop,
    kPrev,
    kNext,
    kPlay,
    kMute,
    kVolume,
    kViewOptions,
    kDetailAction,
    kDetailBriefToggle,
    kRefresh,
    kDownloadAll,
    kLocalPlay, // Play on a local album's or playlist's command bar
    kLyricsToggle,
    kVolArrow, // a narrow window's arrow beside the speaker: the volume flyout
    kCmdMore,  // the command bar's » for the buttons that no longer fit
};

// Detail band above a provider's artist, album and playlist pages: a fixed band above the list (new UI, no
// Win7 measurement).
constexpr float kDetailHeaderHeight = 104, kDetailArt = 80, kDetailActionWidth = 88, kDetailActionHeight = 26;

constexpr float kDetailBriefMaxHeight = 260.0f, kDetailScrollWidth = 17.0f;

// Music categories shown in the navigation pane, in order: media::GroupKind values as digits.
// {6A1F3C5D-2B7E-4D90-8C15-3E9A7B2D4F61}
extern cfg_var_modern::cfg_string cfg_navCategories;

// Where the Smart playlists section sits among the Local Music categories (0 = first).
// {6A1F3C64-2B7E-4D90-8C15-3E9A7B2D4F61}
extern cfg_var_modern::cfg_int cfg_navSmartAt;

// The lyrics pane is open.
// {6A1F3C68-2B7E-4D90-8C15-3E9A7B2D4F61}
extern cfg_var_modern::cfg_int cfg_lyricsOpen;

inline D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f,
                        a);
}

inline std::optional<stream::NavigationSection> FindProviderSection(const stream::Provider& provider,
                                                             const stream::Id& id) {
    if (id.empty()) return std::nullopt;
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(),
                                    [&id](const auto& section) { return section.id == id; });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

inline std::optional<stream::NavigationSection> FindProviderSection(
    const stream::Provider& provider, stream::SectionKind kind,
    std::optional<stream::Shelf> shelf = std::nullopt) {
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(), [kind, shelf](const auto& section) {
        return section.kind == kind &&
               (!shelf || (section.kind == stream::SectionKind::PlaylistShelf && section.shelf == *shelf));
    });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

// The section a page stands in. A section's own pages are in it; an album, an artist or a
// playlist only when it is really there (saved, followed, the user's): otherwise it stands right
// under its source (owner, 2026-11-21: an artist page read 唱片集 > ..., a playlist from Home
// "my playlists"). Where a page was opened from is View::from's business, not this one's.
inline std::optional<stream::NavigationSection> SectionForView(const stream::Provider& provider, const View& view) {
    switch (view.kind) {
    case View::Kind::RemoteArtist:
    case View::Kind::RemoteTopTracks: {
        const bool followed = provider.ArtistPreferenceState(view.remoteId).value_or([&] {
            const auto artists = provider.FollowedArtists();
            return std::any_of(artists.begin(), artists.end(),
                               [&view](const stream::Artist& a) { return a.id == view.remoteId; });
        }());
        return followed ? FindProviderSection(provider, stream::SectionKind::Artists) : std::nullopt;
    }
    case View::Kind::RemoteAlbum:
        return provider.IsAlbumSaved(view.remoteId) ? FindProviderSection(provider, stream::SectionKind::Albums)
                                                    : std::nullopt;
    case View::Kind::RemotePlaylist:
        return provider.SectionOfPlaylist(view.remoteId);
    default:
        break;
    }
    if (const auto section = FindProviderSection(provider, view.sectionId)) return section;
    switch (view.kind) {
    case View::Kind::RemoteFavorites:
        return FindProviderSection(provider, stream::SectionKind::Favorites);
    case View::Kind::RemoteArtists:
        return FindProviderSection(provider, stream::SectionKind::Artists);
    case View::Kind::RemoteAlbums:
        return FindProviderSection(provider, stream::SectionKind::Albums);
    case View::Kind::RemoteShelf:
        return FindProviderSection(provider, stream::SectionKind::PlaylistShelf, view.shelf);
    case View::Kind::RemoteSectionTracks:
        return FindProviderSection(provider, stream::SectionKind::Tracks);
    case View::Kind::RemoteGroups:
    case View::Kind::RemoteGroupTracks:
        return FindProviderSection(provider, stream::SectionKind::Groups);
    default:
        return std::nullopt;
    }
}

inline View ProviderLandingView(const stream::Provider& provider) {
    const auto sections = provider.Sections();
    for (const auto& section : sections)
        if (!section.id.empty()) return SectionView(provider, section);
    return RemoteView(provider, View::Kind::RemoteFavorites);
}

// A section's own pages need it; an album, an artist or a playlist stands without one (right
// under its source when it is not in the library, SectionForView).
inline bool SectionBackedView(View::Kind kind) {
    switch (kind) {
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteShelf:
    case View::Kind::RemoteArtists:
    case View::Kind::RemoteAlbums:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
    case View::Kind::RemoteGroupTracks:
        return true;
    default:
        return false;
    }
}

inline View ProviderGroupsFallback(const stream::Provider& provider, const stream::Id& preferred) {
    if (const auto section = FindProviderSection(provider, preferred);
        section && section->kind == stream::SectionKind::Groups)
        return SectionView(provider, *section);
    if (const auto section = FindProviderSection(provider, stream::SectionKind::Groups))
        return SectionView(provider, *section);
    return ProviderLandingView(provider);
}

// Repairs a view against the provider's current section snapshot. Group ids that came from
// localized service data are not stable, so a language/account transition falls back to the
// provider's Groups page instead of opening a track page with an obsolete group id.
inline View NormalizeRemoteView(const stream::Provider& provider, View view, bool discardGroupTracks) {
    if (!view.IsRemote() || view.provider != provider.Key()) return view;
    if (view.kind == View::Kind::Search) return view; // its sectionId is the tab, not a section
    const bool sectionValid =
        !view.sectionId.empty() && FindProviderSection(provider, view.sectionId).has_value();
    if (!sectionValid) view.sectionId.clear();
    if (view.kind == View::Kind::RemoteGroupTracks &&
        (discardGroupTracks || !sectionValid || view.groupId.empty()))
        return ProviderGroupsFallback(provider, view.sectionId);
    if (sectionValid || view.sectionId.empty()) {
        if (const auto section = SectionForView(provider, view)) view.sectionId = section->id;
    }
    if (SectionBackedView(view.kind) && view.sectionId.empty()) return ProviderLandingView(provider);
    if (const auto section = FindProviderSection(provider, view.sectionId)) {
        switch (view.kind) {
        case View::Kind::RemoteFavorites:
        case View::Kind::RemoteShelf:
        case View::Kind::RemoteArtists:
        case View::Kind::RemoteAlbums:
        case View::Kind::RemoteSectionTracks:
        case View::Kind::RemoteGroups:
            view.name = section->label;
            break;
        default:
            break;
        }
    }
    if (view.kind == View::Kind::RemotePlaylist) {
        if (const auto playlist = provider.FindPlaylist(view.remoteId); playlist && !playlist->name.empty())
            view.name = playlist->name;
    } else if (view.kind == View::Kind::RemoteAlbum) {
        if (const auto album = provider.FindAlbum(view.remoteId); album && !album->name.empty())
            view.name = album->name;
    }
    if (view.kind == View::Kind::RemoteArtist || view.kind == View::Kind::RemoteTopTracks) {
        for (const auto& artist : provider.FollowedArtists())
            if (artist.id == view.remoteId && !artist.name.empty()) {
                view.name = artist.name;
                break;
            }
    }
    return view;
}

inline com_ptr<ID2D1LinearGradientBrush> VGrad(ID2D1DeviceContext* dc, float y0, float y1,
                                        std::initializer_list<std::pair<float, uint32_t>> stops) {
    std::vector<D2D1_GRADIENT_STOP> v;
    for (auto& [p, c] : stops)
        v.push_back({p, Hex(c)});
    com_ptr<ID2D1GradientStopCollection> coll;
    winrt::check_hresult(
        dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), coll.put()));
    com_ptr<ID2D1LinearGradientBrush> b;
    winrt::check_hresult(dc->CreateLinearGradientBrush({{0, y0}, {0, y1}}, coll.get(), b.put()));
    return b;
}

inline void Place(const wuc::Visual& v, const RECT& r) {
    v.Offset(float3{static_cast<float>(r.left), static_cast<float>(r.top), 0});
    v.Size(float2{static_cast<float>(std::max<LONG>(r.right - r.left, 0)),
                  static_cast<float>(std::max<LONG>(r.bottom - r.top, 0))});
}

// Windows Media Player's own icon from the user's wmploc.dll (icon group 102), else
// foobar2000's.
constexpr int kWmpIcon = 102;

inline HICON CreateAppIcon(int sizePx) {
    if (HICON icon = win7::Resources::Get().Icon(win7::Module::Wmploc, kWmpIcon, sizePx)) return icon;
    return ui_control::get()->load_main_icon(sizePx, sizePx);
}

// WMP's large icon frames carry uneven transparent margins (the 64 px one shows 50x46 px,
// low and left), so the taskbar drew it small and off-centre. This builds a big icon
// from the 256 px frame cropped to its visible part, centred and scaled to `sizePx`.
inline HICON CreateCenteredAppIcon(int sizePx) {
    HICON big = win7::Resources::Get().Icon(win7::Module::Wmploc, kWmpIcon, 256);
    if (!big) return CreateAppIcon(sizePx);
    auto* wic = gfx::CompositionHost::Get().Wic();
    com_ptr<IWICBitmap> src;
    HICON result = nullptr;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(big, src.put()))) {
        UINT w = 0, h = 0;
        src->GetSize(&w, &h);
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        src->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data());
        UINT l = w, t = h, r = 0, b = 0;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > 8) {
                    l = std::min(l, x), t = std::min(t, y), r = std::max(r, x), b = std::max(b, y);
                }
        if (r > l && b > t) {
            com_ptr<IWICBitmapClipper> clip;
            com_ptr<IWICBitmapScaler> scaler;
            const UINT cw = r - l + 1, ch = b - t + 1, side = std::max(cw, ch);
            const UINT dw = std::max<UINT>(1, sizePx * cw / side), dh = std::max<UINT>(1, sizePx * ch / side);
            const WICRect rc{static_cast<INT>(l), static_cast<INT>(t), static_cast<INT>(cw),
                             static_cast<INT>(ch)};
            if (SUCCEEDED(wic->CreateBitmapClipper(clip.put())) &&
                SUCCEEDED(clip->Initialize(src.get(), &rc)) &&
                SUCCEEDED(wic->CreateBitmapScaler(scaler.put())) &&
                SUCCEEDED(
                    scaler->Initialize(clip.get(), dw, dh, WICBitmapInterpolationModeHighQualityCubic))) {
                std::vector<BYTE> scaled(static_cast<size_t>(dw) * dh * 4);
                scaler->CopyPixels(nullptr, dw * 4, static_cast<UINT>(scaled.size()), scaled.data());
                // Into a centred square 32-bpp DIB (BGRA, straight alpha as icons expect).
                BITMAPV5HEADER bh{};
                bh.bV5Size = sizeof(bh);
                bh.bV5Width = sizePx;
                bh.bV5Height = -sizePx;
                bh.bV5Planes = 1;
                bh.bV5BitCount = 32;
                bh.bV5Compression = BI_BITFIELDS;
                bh.bV5RedMask = 0x00FF0000, bh.bV5GreenMask = 0x0000FF00, bh.bV5BlueMask = 0x000000FF,
                bh.bV5AlphaMask = 0xFF000000;
                void* bits = nullptr;
                HDC screen = GetDC(nullptr);
                HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS,
                                                 &bits, nullptr, 0);
                ReleaseDC(nullptr, screen);
                if (color) {
                    auto* out = static_cast<BYTE*>(bits);
                    std::memset(out, 0, static_cast<size_t>(sizePx) * sizePx * 4);
                    const UINT ox = (sizePx - dw) / 2, oy = (sizePx - dh) / 2;
                    for (UINT y = 0; y < dh; ++y)
                        std::memcpy(out + ((static_cast<size_t>(y) + oy) * sizePx + ox) * 4,
                                    scaled.data() + static_cast<size_t>(y) * dw * 4,
                                    static_cast<size_t>(dw) * 4);
                    HBITMAP mask = CreateBitmap(sizePx, sizePx, 1, 1, nullptr);
                    ICONINFO ii{TRUE, 0, 0, mask, color};
                    result = CreateIconIndirect(&ii);
                    DeleteObject(mask);
                    DeleteObject(color);
                }
            }
        }
    }
    DestroyIcon(big);
    return result ? result : CreateAppIcon(sizePx);
}

inline float DbFromSlider(float v) {
    return v <= 0.01f ? -100.0f : std::max(-100.0f, 40.0f * std::log10(v));
}

} // namespace mainwin

} // namespace glasswave::ui
