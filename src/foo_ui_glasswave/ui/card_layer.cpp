#include "pch.h"
#include "ui/card_layer.h"

#include "cache/images.h"
#include "gfx/composition_host.h"
#include "net/http.h"
#include "netease/log.h"
#include "ui/cover_tint.h"
#include "ui/shell_glue.h"
#include "ui/text.h"
#include "wmp/wmp_transport.h"

#include <SDK/album_art_helpers.h>

#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>

#include <numbers>

namespace glasswave::ui {

namespace {

namespace wuc = winrt::Windows::UI::Composition;
namespace playback = winrt::Windows::Media::Playback;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

// Material Design 1's card elevations (owner, 2026-11-15: "follow Material Design 1's card, raised
// about half as high as the first try"): resting at 2 dp, raised to 8 dp under the mouse, 4 dp
// held down. Each elevation is two shadows, as Material draws them: a key light from above (short,
// darker, offset down) and an ambient light (wide, faint). The card also rises 3 DIP and grows 1.5 %.
// DIP and opacities; new UI values (Windows 7 has no such cards).
struct Elevation {
    float rise, scale;
    float keyY, keyBlur, keyOpacity;
    float ambientY, ambientBlur, ambientOpacity;
};
constexpr Elevation kRest{0, 1.0f, 1.5f, 3, 0.26f, 0.5f, 6, 0.12f};
constexpr Elevation kRaised{3, 1.015f, 6, 10, 0.24f, 2.5f, 16, 0.16f};
constexpr Elevation kPressed{1, 1.005f, 3, 5, 0.25f, 1, 9, 0.14f};
constexpr int kRaiseMs = 240, kSettleMs = 320, kPressMs = 100;
constexpr int kVideoFadeMs = 700;
constexpr float kPad = 16;          // DIP: the words from the card's edges
constexpr float kSmoothing = 0.6f;  // the corners' smoothing (0: circular arcs; Apple's icons use 0.6)

D2D1_COLOR_F Color(uint32_t rgb, float alpha = 1.0f) {
    return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
}

float Luminance(uint32_t rgb) {
    return (0.2126f * ((rgb >> 16) & 0xFF) + 0.7152f * ((rgb >> 8) & 0xFF) + 0.0722f * (rgb & 0xFF)) / 255.0f;
}

uint32_t Mix(uint32_t a, uint32_t b, float t) {
    const auto channel = [&](int shift) {
        const float x = ((a >> shift) & 0xFF) * (1 - t) + ((b >> shift) & 0xFF) * t;
        return static_cast<uint32_t>(std::lround(std::clamp(x, 0.0f, 255.0f))) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

winrt::com_ptr<ID2D1SolidColorBrush> Solid(ID2D1DeviceContext* dc, uint32_t rgb, float alpha = 1.0f) {
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(Color(rgb, alpha), brush.put());
    return brush;
}

// The card's outline with continuous-curvature corners (Figma's corner smoothing): each corner is a
// short circular arc between two Bezier curves that ease the curvature in from the straight edge,
// so no joint shows where the curve begins, as a plain rounded rectangle's does.
winrt::com_ptr<ID2D1PathGeometry> CardShape(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float radius) {
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return nullptr;
    const float r = radius, xi = kSmoothing;
    const float deg = std::numbers::pi_v<float> / 180;
    const float p = (1 + xi) * r;
    const float arcMeasure = 90 * (1 - xi);
    const float arc = std::sin(arcMeasure / 2 * deg) * r * std::numbers::sqrt2_v<float>;
    const float alpha = (90 - arcMeasure) / 2;
    const float p3p4 = r * std::tan(alpha / 2 * deg);
    const float beta = 45 * xi;
    const float c = p3p4 * std::cos(beta * deg);
    const float d = c * std::tan(beta * deg);
    const float b = (p - arc - c - d) / 3;
    const float a = 2 * b;
    // The corners clockwise from the top right; u runs along the edge into the corner, v inwards
    // along the next edge.
    struct Corner {
        D2D1_POINT_2F start, u, v;
    };
    const float w = size.width, h = size.height;
    const Corner corners[] = {{{w - p, 0}, {1, 0}, {0, 1}},
                              {{w, h - p}, {0, 1}, {-1, 0}},
                              {{p, h}, {-1, 0}, {0, -1}},
                              {{0, p}, {0, -1}, {1, 0}}};
    const auto at = [](const Corner& k, D2D1_POINT_2F from, float along, float in) {
        return D2D1_POINT_2F{from.x + k.u.x * along + k.v.x * in, from.y + k.u.y * along + k.v.y * in};
    };
    sink->BeginFigure(corners[0].start, D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 0; i < 4; ++i) {
        const Corner& k = corners[i];
        sink->AddLine(k.start);
        sink->AddBezier({at(k, k.start, a, 0), at(k, k.start, a + b, 0), at(k, k.start, a + b + c, d)});
        const D2D1_POINT_2F arcStart = at(k, k.start, a + b + c, d);
        const D2D1_POINT_2F arcEnd = at(k, arcStart, arc, arc);
        sink->AddArc({arcEnd, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        sink->AddBezier({at(k, arcEnd, d, c), at(k, arcEnd, d, b + c), at(k, arcEnd, d, a + b + c)});
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

// A vertical gradient of one colour between alphas.
winrt::com_ptr<ID2D1LinearGradientBrush> Fade(ID2D1DeviceContext* dc, uint32_t rgb, float top, float bottom,
                                              std::initializer_list<std::pair<float, float>> stops) {
    std::vector<D2D1_GRADIENT_STOP> list;
    for (const auto& [where, alpha] : stops) list.push_back({where, Color(rgb, alpha)});
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1LinearGradientBrush> brush;
    dc->CreateGradientStopCollection(list.data(), static_cast<UINT32>(list.size()), collection.put());
    if (collection) dc->CreateLinearGradientBrush({{0, top}, {0, bottom}}, collection.get(), brush.put());
    return brush;
}

// The image cache's name for a picture: its address without scheme and host (CDN paths, not
// signed); the source key scopes it.
std::string PictureId(const std::wstring& url) {
    std::wstring_view path = url;
    if (const size_t scheme = path.find(L"://"); scheme != std::wstring_view::npos) {
        path.remove_prefix(scheme + 3);
        if (const size_t slash = path.find(L'/'); slash != std::wstring_view::npos) path.remove_prefix(slash);
    }
    return Utf8(path);
}

// The H.264 rendition of a motion cover's HLS master playlist closest above `widthPx` (Windows
// decodes H.264 everywhere; HEVC needs an extension). Empty: none.
std::wstring PickVariant(const std::wstring& master, int widthPx, abort_callback& abort) {
    net::HttpOptions options;
    options.timeoutMs = 15000;
    options.maxBodyBytes = 512u * 1024u;
    const net::HttpResponse response = net::SendHttp(master, options, abort);
    if (response.status != 200) return {};
    std::istringstream lines(response.body);
    std::string line, info;
    std::wstring best, largest;
    int bestWidth = 0, largestWidth = 0;
    long long bestBandwidth = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            info = line;
            continue;
        }
        if (info.empty() || line.empty() || line[0] == '#') continue;
        const std::string attributes = std::exchange(info, {});
        if (attributes.find("CODECS=\"avc1") == std::string::npos) continue;
        int w = 0, h = 0;
        if (const size_t pos = attributes.find("RESOLUTION="); pos != std::string::npos)
            sscanf_s(attributes.c_str() + pos + 11, "%dx%d", &w, &h);
        long long bandwidth = 0;
        if (const size_t pos = attributes.find("BANDWIDTH="); pos != std::string::npos)
            bandwidth = _atoi64(attributes.c_str() + pos + 10);
        std::wstring url = Wide(line.c_str());
        if (url.find(L"://") == std::wstring::npos) // relative to the master playlist
            url = master.substr(0, master.rfind(L'/') + 1) + url;
        if (w > largestWidth) {
            largestWidth = w;
            largest = url;
        }
        // The smallest that covers the card; of equal sizes the lighter one.
        if (w >= widthPx && (best.empty() || w < bestWidth || (w == bestWidth && bandwidth < bestBandwidth))) {
            best = url;
            bestWidth = w;
            bestBandwidth = bandwidth;
        }
    }
    return best.empty() ? largest : best;
}

bool ClientAreaAnimations() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

// The card's colours: the source's, else from the picture (its vivid hue, deepened so white words
// read on it), else a neutral slate while the picture loads.
void ChooseColors(const media::CardInfo& info, IWICBitmapSource* picture, uint32_t& background, uint32_t& titleInk,
                  uint32_t& textInk) {
    if (info.colors) {
        background = info.background;
        titleInk = info.titleInk;
        textInk = info.textInk;
        return;
    }
    background = 0x3A3F47;
    if (picture)
        if (const auto tint = CoverTint(picture)) background = Mix(*tint, 0x000000, 0.38f);
    const bool light = Luminance(background) > 0.6f;
    titleInk = light ? 0x111111 : 0xFFFFFF;
    textInk = light ? 0x333333 : 0xF2F2F2;
}

} // namespace

// A shelf's strip: `holder` stands at the shelf's height, `scroller` inside it moves sideways.
struct CardLayer::Strip {
    wuc::ContainerVisual holder{nullptr}, scroller{nullptr};
    float target = 0; // px, the scroller's x when no slide runs
    ULONGLONG slideEnd = 0;
};

struct CardLayer::Card {
    media::CardInfo info;
    std::wstring strip;                  // the shelf it stands in
    wuc::ContainerVisual slot{nullptr};  // its place along the strip
    wuc::ContainerVisual lift{nullptr};  // the hover transform
    wuc::SpriteVisual ambient{nullptr};  // casts the ambient shadow (the card's shape, its colour)
    wuc::SpriteVisual face{nullptr};     // background and picture; casts the key shadow
    wuc::DropShadow keyShadow{nullptr}, ambientShadow{nullptr};
    wuc::SpriteVisual video{nullptr};    // the motion cover over the picture
    wuc::SpriteVisual words{nullptr};    // the scrim and the words over both
    wuc::SpriteVisual play{nullptr}, playLit{nullptr}, playHeld{nullptr};
    gfx::RetainedSurface ambientSurface, faceSurface, wordsSurface, playSurface, playLitSurface, playHeldSurface;
    float paintedScale = 0;

    winrt::com_ptr<IWICBitmapSource> picture;
    bool loading = false, failed = false;
    uint32_t background = 0x3A3F47, titleInk = 0xFFFFFF, textInk = 0xFFFFFF;

    playback::MediaPlayer player{nullptr};
    playback::MediaPlayerSurface playerSurface{nullptr};
    bool variantAsked = false;
    std::wstring variant; // the rendition to play; empty: not known yet (or none)
    bool videoShown = false;

    bool shown = false, inView = false;
};

CardLayer::CardLayer(const wuc::ContainerVisual& parent, const wuc::Visual& above) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    m_root.Clip(comp.CreateInsetClip());
    parent.Children().InsertBelow(m_root, above);
    m_animations = ClientAreaAnimations();
}

CardLayer::~CardLayer() {
    for (auto& [key, card] : m_cards) StopVideo(*card);
}

void CardLayer::SetBounds(const RECT& px, float scale) {
    const bool rescaled = scale != m_scale;
    m_scale = scale;
    const float w = static_cast<float>(std::max<LONG>(px.right - px.left, 0));
    m_viewWidth = w / scale;
    m_root.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    m_root.Size(float2{w, static_cast<float>(std::max<LONG>(px.bottom - px.top, 0))});
    if (rescaled)
        for (auto& [key, card] : m_cards) {
            if (card->shown) Paint(*card);
            ApplyState(*card, false);
        }
}

// The card's outline in white, at the card's size: the video's mask.
wuc::CompositionSurfaceBrush CardLayer::ShapeBrush() {
    auto& host = gfx::CompositionHost::Get();
    if (!m_shape || m_shapeScale != m_scale) {
        m_shape = host.CreateSurface(Px(kCard.width), Px(kCard.height));
        m_shapeScale = m_scale;
        host.Draw(m_shape, m_scale, [&](ID2D1DeviceContext* dc) {
            if (auto shape = CardShape(dc, kCard, kCorner)) dc->FillGeometry(shape.get(), Solid(dc, 0xFFFFFF).get());
        });
    }
    auto brush = host.Compositor().CreateSurfaceBrush(m_shape);
    brush.Stretch(wuc::CompositionStretch::Fill);
    return brush;
}

CardLayer::Card& CardLayer::CardFor(const media::CardInfo& info) {
    auto& slot = m_cards[info.key];
    if (slot) { // the source read again: what changed is repainted, reloaded or restarted
        Card& card = *slot;
        const media::CardInfo& old = card.info;
        const bool newPicture = old.imageUrl != info.imageUrl;
        const bool newLook = newPicture || old.title != info.title || old.label != info.label ||
                             old.subtitle != info.subtitle || old.badge != info.badge || old.colors != info.colors ||
                             old.background != info.background || old.titleInk != info.titleInk ||
                             old.textInk != info.textInk || old.imageTall != info.imageTall;
        if (old.videoUrl != info.videoUrl) {
            StopVideo(card);
            card.variantAsked = false;
            card.variant.clear();
        }
        card.info = info;
        if (newPicture) {
            card.picture = nullptr;
            card.failed = false;
        }
        if (newLook) card.paintedScale = 0;
        return card;
    }
    slot = std::make_unique<Card>();
    Card& card = *slot;
    card.info = info;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    card.slot = comp.CreateContainerVisual();
    card.lift = comp.CreateContainerVisual();
    card.ambient = comp.CreateSpriteVisual();
    card.face = comp.CreateSpriteVisual();
    card.video = comp.CreateSpriteVisual();
    card.words = comp.CreateSpriteVisual();
    card.play = comp.CreateSpriteVisual();
    card.playLit = comp.CreateSpriteVisual();
    card.playHeld = comp.CreateSpriteVisual();
    card.video.Opacity(0);
    card.play.Opacity(0);
    card.playLit.Opacity(0);
    card.playHeld.Opacity(0);
    for (auto* shadow : {&card.keyShadow, &card.ambientShadow}) {
        *shadow = comp.CreateDropShadow();
        shadow->Color(winrt::Windows::UI::Color{255, 0, 0, 0});
    }
    card.ambient.Shadow(card.ambientShadow);
    card.face.Shadow(card.keyShadow);
    for (const auto& v : {card.ambient, card.face, card.video, card.words, card.play, card.playLit, card.playHeld})
        card.lift.Children().InsertAtTop(v);
    card.slot.Children().InsertAtTop(card.lift);
    card.slot.IsVisible(false);
    ApplyState(card, false);
    return card;
}

CardLayer::Card* CardLayer::Find(const media::ListSource* source, size_t row) {
    const media::CardInfo* info = source && row != SIZE_MAX ? source->Card(row) : nullptr;
    if (!info) return nullptr;
    const auto it = m_cards.find(info->key);
    return it == m_cards.end() ? nullptr : it->second.get();
}

void CardLayer::Layout(const media::ListSource& source, const std::vector<Shelf>& shelves, int slideMs) {
    const ULONGLONG now = GetTickCount64();
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    std::set<std::wstring> stripsShown, placed;
    for (const auto& shelf : shelves) {
        auto& owned = m_strips[shelf.key];
        if (!owned) {
            owned = std::make_unique<Strip>();
            owned->holder = comp.CreateContainerVisual();
            owned->scroller = comp.CreateContainerVisual();
            owned->holder.Children().InsertAtTop(owned->scroller);
            m_root.Children().InsertAtTop(owned->holder);
            owned->target = std::numeric_limits<float>::quiet_NaN();
        }
        Strip& strip = *owned;
        stripsShown.insert(shelf.key);
        strip.holder.IsVisible(true);
        strip.holder.Offset(float3{0, std::round(shelf.top * m_scale), 0});
        const float target = -std::round(shelf.scroll * m_scale);
        if (shelf.slideFrom && slideMs > 0 && m_animations) {
            // Paging: the whole strip slides, as a scroll would (Windows 8.1's fast-out curve).
            strip.scroller.StopAnimation(L"Offset");
            strip.scroller.Offset(float3{-std::round(*shelf.slideFrom * m_scale), 0, 0});
            auto slide = comp.CreateVector3KeyFrameAnimation();
            slide.InsertKeyFrame(1.0f, float3{target, 0, 0},
                                 comp.CreateCubicBezierEasingFunction({0.1f, 0.9f}, {0.2f, 1.0f}));
            slide.Duration(std::chrono::milliseconds(slideMs));
            strip.scroller.StartAnimation(L"Offset", slide);
            strip.slideEnd = now + static_cast<ULONGLONG>(slideMs);
        } else if (target != strip.target || now >= strip.slideEnd) {
            if (now < strip.slideEnd) strip.scroller.StopAnimation(L"Offset");
            strip.slideEnd = 0;
            strip.scroller.Offset(float3{target, 0, 0});
        }
        strip.target = target;
        const float from = shelf.slideFrom ? *shelf.slideFrom : shelf.scroll;
        for (const auto& [row, x] : shelf.cards) {
            const media::CardInfo* info = source.Card(row);
            if (!info || !placed.insert(info->key).second) continue;
            Card& card = CardFor(*info);
            if (card.strip != shelf.key) {
                if (auto parent = card.slot.Parent()) parent.Children().Remove(card.slot);
                strip.scroller.Children().InsertAtTop(card.slot);
                card.strip = shelf.key;
            }
            card.slot.Offset(float3{std::round(x * m_scale), 0, 0});
            if (!card.shown) {
                card.shown = true;
                card.slot.IsVisible(true);
            }
            // On screen where the strip stands or slides from.
            const auto visibleAt = [&](float scroll) { return x - scroll < m_viewWidth && x - scroll + kCard.width > 0; };
            card.inView = visibleAt(shelf.scroll) || visibleAt(from);
            if (card.paintedScale != m_scale) Paint(card);
            if (!card.picture && !card.loading && !card.failed) LoadPicture(card);
            UpdateVideo(card);
        }
    }
    for (auto& [key, strip] : m_strips)
        if (!stripsShown.count(key)) strip->holder.IsVisible(false);
    for (auto& [key, card] : m_cards) {
        if (placed.count(key) || !card->shown) continue;
        card->shown = false;
        card->inView = false;
        card->slot.IsVisible(false);
        StopVideo(*card);
    }
}

void CardLayer::Clear() {
    for (auto& [key, card] : m_cards) StopVideo(*card);
    for (auto& [key, strip] : m_strips) m_root.Children().Remove(strip->holder);
    m_cards.clear();
    m_strips.clear();
    m_hot.clear();
    m_pressed.clear();
}

// The raised card over its neighbours, so its shadow falls on them.
void CardLayer::Raise(Card& card) {
    if (auto parent = card.slot.Parent()) {
        parent.Children().Remove(card.slot);
        parent.Children().InsertAtTop(card.slot);
    }
}

void CardLayer::SetHot(const media::ListSource* source, size_t row, bool onPlay) {
    Card* card = Find(source, row);
    const std::wstring key = card ? card->info.key : std::wstring();
    if (key == m_hot && (card != nullptr && onPlay) == m_hotPlay) return;
    const std::wstring old = std::exchange(m_hot, key);
    m_hotPlay = card && onPlay;
    if (!old.empty() && old != key)
        if (auto it = m_cards.find(old); it != m_cards.end()) ApplyState(*it->second, true);
    if (card) {
        if (old != key) Raise(*card);
        ApplyState(*card, true);
    }
}

void CardLayer::SetPressed(const media::ListSource* source, size_t row) {
    Card* card = Find(source, row);
    const std::wstring key = card ? card->info.key : std::wstring();
    if (key == m_pressed) return;
    const std::wstring old = std::exchange(m_pressed, key);
    if (!old.empty())
        if (auto it = m_cards.find(old); it != m_cards.end()) ApplyState(*it->second, true);
    if (card) ApplyState(*card, true);
}

void CardLayer::SetMotion(bool on) {
    if (on == m_motion) return;
    m_motion = on;
    for (auto& [key, card] : m_cards) UpdateVideo(*card);
}

// The card's elevation for its state, animated on the compositor (or set at once).
void CardLayer::ApplyState(Card& card, bool animate) {
    const bool hot = card.info.key == m_hot, pressed = card.info.key == m_pressed;
    const Elevation& e = pressed && hot ? kPressed : hot ? kRaised : kRest;
    const int ms = pressed && hot ? kPressMs : hot ? kRaiseMs : kSettleMs;
    const float w = static_cast<float>(Px(kCard.width)), h = static_cast<float>(Px(kCard.height));
    card.lift.CenterPoint(float3{w / 2, h / 2, 0});
    const float s = m_scale;
    const float3 offset{0, -std::round(e.rise * s), 0};
    const float3 scale{e.scale, e.scale, 1};
    const bool orb = hot && card.info.playable;
    const float playOpacity = orb ? 1.0f : 0.0f, litOpacity = orb && m_hotPlay ? 1.0f : 0.0f;
    const float heldOpacity = orb && m_hotPlay && pressed ? 1.0f : 0.0f;
    struct ShadowState {
        wuc::DropShadow& shadow;
        float y, blur, opacity;
    };
    const ShadowState shadows[] = {{card.keyShadow, e.keyY * s, e.keyBlur * s, e.keyOpacity},
                                   {card.ambientShadow, e.ambientY * s, e.ambientBlur * s, e.ambientOpacity}};
    if (!animate || !m_animations) {
        for (const auto* name : {L"Offset", L"Scale"}) card.lift.StopAnimation(name);
        card.lift.Offset(offset);
        card.lift.Scale(scale);
        for (const auto& [shadow, y, blur, opacity] : shadows) {
            for (const auto* name : {L"Offset", L"BlurRadius", L"Opacity"}) shadow.StopAnimation(name);
            shadow.Offset(float3{0, y, 0});
            shadow.BlurRadius(blur);
            shadow.Opacity(opacity);
        }
        for (const auto& v : {card.play, card.playLit, card.playHeld}) v.StopAnimation(L"Opacity");
        card.play.Opacity(playOpacity);
        card.playLit.Opacity(litOpacity);
        card.playHeld.Opacity(heldOpacity);
        return;
    }
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    // Material's standard curve: out of rest quickly, settling softly.
    const auto ease = comp.CreateCubicBezierEasingFunction({0.4f, 0.0f}, {0.2f, 1.0f});
    const auto vector = [&](const auto& target, const wchar_t* property, float3 value) {
        auto a = comp.CreateVector3KeyFrameAnimation();
        a.InsertKeyFrame(1.0f, value, ease);
        a.Duration(std::chrono::milliseconds(ms));
        target.StartAnimation(property, a);
    };
    const auto scalar = [&](const auto& target, const wchar_t* property, float value, int duration) {
        auto a = comp.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(1.0f, value, ease);
        a.Duration(std::chrono::milliseconds(duration));
        target.StartAnimation(property, a);
    };
    vector(card.lift, L"Offset", offset);
    vector(card.lift, L"Scale", scale);
    for (const auto& [shadow, y, blur, opacity] : shadows) {
        vector(shadow, L"Offset", float3{0, y, 0});
        scalar(shadow, L"BlurRadius", blur, ms);
        scalar(shadow, L"Opacity", opacity, ms);
    }
    scalar(card.play, L"Opacity", playOpacity, hot ? 160 : 260);
    scalar(card.playLit, L"Opacity", litOpacity, m_hotPlay ? 140 : 260);
    scalar(card.playHeld, L"Opacity", heldOpacity, heldOpacity > 0 ? 60 : 200);
}

void CardLayer::Paint(Card& card) {
    card.paintedScale = m_scale;
    const int w = Px(kCard.width), h = Px(kCard.height);
    for (const auto& v : {card.ambient, card.face, card.video, card.words})
        v.Size(float2{static_cast<float>(w), static_cast<float>(h)});
    if (card.playerSurface) card.player.SetSurfaceSize({static_cast<float>(w), static_cast<float>(h)});
    if (card.player)
        if (auto mask = card.video.Brush().try_as<wuc::CompositionMaskBrush>()) mask.Mask(ShapeBrush());
    PaintFace(card);
    PaintWords(card);
    // WMP's play orb in the corner, lit under the mouse.
    auto& host = gfx::CompositionHost::Get();
    const int p = Px(kPlay);
    const float px = static_cast<float>(Px(kCard.width - kPlayInset - kPlay)),
                py = static_cast<float>(Px(kCard.height - kPlayInset - kPlay));
    for (int look = 0; look < 3; ++look) { // at rest, lit, held
        auto& v = look == 0 ? card.play : look == 1 ? card.playLit : card.playHeld;
        auto& surface = look == 0 ? card.playSurface : look == 1 ? card.playLitSurface : card.playHeldSurface;
        host.Repaint(v, surface, p, p, m_scale, [&](ID2D1DeviceContext* dc) {
            wmp::transport::PlayOrb(dc, {0, 0, kPlay, kPlay}, look ? 1.0f : 0.0f, 1.0f, look == 2 ? 1.0f : 0.0f);
        });
        v.Offset(float3{px, py, 0});
    }
    ApplyState(card, false);
}

void CardLayer::PaintFace(Card& card) {
    ChooseColors(card.info, card.picture.get(), card.background, card.titleInk, card.textInk);
    auto& host = gfx::CompositionHost::Get();
    const int w = Px(kCard.width), h = Px(kCard.height);
    // The ambient shadow's caster: the card's shape in its colour, hidden under the face.
    host.Repaint(card.ambient, card.ambientSurface, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (auto shape = CardShape(dc, kCard, kCorner)) dc->FillGeometry(shape.get(), Solid(dc, card.background).get());
    });
    host.Repaint(card.face, card.faceSurface, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size = kCard;
        auto shape = CardShape(dc, size, kCorner);
        if (!shape) return;
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.get()), nullptr);
        dc->Clear(Color(card.background));
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        if (card.picture) dc->CreateBitmapFromWicBitmap(card.picture.get(), nullptr, bitmap.put());
        if (bitmap) {
            const D2D1_SIZE_F b = bitmap->GetSize();
            if (card.info.imageTall) {
                // Filling the card, cropped to its shape about the centre.
                const float k = std::max(size.width / b.width, size.height / b.height);
                const float cw = size.width / k, ch = size.height / k;
                const D2D1_RECT_F src{(b.width - cw) / 2, (b.height - ch) / 2, (b.width + cw) / 2, (b.height + ch) / 2};
                dc->DrawBitmap(bitmap.get(), {0, 0, size.width, size.height}, 1.0f,
                               D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src);
            } else {
                // A square cover across the top, melting into the card's colour below it.
                const float side = size.width;
                const float k = std::max(side / b.width, side / b.height);
                const float cw = side / k, ch = side / k;
                const D2D1_RECT_F src{(b.width - cw) / 2, (b.height - ch) / 2, (b.width + cw) / 2, (b.height + ch) / 2};
                dc->DrawBitmap(bitmap.get(), {0, 0, side, side}, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                               &src);
                if (auto melt = Fade(dc, card.background, side * 0.55f, side,
                                     {{0.0f, 0.0f}, {0.55f, 0.55f}, {1.0f, 1.0f}}))
                    dc->FillRectangle({0, side * 0.55f, side, side + 1}, melt.get());
            }
        } else {
            // Loading: the colour with a soft light from the top.
            if (auto light = Fade(dc, 0xFFFFFF, 0, size.height, {{0.0f, 0.10f}, {1.0f, 0.0f}}))
                dc->FillRectangle({0, 0, size.width, size.height}, light.get());
        }
        // A short mark drawn big on the picture (NetEase's daily date), as its client does.
        if (!card.info.badge.empty()) {
            auto day = MakeText(card.info.badge, 52, DWRITE_FONT_WEIGHT_SEMI_BOLD, size.width, 70);
            dc->DrawTextLayout({kPad + 1, 10 + 1.5f}, day.get(), Solid(dc, 0x000000, 0.25f).get());
            dc->DrawTextLayout({kPad, 10}, day.get(), Solid(dc, 0xFFFFFF).get());
        }
        // A hairline of light just inside the edge, as a glossy print catches it: the outline
        // stroked twice as wide, its outer half cut off by the shape.
        const float line = std::max(1.0f, std::floor(m_scale)) / m_scale;
        dc->DrawGeometry(shape.get(), Solid(dc, 0xFFFFFF, 0.10f).get(), 2 * line);
        dc->PopLayer();
    });
    card.keyShadow.Mask(card.face.Brush());
    card.ambientShadow.Mask(card.ambient.Brush());
}

