#pragma once
// Procedural Direct2D rendering of Windows 7 / WMP 12 controls.
//
// Colors, opacities, line widths and geometry come from pixel measurements of
// native Win7 screenshots (see docs/design/visual-spec.md). No bitmaps are
// extracted from Windows themes or WMP (docs/adr/0005). Everything is drawn in
// DIP with hairlines snapped to whole device pixels, so the result stays crisp
// at any DPI instead of reproducing the blur/jaggies of upscaled 96-DPI art.

namespace glasswave::aero::paint {

enum class Layer { Normal, Hover, Pressed, Inactive };
enum class CaptionGlyph { Minimize, Maximize, Restore, Close };
enum class TransportGlyph { Play, Pause, Stop, Previous, Next, Shuffle, Repeat, Volume, Mute };

// Caption button; first/last mark the ends of the cluster (rounded bottom corners).
void CaptionButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, CaptionGlyph glyph, Layer layer, bool first,
                   bool last);

// Window frame (nine-grid source): outer dark line + inner highlight, rounded corners.
void FrameOutline(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float cornerRadius);

// Client edge (nine-grid source): light line + dark line, then the light client background.
void ClientEdge(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
// Win7's faint cyan glow just inside the client edge (hollow nine-grid overlay).
void ClientGlow(ID2D1DeviceContext* dc, D2D1_SIZE_F size);

// Screen-anchored glass reflection streaks (drawn at low resolution, stretched).
void Reflection(ID2D1DeviceContext* dc, D2D1_SIZE_F size);

// Win7 "glow" text on glass: a faint soft halo behind dark text. `origin` must
// leave room for the halo (GlowPadding) or it gets clipped by the surface.
void GlowText(ID2D1DeviceContext* dc, IDWriteTextLayout* layout, D2D1_POINT_2F origin, D2D1_COLOR_F textColor);
constexpr float kGlowPadding = 10.0f;

// ---- WMP 12 transport ---------------------------------------------------------------
// The capsule that frames the whole control group (nine-grid source).
void TransportCapsule(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
// Round play/pause button, including its 1-DIP white halo ring (size = diameter + 2).
void PlayOrb(ID2D1DeviceContext* dc, float size, Layer layer, TransportGlyph glyph);
// Hover glow around the orb (carrier of the WMC-style breathing animation).
void OrbHalo(ID2D1DeviceContext* dc, float size);
// Silver previous/next segment; its inner end is hidden under the orb.
void Segment(ID2D1DeviceContext* dc, D2D1_SIZE_F size, bool left, Layer layer, TransportGlyph glyph, bool enabled);
// Flat icon button (stop, shuffle, repeat, volume) with WMP's hover plate.
void IconButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, TransportGlyph glyph, bool enabled,
                bool checked);
// IconButton's plate alone (hover / pressed / checked), and the brush its glyphs use.
void ButtonPlate(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, bool checked);
winrt::com_ptr<ID2D1Brush> TransportGlyphBrush(ID2D1DeviceContext* dc, float top, float bottom, bool enabled,
                                               Layer layer);
// Vertical divider inside the capsule.
void CapsuleDivider(ID2D1DeviceContext* dc, D2D1_SIZE_F size);
// Volume slider: track + blue thumb. value in [0, 1].
void VolumeSlider(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float value, Layer layer);

// Bitmap icon (WIC -> D2D).
void Icon(ID2D1DeviceContext* dc, IWICBitmapSource* icon, D2D1_RECT_F dest);

}  // namespace glasswave::aero::paint
