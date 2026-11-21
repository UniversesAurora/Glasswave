#pragma once

#include <array>

// What a WMP 12 scroll bar does, apart from where it sits and how it is painted
// (wmp::paint::ScrollBar): which part is under the mouse and which is held, their looks fading
// between normal, hot and pressed, the thumb's place, and the steps a press and its
// auto-repeat scroll. The list view and the detail band's description each own one.

namespace glasswave::ui {

class ScrollBarModel {
  public:
    enum Part { kNone = 0, kUp = 1, kDown = 2, kTrack = 3, kThumb = 4 };

    // The bar in its own unit (pixels or DIP): its length, each arrow's and the smallest thumb;
    // the content it scrolls in the content's unit: its length, the view's and the scroll.
    struct Geometry {
        float length = 0, arrow = 0, minThumb = 0;
        float content = 0, view = 0, scroll = 0;
        float MaxScroll() const { return content > view ? content - view : 0.0f; }
    };

    // The thumb's top and height along the bar; false when nothing scrolls.
    static bool Thumb(const Geometry& g, float* top, float* height);
    static Part PartAt(const Geometry& g, float y);

    Part Hot() const { return m_hot; }
    Part Pressed() const { return m_pressed; }
    bool SetHot(Part part); // true when it changed
    // A press on `part` at `y` along the bar: the scroll it makes at once (an arrow's `line`,
    // the track's `page` towards the press; nothing for the thumb, which the owner drags).
    float Press(Part part, float y, const Geometry& g, float line, float page);
    // While an arrow or the track is held: the next step, 0 once it should stop (an end
    // reached, or the thumb under the press).
    float Repeat(const Geometry& g, float line, float page) const;
    bool Release(); // true when a part was held
    void Reset();   // nothing hot or held, every look normal at once

    // One tick of the looks' fades; true while one is still moving. `changed`: repaint.
    bool Animate(bool* changed);
    const float* Blend() const { return m_blend.data(); } // per part: 0 normal .. 1 hot .. 2 pressed

  private:
    Part m_hot = kNone, m_pressed = kNone;
    int m_trackDir = 0; // the track held above (-1) or below (1) the thumb
    float m_pressY = 0;
    std::array<float, 5> m_blend{};
};

} // namespace glasswave::ui
