#pragma once
#include "aero/aero_colorization.h"

// Aero glass material scoped to our own window:
//   HostBackdropBrush (DWM's live blur of whatever is behind this window)
//     -> ColorMatrix effect (Win7 colorization recipe, see aero_colorization.h)
//     + screen-anchored reflection streaks
// Falls back to an opaque "Aero Basic" style color when Windows transparency
// effects are turned off.

namespace glasswave::aero {

class GlassMaterial {
public:
    // dwmArt: use Win7's own reflection texture and top-left glare (aero_atlas.h).
    GlassMaterial(const Colorization& colorization, bool dwmArt);
    void SetScale(float scale);  // DPI change: repaint the glare

    const winrt::Windows::UI::Composition::ContainerVisual& Root() const { return m_root; }

    void SetActive(bool active);
    // Lighter blur (experimental): DWM's legacy blur-behind accent on `hwnd` with a fitted
    // translucent overlay (FitLegacyOverlay) instead of the host backdrop and color matrix.
    void SetLegacyBlur(HWND hwnd, bool legacy);
    bool LegacyBlur() const { return m_legacy; }
    void SetColorization(const Colorization& colorization);
    // Call when the system "Transparency effects" setting may have changed.
    void RefreshTransparencyPolicy();
    // Streaks are fixed to screen coordinates: moving the window only shifts an offset.
    void UpdateReflectionAnchor(HWND hwnd);
    // Same, for a window about to move to `windowRect` (WM_WINDOWPOSCHANGING). Applying
    // the offset before the move lets DWM show both in the same frame; doing it after
    // the move (WM_MOVE) lags a frame behind and makes the streaks shake while dragging.
    void UpdateReflectionAnchor(HWND hwnd, const RECT& windowRect);

private:
    void RebuildBrushes();
    void ApplyBrush();

    Colorization m_colorization;
    bool m_active = true;
    bool m_transparency = true;
    bool m_dwmArt = false;
    bool m_legacy = false;
    HWND m_hwnd = nullptr;
    float m_scale = 1.0f;
    RECT m_reflectionMonitor{};

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_glass{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_reflection{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_glare{nullptr}, m_glareInactive{nullptr};
    winrt::Windows::UI::Composition::CompositionBrush m_activeBrush{nullptr};
    winrt::Windows::UI::Composition::CompositionBrush m_inactiveBrush{nullptr};
    winrt::Windows::UI::Composition::CompositionBrush m_basicActive{nullptr};
    winrt::Windows::UI::Composition::CompositionBrush m_basicInactive{nullptr};
    winrt::Windows::UI::Composition::CompositionBrush m_legacyActive{nullptr};
    winrt::Windows::UI::Composition::CompositionBrush m_legacyInactive{nullptr};
    void ApplyAccent();
};

}  // namespace glasswave::aero
