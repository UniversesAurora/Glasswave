#pragma once
// Windows 7 Aero window and WMP 12 layout metrics, in DIP (pixels at 96 DPI).
// Measured from native Win7 screenshots; see docs/design/visual-spec.md.

namespace glasswave::aero::metrics {

// Window frame: outer line 1 + highlight 1 + glass 4 + client-edge light 1 + dark 1.
inline constexpr float kResizeBorder = 8.0f;
inline constexpr float kCornerRadius = 6.0f;    // all four corners
inline constexpr float kCaptionHeight = 30.0f;  // outer edge to client edge (restored)
inline constexpr float kCaptionHeightMax = 22.0f;

// Caption buttons (minimize / maximize / close cluster)
inline constexpr float kCapBtnMinW = 26.0f;
inline constexpr float kCapBtnMaxW = 27.0f;
inline constexpr float kCapBtnCloseW = 47.0f;
inline constexpr float kCapBtnH = 20.0f;
inline constexpr float kCapBtnRightInset = 6.0f;  // from the window's outer edge
inline constexpr float kCapBtnTopInset = 1.0f;

// Caption text and icon
inline constexpr float kTitleFontSize = 12.0f;  // Segoe UI 9pt
inline constexpr float kIconSize = 16.0f;

// WMP 12 library-mode client layout (measured at 120% DPI, converted to DIP)
inline constexpr float kNavBarHeight = 29.0f;      // back/forward + breadcrumb row
inline constexpr float kCommandBarHeight = 28.0f;  // Organize / Stream / Create playlist row
inline constexpr float kTransportHeight = 45.0f;   // light transport band at the bottom

// Transport capsule; x offsets are relative to the play orb's center.
inline constexpr float kCapsuleWidth = 333.0f;
inline constexpr float kCapsuleHeight = 26.0f;
inline constexpr float kOrbDiameter = 36.0f;  // plus a 1-DIP white halo ring on each side
inline constexpr float kSegmentHeight = 21.0f;
inline constexpr float kSegmentReach = 60.0f;  // orb center to the segment's outer end
inline constexpr float kIconButtonW = 24.0f;
inline constexpr float kIconButtonH = 20.0f;
inline constexpr float kShuffleX = -141.0f;
inline constexpr float kRepeatX = -116.0f;
inline constexpr float kDividerX = -97.0f;
inline constexpr float kStopX = -78.0f;
inline constexpr float kVolumeX = 78.0f;
inline constexpr float kSliderLeft = 92.0f;
inline constexpr float kSliderRight = 156.0f;

// Animation timings (ms): Win7 hover fades in fast and out slowly.
inline constexpr int kHoverInMs = 120;
inline constexpr int kHoverOutMs = 420;
inline constexpr int kPressMs = 60;
inline constexpr int kPulsePeriodMs = 1800;  // WMC-style "breathing" focus glow

}  // namespace glasswave::aero::metrics
