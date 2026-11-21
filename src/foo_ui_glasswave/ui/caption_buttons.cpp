#include "pch.h"
#include "ui/caption_buttons.h"

namespace glasswave::ui {

namespace paint = aero::paint;

Widget::Painter CaptionPainter(CaptionButton button, bool atlasArt, std::function<bool()> maximized) {
    return [button, atlasArt, maximized = std::move(maximized)](ID2D1DeviceContext* dc, D2D1_SIZE_F s,
                                                                paint::Layer l) {
        const paint::CaptionGlyph glyph = button == CaptionButton::Minimize ? paint::CaptionGlyph::Minimize
                                          : button == CaptionButton::Close ? paint::CaptionGlyph::Close
                                          : maximized && maximized()        ? paint::CaptionGlyph::Restore
                                                                            : paint::CaptionGlyph::Maximize;
        if (atlasArt)
            aero::atlas::CaptionButton(dc, s, button, glyph, l);
        else // the cluster's ends have the rounded corners
            paint::CaptionButton(dc, s, glyph, l, button == CaptionButton::Minimize, button == CaptionButton::Close);
    };
}

Widget::Options CaptionOptions(CaptionButton button, bool atlasArt) {
    Widget::Options o;
    o.inactiveLayer = true;
    if (atlasArt) {
        const bool close = button == CaptionButton::Close;
        o.glow = [close](ID2D1DeviceContext* dc, D2D1_SIZE_F s) { aero::atlas::CaptionGlow(dc, s, close); };
        o.glowOutset = aero::atlas::GlowOutset(close);
        o.glowOutsetInPixels = true;
    }
    return o;
}

} // namespace glasswave::ui