void CardLayer::PaintWords(Card& card) {
    auto& host = gfx::CompositionHost::Get();
    const int w = Px(kCard.width), h = Px(kCard.height);
    host.Repaint(card.words, card.wordsSurface, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size = kCard;
        const float textW = size.width - 2 * kPad - kPlay + 6; // beside the play orb
        auto subtitle = card.info.subtitle.empty()
                            ? nullptr
                            : MakeWrappedText(card.info.subtitle, 13, DWRITE_FONT_WEIGHT_NORMAL, textW, 36);
        auto title = card.info.title.empty()
                         ? nullptr
                         : MakeWrappedText(card.info.title, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, textW, 40);
        auto label = card.info.label.empty()
                         ? nullptr
                         : MakeText(card.info.label, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, textW, 18);
        const auto height = [](IDWriteTextLayout* layout) {
            DWRITE_TEXT_METRICS m{};
            if (layout) layout->GetMetrics(&m);
            return layout ? m.height : 0.0f;
        };
        float y = size.height - kPad;
        const float subtitleH = height(subtitle.get()), titleH = height(title.get()), labelH = height(label.get());
        const float top = y - subtitleH - titleH - labelH - 2;
        // The scrim: the card's colour rising from the bottom behind the words, so they read on
        // any picture (Apple's cards do the same with the artwork's own background colour).
        const float scrimTop = std::max(0.0f, top - 70);
        if (auto scrim = Fade(dc, card.background, scrimTop, size.height,
                              {{0.0f, 0.0f}, {0.45f, card.info.imageTall ? 0.55f : 0.35f}, {1.0f, 0.92f}}))
            if (auto shape = CardShape(dc, size, kCorner)) dc->FillGeometry(shape.get(), scrim.get());
        if (subtitle) {
            y -= subtitleH;
            dc->DrawTextLayout({kPad, y}, subtitle.get(), Solid(dc, card.textInk, 0.88f).get());
        }
        if (title) {
            y -= titleH;
            dc->DrawTextLayout({kPad, y}, title.get(), Solid(dc, card.titleInk).get());
        }
        if (label) {
            y -= labelH + 2;
            dc->DrawTextLayout({kPad, y}, label.get(), Solid(dc, card.textInk, 0.62f).get());
        }
    });
}

