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

}  // namespace glasswave::gfx
