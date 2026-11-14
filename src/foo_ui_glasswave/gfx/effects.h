#pragma once
// Direct2D effects described for Windows.UI.Composition effect brushes (no Win2D).

namespace glasswave::gfx {

namespace wge = winrt::Windows::Graphics::Effects;

// Gaussian blur; its standard deviation (pixels) is animatable as "<name>.BlurAmount".
wge::IGraphicsEffect MakeGaussianBlurEffect(const winrt::hstring& name, float blurAmount,
                                            const wge::IGraphicsEffectSource& source);

// `source` with its alpha multiplied by `mask`'s alpha.
wge::IGraphicsEffect MakeAlphaMaskEffect(const wge::IGraphicsEffectSource& source,
                                         const wge::IGraphicsEffectSource& mask);

// Whatever of our own window lies behind the visual it paints, blurred by `blurPx` (standard
// deviation in px): frosted glass for small panes inside the window (the volume bubble). Null
// if unavailable.
winrt::Windows::UI::Composition::CompositionBrush MakeFrostBrush(const winrt::Windows::UI::Composition::Compositor& comp,
                                                                 float blurPx);

// Clear glass in the manner of iOS's materials and Win7's frame: what lies behind, blurred by
// `blurPx`, its colours deepened (`saturation`, 1 = unchanged), then mixed with `tint` by the
// tint's alpha. Not frosted: no noise, no milky haze unless the tint brings it. With `mask`
// the glass takes that brush's alpha (its shape). Falls back to MakeFrostBrush.
winrt::Windows::UI::Composition::CompositionBrush MakeGlassBrush(const winrt::Windows::UI::Composition::Compositor& comp,
                                                                 float blurPx, float saturation, D2D1_COLOR_F tint,
                                                                 const winrt::Windows::UI::Composition::CompositionBrush& mask = nullptr);

}  // namespace glasswave::gfx
