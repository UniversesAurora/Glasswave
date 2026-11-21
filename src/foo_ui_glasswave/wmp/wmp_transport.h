#pragma once
#include "aero/aero_painter.h"

// WMP 12 transport controls drawn from the original wmploc.dll artwork (ADR 0007).
//
// Every position is in bitmap pixels (= DIP) relative to the origin of the transport
// cluster bitmap (wmploc 257/4153, 182x51: stop plate, previous segment, play orb, next
// segment). The offsets were found by matching the bitmaps against a native WMP 12
// capture pixel for pixel (docs/design/win7-assets.md).

namespace glasswave::wmp::transport {

using aero::paint::Layer;

// Resource IDs (wmploc.dll, type 257)
enum Id : int {
    kCluster = 4153, kClusterPressed = 4154, kClusterHover = 4155,
    kGlowRing = 4160,
    kPlayGlyph = 4162, kPlayGlyphDisabled = 4163, kPauseGlyph = 4164, kPauseGlyphDisabled = 4165,
    kNextGlyph = 4166, kNextGlyphDisabled = 4167, kPrevGlyph = 4168, kPrevGlyphDisabled = 4169,
    kShuffleOn = 4174, kShuffleOff = 4175, kRepeatOn = 4176, kRepeatOff = 4177,
    kStopGlyph = 4179, kStopGlyphDisabled = 4180,
    kCapsule = 2164,
    kMuted = 5244, kVolume0 = 5245, kVolume1 = 5246, kVolume2 = 5247,
    kVolumeFill = 5678, kVolumeThumb = 5679, kVolumeThumbPressed = 5680, kVolumeThumbHover = 5681,
    kNavArrows = 5350, kNavArrowsPressed = 5351, kNavArrowsHover = 5352, kNavArrowsDisabled = 5353,
};

// Regions relative to the cluster origin (DIP).
inline constexpr D2D1_RECT_F kClusterRect{0, 0, 182, 51};
inline constexpr D2D1_RECT_F kCapsuleRect{-171, -9, -171 + 558, -9 + 68};
inline constexpr D2D1_RECT_F kCapsuleVisible{-93, 1, 309, 49};  // opaque part of the capsule bitmap
inline constexpr D2D1_RECT_F kStopRect{2, 13, 27, 38};
inline constexpr D2D1_RECT_F kPrevRect{36, 13, 108, 38};
inline constexpr D2D1_RECT_F kNextRect{108, 13, 180, 38};
inline constexpr D2D1_RECT_F kOrbRect{86, 4, 130, 48};  // circle, center (108, 26), r 22
inline constexpr D2D1_RECT_F kShuffleRect{-74, 13, -49, 38};
inline constexpr D2D1_RECT_F kRepeatRect{-43, 13, -18, 38};
inline constexpr D2D1_RECT_F kMuteRect{192, 13, 217, 38};
inline constexpr D2D1_RECT_F kVolumeRect{222, 13, 302, 36};  // fill bitmap; thumb travels inside
inline constexpr float kThumbMin = 233, kThumbMax = 288;     // thumb center range (cluster x)
inline constexpr float kGlowFactor = 52.0f / 44.0f;          // glow ring size / orb size
inline constexpr float kBandHeight = 56;                      // transport band, top rule included
inline constexpr float kClusterTop = 5;                       // band top to cluster origin

// Navigation bar back / forward orbs (5350..5353 hold both, 27x27 each) and the recess they sit in
// (5360, 56x29, orbs at (1, 1)). Drawn at the command bar's button height (owner: the 27 px
// bitmaps at one per DIP looked too big next to the text).
inline constexpr float kNavOrb = 27;
inline constexpr int kNavGroove = 5360;
inline constexpr D2D1_SIZE_F kNavGrooveSize{56, 29};
inline constexpr float kNavOrbShown = 22;  // DIP

bool Available();  // wmploc.dll artwork present

enum class Button { Stop, Prev, Next };
// Cluster button whose widget covers `region` (cluster coordinates).
void ClusterButton(ID2D1DeviceContext* dc, D2D1_RECT_F region, Button button, Layer layer, bool enabled);
void Orb(ID2D1DeviceContext* dc, Layer layer, bool pause);
void GlowRing(ID2D1DeviceContext* dc, float size);
// The cluster's play orb on its own, scaled into `box` (DIP): a cover's "play all" button.
// hover 0..1 fades in the hover look; opacity fades the whole button.
// pressed 0..1: the cluster's pressed look (wmploc 4154) over it, while held.
void PlayOrb(ID2D1DeviceContext* dc, D2D1_RECT_F box, float hover, float opacity, float pressed = 0);
void Toggle(ID2D1DeviceContext* dc, Layer layer, bool shuffle, bool on);
// "Recommend less" (NetEase's 不感兴趣) in the shuffle toggle's place while a station plays: a
// heart struck through, drawn as WMP 12's toggle glyphs are (wmploc 4174 / 4175: #0066CC with a
// white halo, a #63CFFC glow behind it when lit). 25 x 25 DIP.
void Dislike(ID2D1DeviceContext* dc, Layer layer);
void Mute(ID2D1DeviceContext* dc, Layer layer, bool muted, float volume);
void Volume(ID2D1DeviceContext* dc, Layer layer, float value);
void ClusterBackground(ID2D1DeviceContext* dc);
void Capsule(ID2D1DeviceContext* dc);
// The capsule with its ends moved in for a narrow window (WMP 12 shortens it the same way):
// its left end at cluster x `left` (normally kCapsuleVisible.left), its right end at `right`,
// drawn as two parts of the bitmap that meet under the cluster. Same surface as Capsule().
void CapsuleSpan(ID2D1DeviceContext* dc, float left, float right);

// A vertical gradient with one stop per row: at each row's centre (smooth), or rows kept as
// crisp bands.
winrt::com_ptr<ID2D1LinearGradientBrush> RowBands(ID2D1DeviceContext* dc, float y0, float y1,
                                                  std::initializer_list<D2D1_COLOR_F> rows, bool smooth);
// WMP 12's split volume button (wmploc 4626 - 4630; 39x25 units of `k` DIP: the speaker's 25,
// the arrow's 14): one half of plate `id` at x, and the arrow in vectors.
void SplitPlate(ID2D1DeviceContext* dc, int id, bool arrowHalf, float x, float k);
void SplitArrow(ID2D1DeviceContext* dc, float k);
// WMP 12's volume flyout frame (glass rims, a frosted inside) filling `size`; returns the
// inside, where its slider goes. Lay it over a blurred copy of what is behind.
D2D1_RECT_F FlyoutFrame(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
void NavOrb(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, bool back, bool enabled);
void NavGroove(ID2D1DeviceContext* dc, D2D1_SIZE_F size);

}  // namespace glasswave::wmp::transport