// The picture, through the persistent image cache (one download for every run), decoded off
// the UI thread at the card's size.
void CardLayer::LoadPicture(Card& card) {
    if (card.info.imageUrl.empty()) {
        card.failed = true;
        return;
    }
    card.loading = true;
    const int sizePx = Px(std::max(kCard.width, kCard.height)) + 64;
    cache::ImageRequest request;
    request.identity = {Utf8(card.info.provider), "home-card", PictureId(card.info.imageUrl), "v1"};
    request.resolve = [url = card.info.imageUrl](abort_callback&, unsigned) {
        return std::optional<cache::HttpSource>(cache::HttpSource{url, {}});
    };
    fb2k::splitTask([alive = std::weak_ptr<int>(m_alive), this, key = card.info.key, url = card.info.imageUrl,
                     request = std::move(request), sizePx]() mutable {
        winrt::com_ptr<IWICBitmapSource> picture;
        try {
            abort_callback_dummy abort;
            if (auto* images = cache::Images::Existing())
                if (auto result = images->Open(std::move(request), abort); result && !result->bytes.empty())
                    picture = DecodeArt(album_art_data_impl::g_create(result->bytes.data(), result->bytes.size()),
                                        sizePx);
        } catch (const std::exception&) {
        }
        fb2k::inMainThread([alive, this, key, url, picture]() {
            if (alive.expired()) return;
            const auto it = m_cards.find(key);
            if (it == m_cards.end() || it->second->info.imageUrl != url) return;
            Card& card = *it->second;
            card.loading = false;
            card.picture = picture;
            card.failed = !picture;
            if (!picture) {
                netease::Log(L"home: no picture for card " + key);
                return;
            }
            PaintFace(card);
            PaintWords(card); // the colours may come from the picture
        });
    });
}

