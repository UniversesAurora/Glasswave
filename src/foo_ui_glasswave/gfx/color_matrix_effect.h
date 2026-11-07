#pragma once
// Win2D-free description of the D2D ColorMatrix effect for Windows.UI.Composition effect brushes.
// Composition reads the effect CLSID, properties and sources through IGraphicsEffectD2D1Interop.

namespace glasswave::gfx {

// matrix: 20 floats, row-major D2D1_MATRIX_5X4_F; source: usually a CompositionEffectSourceParameter.
winrt::Windows::Graphics::Effects::IGraphicsEffect MakeColorMatrixEffect(
    const std::array<float, 20>& matrix, const winrt::Windows::Graphics::Effects::IGraphicsEffectSource& source);

}  // namespace glasswave::gfx
