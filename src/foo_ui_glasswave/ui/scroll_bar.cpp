#include "pch.h"
#include "ui/scroll_bar.h"

namespace glasswave::ui {

namespace {
constexpr float kFadeStep = 0.12f; // a look's change per animation tick
} // namespace

// The thumb is the view's share of the content, never shorter than minThumb nor longer than
// the track between the arrows.
bool ScrollBarModel::Thumb(const Geometry& g, float* top, float* height) {
    const float track = g.length - g.arrow * 2;
    const float max = g.MaxScroll();
    if (max <= 0.5f || track <= 0 || g.content <= 0) return false;
    *height = std::min(track, std::max(g.minThumb, track * g.view / g.content));
    *top = g.arrow + (track - *height) * std::clamp(g.scroll / max, 0.0f, 1.0f);
    return true;
}

ScrollBarModel::Part ScrollBarModel::PartAt(const Geometry& g, float y) {
    if (y < g.arrow) return kUp;
    if (y >= g.length - g.arrow) return kDown;
    float top = 0, height = 0;
    return Thumb(g, &top, &height) && y >= top && y < top + height ? kThumb : kTrack;
}

bool ScrollBarModel::SetHot(Part part) {
    if (part == m_hot) return false;
    m_hot = part;
    return true;
}

float ScrollBarModel::Press(Part part, float y, const Geometry& g, float line, float page) {
    m_pressed = part;
    m_pressY = y;
    m_trackDir = 0;
    switch (part) {
    case kUp: return -line;
    case kDown: return line;
    case kTrack: {
        float top = 0, height = 0;
        if (!Thumb(g, &top, &height)) return 0;
        m_trackDir = y < top ? -1 : 1;
        return m_trackDir * page;
    }
    default: return 0;
    }
}

float ScrollBarModel::Repeat(const Geometry& g, float line, float page) const {
    const float max = g.MaxScroll();
    switch (m_pressed) {
    case kUp: return g.scroll > 0 ? -line : 0.0f;
    case kDown: return g.scroll < max ? line : 0.0f;
    case kTrack: {
        float top = 0, height = 0;
        if (!Thumb(g, &top, &height)) return 0;
        if (m_trackDir < 0) return m_pressY < top && g.scroll > 0 ? -page : 0.0f;
        if (m_trackDir > 0) return m_pressY >= top + height && g.scroll < max ? page : 0.0f;
        return 0;
    }
    default: return 0;
    }
}

bool ScrollBarModel::Release() {
    if (m_pressed == kNone) return false;
    m_pressed = kNone;
    m_trackDir = 0;
    return true;
}

void ScrollBarModel::Reset() {
    m_hot = m_pressed = kNone;
    m_trackDir = 0;
    m_blend.fill(0.0f);
}

bool ScrollBarModel::Animate(bool* changed) {
    bool active = false;
    for (int part = kUp; part <= kThumb; ++part) {
        const float target = m_pressed == part ? 2.0f : m_hot == part ? 1.0f : 0.0f;
        float& blend = m_blend[part];
        if (blend < target) {
            blend = std::min(target, blend + kFadeStep);
            *changed = true;
        } else if (blend > target) {
            blend = std::max(target, blend - kFadeStep);
            *changed = true;
        }
        if (blend != target) active = true;
    }
    return active;
}

} // namespace glasswave::ui