// A motion cover plays while its card is on screen and the window can be seen; otherwise its
// player is closed (a decoder per card would hold memory for nothing).
void CardLayer::UpdateVideo(Card& card) {
    const bool want = card.shown && card.inView && m_motion && m_animations && !card.info.videoUrl.empty();
    if (!want) {
        StopVideo(card);
        return;
    }
    if (card.player) return;
    if (!card.variantAsked) {
        card.variantAsked = true;
        fb2k::splitTask([alive = std::weak_ptr<int>(m_alive), this, key = card.info.key, master = card.info.videoUrl,
                         widthPx = Px(kCard.width)] {
            std::wstring variant;
            try {
                abort_callback_dummy abort;
                variant = PickVariant(master, widthPx, abort);
            } catch (const std::exception&) {
            }
            fb2k::inMainThread([alive, this, key, master, variant] {
                if (alive.expired()) return;
                const auto it = m_cards.find(key);
                if (it == m_cards.end() || it->second->info.videoUrl != master) return;
                it->second->variant = variant;
                if (!variant.empty()) UpdateVideo(*it->second);
            });
        });
        return;
    }
    if (card.variant.empty()) return;
    try {
        const auto& comp = gfx::CompositionHost::Get().Compositor();
        playback::MediaPlayer player;
        // Never the system's media controls or media keys: those belong to foobar2000.
        player.CommandManager().IsEnabled(false);
        player.IsMuted(true);
        player.IsLoopingEnabled(true);
        player.AutoPlay(true);
        player.Source(winrt::Windows::Media::Core::MediaSource::CreateFromUri(
            winrt::Windows::Foundation::Uri(card.variant)));
        card.playerSurface = player.GetSurface(comp);
        player.SetSurfaceSize({static_cast<float>(Px(kCard.width)), static_cast<float>(Px(kCard.height))});
        auto frames = comp.CreateSurfaceBrush(card.playerSurface.CompositionSurface());
        frames.Stretch(wuc::CompositionStretch::UniformToFill);
        // Cut to the card's own outline, as the picture under it is.
        auto masked = comp.CreateMaskBrush();
        masked.Source(frames);
        masked.Mask(ShapeBrush());
        card.video.Brush(masked);
        card.video.Opacity(0);
        card.videoShown = false;
        // Shown once the video moves: when it has just opened, its surface may hold no frame yet
        // (leftover pixels), which fading in showed for a moment.
        player.PlaybackSession().PositionChanged([alive = std::weak_ptr<int>(m_alive), this, key = card.info.key,
                                                  moved = std::make_shared<std::atomic_bool>(false)](
                                                     const playback::MediaPlaybackSession& session, auto&&) {
            if (session.Position().count() <= 0 || moved->exchange(true)) return; // once
            fb2k::inMainThread([alive, this, key] {
                if (alive.expired()) return;
                const auto it = m_cards.find(key);
                if (it == m_cards.end() || !it->second->player || it->second->videoShown) return;
                Card& c = *it->second;
                c.videoShown = true;
                gfx::CompositionHost::Get().AnimateOpacity(c.video, 1.0f, kVideoFadeMs);
            });
        });
        player.MediaFailed([key = card.info.key](auto&&, const playback::MediaPlayerFailedEventArgs& args) {
            netease::Log(L"home: motion cover of " + key + L" failed: " + std::wstring(args.ErrorMessage()) +
                         L" (" + std::to_wstring(static_cast<int>(args.Error())) + L")");
        });
        card.player = player;
    } catch (const winrt::hresult_error&) {
        card.player = nullptr;
        card.playerSurface = nullptr;
        card.variant.clear(); // keep the picture
    }
}

void CardLayer::StopVideo(Card& card) {
    if (!card.player) return;
    try {
        card.player.Close();
    } catch (const winrt::hresult_error&) {
    }
    card.player = nullptr;
    card.playerSurface = nullptr;
    card.video.Brush(nullptr);
    card.video.StopAnimation(L"Opacity");
    card.video.Opacity(0);
    card.videoShown = false;
}

} // namespace glasswave::ui
