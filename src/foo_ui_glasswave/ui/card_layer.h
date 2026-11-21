#pragma once

#include "media/list_sources.h"

#include <map>
#include <memory>
#include <optional>

// The featured cards of a list (TileStyle::Card: the home page's top rows; owner, 2026-11-15,
// after Apple Music's Top Picks, without its grey hover veil). Each shelf of cards is a strip of
// composition visuals under the list's rows surface (clear there, so the group headings and the
// shelf's arrows stay on top), laid out whole like a horizontal scroll: paging moves the strip.
// Under the mouse a card rises as a Material Design 1 card does - its two shadows go from the
// resting elevation (2 dp) to the raised one (8 dp) - on the compositor, with no repaint, and
// shows WMP's play orb in its corner. The card's corners are continuous-curvature (no visible
// joint where the curve meets the straight edge). A card with motion artwork plays it, muted and
// looping, while it is on screen and the window can be seen.
//
// The list says where the shelves are; the layer owns their cards, pictures, videos and states.
namespace glasswave::ui {

class CardLayer {
  public:
    // The card in DIP; a TileStyle::Card tile is this plus room for the raised shadow.
    static constexpr D2D1_SIZE_F kCard{220, 293};
    static constexpr float kCorner = 12;    // DIP: the corners' radius (continuous curvature)
    static constexpr float kPlay = 34;      // DIP: the play orb
    static constexpr float kPlayInset = 12; // DIP: from the card's right and bottom edges

    // Its visuals go into `parent` just below `above` (the list's rows).
    CardLayer(const winrt::Windows::UI::Composition::ContainerVisual& parent,
              const winrt::Windows::UI::Composition::Visual& above);
    ~CardLayer();
    CardLayer(const CardLayer&) = delete;
    CardLayer& operator=(const CardLayer&) = delete;

    // One shelf: its cards' rows and x along the strip (DIP), where the strip is scrolled to and,
    // while it pages, where it slides from.
    struct Shelf {
        std::wstring key;
        float top = 0;    // DIP from the layer's top: the cards' top edge
        float scroll = 0; // DIP: the strip's x at the layer's left edge
        std::optional<float> slideFrom;
        std::vector<std::pair<size_t, float>> cards;
    };
    // Where the cards show (px, in the list's visual) and the scale.
    void SetBounds(const RECT& px, float scale);
    // The shelves on screen; any other is hidden. A shelf with slideFrom slides over slideMs.
    void Layout(const media::ListSource& source, const std::vector<Shelf>& shelves, int slideMs);
    void Clear(); // another source: every card goes
    // The card under the mouse (SIZE_MAX: none) and whether the mouse is on its play orb.
    void SetHot(const media::ListSource* source, size_t row, bool onPlay);
    void SetPressed(const media::ListSource* source, size_t row); // SIZE_MAX: released
    // Whether the window can be seen: motion covers play only then.
    void SetMotion(bool on);

  private:
    struct Card;
    struct Strip;
    Card& CardFor(const media::CardInfo& info);
    Card* Find(const media::ListSource* source, size_t row);
    void Paint(Card& card);
    void PaintFace(Card& card);
    void PaintWords(Card& card);
    void LoadPicture(Card& card);
    void UpdateVideo(Card& card);
    void StopVideo(Card& card);
    void ApplyState(Card& card, bool animate);
    void Raise(Card& card);
    winrt::Windows::UI::Composition::CompositionSurfaceBrush ShapeBrush();
    int Px(float dip) const { return static_cast<int>(std::lround(dip * m_scale)); }

    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    std::map<std::wstring, std::unique_ptr<Strip>> m_strips;
    std::map<std::wstring, std::unique_ptr<Card>> m_cards;
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_shape{nullptr}; // the card's outline, white
    float m_shapeScale = 0;
    float m_scale = 1.0f;
    float m_viewWidth = 0; // DIP
    bool m_motion = true;
    bool m_animations = true; // Windows' "Animation effects" setting
    std::wstring m_hot, m_pressed;
    bool m_hotPlay = false;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

} // namespace glasswave::ui
