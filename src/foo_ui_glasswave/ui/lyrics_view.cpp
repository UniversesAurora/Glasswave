#include "pch.h"

#include "ui/lyrics_view.h"

#include "app/settings.h"
#include "gfx/composition_host.h"
#include "gfx/effects.h"

#include <winrt/Windows.Foundation.Collections.h>
#include "ui/i18n.h"
#include "ui/pane_common.h"

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using Clock = std::chrono::steady_clock;

namespace {

// ---- Tuning (docs/design/lyrics-view.md) -----------------------------------------------
constexpr float kSideMargin = pane::kSideMargin;
constexpr float kTextInset = 0.45f;      // em: text inset inside the hover plate
constexpr float kLinePadding = 0.4f;     // em: above and below each line block
constexpr float kSubGap = 0.3f;          // em: main line to translation
constexpr float kLinesAbove = 1.5f;      // lines left visible above the focused one
constexpr float kFadeWidth = 0.5f;       // em: soft edge of the word fill
constexpr float kFloat = 0.05f;          // em: how far a sung word rises
constexpr float kPlateRadius = 0.35f;    // em
constexpr float kHoverAlpha = 0.09f, kPressAlpha = 0.05f;
constexpr float kInactiveScale = 97;     // percent, lines other than the active one while playing
constexpr float kPressScale = 95;        // percent, a pressed line
constexpr float kMaxBlur = 5;            // DIP
constexpr float kBlurAbove = 0.8f;       // DIP per line above the focus (sung lines), from 1.6
constexpr float kBlurBelow0 = 0.3f, kBlurBelow = 0.5f;  // upcoming lines stay readable longer
constexpr int kInterludeGapMs = 7000;    // gaps at least this long show the breathing dots
constexpr float kTopFade = 36, kBottomFade = 72;  // DIP

// Text alphas (white).
constexpr float kDim = 0.24f;            // a line that is not active
constexpr float kDimBrowsing = 0.45f;    // while the user scrolls the lines
constexpr float kSub = 0.32f;            // translations
constexpr float kSubBright = 0.85f;      // highlighted secondary line (sung / whole-line)
constexpr float kSubUnsung = 0.34f;      // unsung part of a secondary line following word progress
constexpr float kBright = 0.92f;         // sung part of the active line
constexpr float kUnsung = 0.36f;         // rest of the active line
constexpr float kUnsynced = 0.85f;       // lyrics without timestamps

constexpr SpringParams kPosSlow{0.9f, 90, 15};
constexpr SpringParams kScaleSpring{2, 100, 25};
constexpr SpringParams kPressSpring{1, 420, 32};
constexpr SpringParams kWheelSpring{1, 220, 32};

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float Mix(float a, float b, float t) { return a + (b - a) * t; }

// Unit cubic Bezier easing (CSS cubic-bezier).
float Bezier(float x1, float y1, float x2, float y2, float x) { return gfx::Ease({x1, y1, x2, y2}, x); }
float EaseOut(float x) { return Bezier(0, 0, 0.58f, 1, Clamp01(x)); }

// Rise and fall used by the long-word emphasis.
float EmphasisCurve(float x) {
    x = Clamp01(x);
    if (x < 0.5f) return Bezier(0.2f, 0.4f, 0.58f, 1.0f, x / 0.5f);
    return 1 - Bezier(0.3f, 0.0f, 0.58f, 1.0f, (x - 0.5f) / 0.5f);
}

bool IsCjk(std::wstring_view s) {
    for (wchar_t c : s)
        if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
            (c >= 0xF900 && c <= 0xFAFF))
            return true;
    return false;
}

std::wstring TrimCopy(std::wstring_view s) {
    while (!s.empty() && iswspace(s.front())) s.remove_prefix(1);
    while (!s.empty() && iswspace(s.back())) s.remove_suffix(1);
    return std::wstring(s);
}

// Apple Music lifts and lights up long syllables (AMLL: >= 1 s; Latin words of 2-7 letters).
bool ShouldEmphasize(const lyrics::Word& w) {
    if (w.end - w.start < 1000) return false;
    const std::wstring t = TrimCopy(w.text);
    if (IsCjk(t)) return true;
    return t.size() > 1 && t.size() <= 7;
}

// Marks a word's range in a text layout so DirectWrite splits glyph runs at word ends.
class WordTag final : public IUnknown {
public:
    explicit WordTag(int index) : index(index) {}
    const int index;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

private:
    ~WordTag() = default;
    LONG m_ref = 1;
};

com_ptr<IDWriteTextFormat> MakeFormat(float size, DWRITE_FONT_WEIGHT weight) { return pane::Format(size, weight); }

// The lines' own format: the families the user listed (settings::LyricFontList), each
// character taken from the first of them that has it and then from Windows' own fallback;
// without a list, the interface font.
com_ptr<IDWriteTextFormat> MakeLyricFormat(float size, DWRITE_FONT_WEIGHT weight) {
    const std::vector<std::wstring> families = settings::LyricFontList();
    if (families.empty()) return pane::Format(size, weight);
    auto* f = gfx::CompositionHost::Get().DWrite();
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    com_ptr<IDWriteTextFormat> format;
    if (FAILED(f->CreateTextFormat(families.front().c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, size, locale, format.put())))
        return pane::Format(size, weight);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    com_ptr<IDWriteFactory2> factory2;
    com_ptr<IDWriteTextFormat1> format1;
    com_ptr<IDWriteFontFallbackBuilder> builder;
    if (FAILED(f->QueryInterface(IID_PPV_ARGS(factory2.put()))) ||
        FAILED(format->QueryInterface(IID_PPV_ARGS(format1.put()))) ||
        FAILED(factory2->CreateFontFallbackBuilder(builder.put())))
        return format;
    std::vector<const wchar_t*> names;
    for (const auto& name : families) names.push_back(name.c_str());
    const DWRITE_UNICODE_RANGE all{0, 0x10FFFF};
    builder->AddMapping(&all, 1, names.data(), static_cast<UINT32>(names.size()), nullptr, nullptr, nullptr, 1.0f);
    com_ptr<IDWriteFontFallback> system, fallback;
    if (SUCCEEDED(factory2->GetSystemFontFallback(system.put()))) builder->AddMappings(system.get());
    if (SUCCEEDED(builder->CreateFontFallback(fallback.put()))) format1->SetFontFallback(fallback.get());
    return format;
}
using pane::White;

}  // namespace

// A glyph run of a line's main text, copied out of DirectWrite once per layout.
struct LyricsView::Run {
    D2D1_POINT_2F origin{};  // baseline origin, relative to the text origin
    com_ptr<IDWriteFontFace> face;
    float em = 0;
    std::vector<UINT16> glyphs;
    std::vector<float> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    BOOL sideways = FALSE;
    UINT32 bidi = 0;
    int word = 0;
    float width = 0, ascent = 0, descent = 0;
    float wordX = 0;  // where this run starts within its word's width (runs of a wrapped word)

    DWRITE_GLYPH_RUN Glyphs() const {
        DWRITE_GLYPH_RUN r{};
        r.fontFace = face.get();
        r.fontEmSize = em;
        r.glyphCount = static_cast<UINT32>(glyphs.size());
        r.glyphIndices = glyphs.data();
        r.glyphAdvances = advances.data();
        r.glyphOffsets = offsets.empty() ? nullptr : offsets.data();
        r.isSideways = sideways;
        r.bidiLevel = bidi;
        return r;
    }
};

struct LyricsView::LineView {
    const lyrics::Line* line = nullptr;
    com_ptr<IDWriteTextLayout> main, sub;
    std::vector<Run> runs, subRuns;
    float subTotalWidth = 0;
    struct WordView {
        int start = 0, end = 0;
        bool emphasize = false, last = false;
        float width = 0;               // sum of its runs
        D2D1_RECT_F box{};             // union of its runs, relative to the text origin
        com_ptr<ID2D1CommandList> glow;  // the word in white, blurred into its glow
    };
    std::vector<WordView> words;
    float mainH = 0, subH = 0, height = 0;
    float mainY = 0, subY = 0; // the line's and its translation's tops in the text block
    Spring y{0}, scale{100}, press{100};
    float blur = 0, blurTarget = 0;
    float glow = 0;  // 0 inactive .. 1 active (bright layer)
    float hover = 0;
    bool active = false;

    // Composition: the block (position, scale) holds the hover plate and the text image.
    pane::PlateRow view;
    wuc::CompositionDrawingSurface surface{nullptr};
    wuc::CompositionSurfaceBrush surfaceBrush{nullptr};
    wuc::CompositionEffectBrush blurBrush{nullptr};
    bool blurred = false;    // the text uses the blur brush
    int image = -1;          // what the surface shows: -1 nothing, 0 the dim image, 1 live
    float imageGlow = -1;    // glow the live image was drawn at
    float shownBlur = -1;    // last values sent to the compositor
};

namespace {

// Collects glyph runs of a layout (IDWriteTextLayout::Draw) instead of drawing them.
class RunCollector final : public IDWriteTextRenderer {
public:
    std::vector<LyricsView::Run>* runs = nullptr;  // filled by DrawGlyphRun

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override {
        *ppd = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown* effect) override;
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }
};

HRESULT RunCollector::DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                   const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown* effect) {
    if (!runs || !run || run->glyphCount == 0) return S_OK;
    LyricsView::Run r;
    r.origin = {x, y};
    r.face.copy_from(run->fontFace);
    r.em = run->fontEmSize;
    r.glyphs.assign(run->glyphIndices, run->glyphIndices + run->glyphCount);
    if (run->glyphAdvances) r.advances.assign(run->glyphAdvances, run->glyphAdvances + run->glyphCount);
    else r.advances.assign(run->glyphCount, 0.0f);
    if (run->glyphOffsets) r.offsets.assign(run->glyphOffsets, run->glyphOffsets + run->glyphCount);
    r.sideways = run->isSideways;
    r.bidi = run->bidiLevel;
    r.word = effect ? static_cast<WordTag*>(effect)->index : 0;
    for (float a : r.advances) r.width += a;
    DWRITE_FONT_METRICS fm{};
    run->fontFace->GetMetrics(&fm);
    const float k = run->fontEmSize / std::max<float>(1, fm.designUnitsPerEm);
    r.ascent = fm.ascent * k;
    r.descent = fm.descent * k;
    runs->push_back(std::move(r));
    return S_OK;
}

}  // namespace

// ---- Construction and state -------------------------------------------------------------

LyricsView::LyricsView(const wuc::ContainerVisual& parent) {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    m_dc = host.CreateDeviceContext();
    m_deviceReplaced = host.OnDeviceReplaced([this] {
        m_dc = gfx::CompositionHost::Get().CreateDeviceContext();
        for (auto& l : m_lines)
            for (auto& wv : l->words) wv.glow = nullptr;
        if (!m_visible) return; // the next showing lays out and draws everything again
        SetVisible(false);
        SetVisible(true);
    });
    // Keep the clip fixed while m_root rises during the lyrics/queue transition.
    m_viewport = comp.CreateContainerVisual();
    m_viewport.Clip(comp.CreateInsetClip());
    parent.Children().InsertAtTop(m_viewport);
    m_root = comp.CreateContainerVisual();
    m_root.IsVisible(false);
    m_viewport.Children().InsertAtTop(m_root);
    m_viewport.Comment(L"lyrics viewport");
    m_message = comp.CreateSpriteVisual();
    m_message.Comment(L"lyrics message");
    m_root.Children().InsertAtTop(m_message);

    // The lines live in a layer whose alpha is multiplied by a vertical gradient: they fade
    // out under the header and at the bottom (Apple Music's edge fade), on the GPU.
    m_layer.Create(m_root);
    m_plateGlass.Create(m_root, m_layer.Visual());
    m_blurFactory = comp.CreateEffectFactory(
        gfx::MakeGaussianBlurEffect(L"Blur", 0, wuc::CompositionEffectSourceParameter(L"source")),
        std::vector<winrt::hstring>{L"Blur.BlurAmount"});

    m_dots = comp.CreateContainerVisual();
    for (size_t k = 0; k < 3; ++k) {
        auto shape = comp.CreateShapeVisual();
        auto ellipse = comp.CreateEllipseGeometry();
        auto sprite = comp.CreateSpriteShape(ellipse);
        m_dotBrush[k] = comp.CreateColorBrush(winrt::Windows::UI::Colors::White());
        sprite.FillBrush(m_dotBrush[k]);
        shape.Shapes().Append(sprite);
        m_dots.Children().InsertAtTop(shape);
    }
    m_layer.Visual().Children().InsertAtTop(m_dots);
}

LyricsView::~LyricsView() {
    *m_alive = false;
    if (m_viewport) {
        if (auto parent = m_viewport.Parent()) parent.Children().Remove(m_viewport);
    }
}

void LyricsView::SetBounds(const RECT& px, float scale) {
    const bool sizeChanged = px.right - px.left != m_bounds.right - m_bounds.left ||
                             px.bottom - px.top != m_bounds.bottom - m_bounds.top || scale != m_scale;
    m_bounds = px;
    m_scale = scale;
    const float w = static_cast<float>(std::max<LONG>(0, px.right - px.left));
    const float h = static_cast<float>(std::max<LONG>(0, px.bottom - px.top));
    m_viewport.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    m_viewport.Size(float2{w, h});
    m_root.Size(float2{w, h});
    if (sizeChanged && m_visible) {
        Relayout(); // first: the fades (LayoutChrome) follow the line height
        LayoutChrome();
        UpdateTargets(false, true, false);
        Render();
    }
}

void LyricsView::SetBottomInset(float dip) {
    if (dip == m_bottomInset) return;
    m_bottomInset = dip;
    if (!m_visible) return;
    LayoutChrome();
    UpdateTargets(false, true, false);
    Render();
}

void LyricsView::SetVisible(bool visible) {
    if (visible == m_visible) return;
    m_visible = visible;
    m_root.IsVisible(visible);
    if (visible) {
        Relayout();
        LayoutChrome();
        m_lastNow = Now();
        m_lastNowAt = Clock::now();
        m_focus = -2;  // recomputed by Tick
        m_lastFrame = Clock::now();
        m_idle = false;
        Tick();
        for (auto& l : m_lines) {
            l->y.Snap(l->y.Target());
            l->scale.Snap(l->scale.Target());
            l->blur = l->blurTarget;
        }
        Render();
        StartFrames();
    } else {
        for (auto& l : m_lines) ReleaseLine(*l);  // surfaces are not kept while hidden
    }
}

void LyricsView::SetHasTrack(bool hasTrack) {
    if (hasTrack == m_hasTrack) return;
    m_hasTrack = hasTrack;
    if (m_visible) PaintMessage();
}

void LyricsView::SetResult(const lyrics::Result& result) {
    // A same-track refresh must not become a new document. Searching and an equal parsed
    // result leave the line views, scroll, focus and word-fill state in place.
    if (result.status == lyrics::Status::Searching && m_lyrics) return;
    if (result.status == lyrics::Status::Found && result.lyrics && m_lyrics &&
        lyrics::SameLyricsDocument(*m_lyrics, *result.lyrics)) {
        m_status = lyrics::Status::Found;
        return;
    }
    // The same lines at other times (the owner adjusting the offset): keep the views and
    // retime them, so the lines move to the new focus instead of flying in again.
    if (result.status == lyrics::Status::Found && result.lyrics && m_lyrics && m_status == lyrics::Status::Found &&
        m_lines.size() == result.lyrics->lines.size() && lyrics::SameLyricsText(*m_lyrics, *result.lyrics)) {
        m_lyrics = result.lyrics;
        for (size_t i = 0; i < m_lines.size(); ++i) {
            auto& v = *m_lines[i];
            v.line = &m_lyrics->lines[i];
            if (v.line->words.empty()) {
                v.words[0].start = v.line->start;
                v.words[0].end = v.line->end;
            } else {
                for (size_t w = 0; w < v.words.size() && w < v.line->words.size(); ++w) {
                    v.words[w].start = v.line->words[w].start;
                    v.words[w].end = v.line->words[w].end;
                }
            }
        }
        if (m_visible) StartFrames();
        return;
    }
    m_status = result.status;
    m_lyrics = result.status == lyrics::Status::Found ? result.lyrics : nullptr;
    m_browsing = false;
    m_scroll = 0;
    m_velocity = 0;
    m_hover = m_pressed = -1;
    m_focus = -2;
    m_interlude = -2;
    m_interludeShow = 0;
    Relayout();
    if (!m_visible) return;
    PaintMessage();
    // New lyrics fly in from below, line after line.
    m_lastNow = Now();
    m_lastNowAt = Clock::now();
    m_idle = false;
    Tick();
    const float H = (m_bounds.bottom - m_bounds.top) / m_scale;
    for (auto& l : m_lines) l->y.Snap(l->y.Target() + H * 0.6f);
    UpdateTargets(true, false, true);
    StartFrames();
}

void LyricsView::SetPlaying(bool playing, bool paused) {
    if (playing == m_playing && paused == m_paused) return;
    m_playing = playing;
    m_paused = paused;
    m_lastNow = Now(); // the clock the next tick measures a seek against starts here
    m_lastNowAt = Clock::now();
    if (m_visible) {
        UpdateTargets(false, false, true);
        StartFrames();
    }
}

void LyricsView::StartFrames() {
    m_idle = false;
    m_lastFrame = Clock::now();
    if (onFrames) onFrames();
}

// Whether a sleeping view has something to show again: the time jumped (a seek, also one made
// while paused) or reached the next line's start or end.
bool LyricsView::WakeDue(double now) const {
    if (!m_playing || m_paused) return std::abs(now - m_lastNow) > 0.001;
    const double expected = m_lastNow + std::chrono::duration<double>(Clock::now() - m_lastNowAt).count();
    return std::abs(now - expected) > 0.6 || static_cast<int>(now * 1000) >= m_nextEventMs;
}

void LyricsView::Resync() {
    if (m_visible && m_idle && m_lyrics && m_lyrics->synced && WakeDue(Now())) StartFrames();
}

// A line-timed song changes nothing between one line's start or end and the next: frames
// stop, and one wake-up (never more than one pending) brings them back at that moment.
void LyricsView::SleepUntilNextEvent() {
    ++m_wakeGeneration;
    m_idle = true;
    m_nextEventMs = INT_MAX;
    if (!m_lyrics || !m_lyrics->synced || !m_playing || m_paused) return;
    const int ms = static_cast<int>(m_lastNow * 1000);
    for (const auto& l : m_lines) {
        if (l->line->start > ms) m_nextEventMs = std::min(m_nextEventMs, l->line->start);
        if (l->line->end > ms) m_nextEventMs = std::min(m_nextEventMs, l->line->end);
    }
    if (m_nextEventMs == INT_MAX) return;
    const double delay = std::max(0.0, (m_nextEventMs - m_lastNow * 1000.0) / 1000.0) + 0.002;
    fb2k::callLater(delay, [this, alive = m_alive, generation = m_wakeGeneration] {
        if (*alive && generation == m_wakeGeneration && m_idle) StartFrames();
    });
}

double LyricsView::Now() const {
    try {
        auto pc = playback_control::get();
        metadb_handle_ptr track;
        if (pc->get_now_playing(track)) return pc->playback_get_position();
    } catch (...) {
    }
    // An output-device rebuild can briefly unload the core's current item. Freeze the last
    // position instead of treating that transient state as a seek to 0:00.
    return m_lastNow;
}

// The view's own size for its width, times the user's percentage.
float LyricsView::FontSize() const {
    const float w = (m_bounds.right - m_bounds.left) / m_scale;
    const float automatic = std::clamp(std::round(w * 0.075f), 20.0f, 42.0f);
    return std::round(automatic * settings::LyricSizePercent() / 100.0f);
}

void LyricsView::Restyle() {
    if (!m_visible) return; // SetVisible lays everything out afresh
    Relayout();
    LayoutChrome();
    UpdateTargets(false, true, false);
    Render();
    StartFrames();
}

float LyricsView::ContentTop() const { return 0; }

// The room above the focused line. Normally a fixed reference block (1.5 lines) keeps the
// focus at the same height across songs, regardless of wrapping or translations. In a low view
// the room goes in turn: first below the line, until it is as much as above; then above it,
// until none is left; then below again - the line itself always stays whole.
float LyricsView::TopSpace(float lineH) const {
    const float H = std::max(0.0f, (m_bounds.bottom - m_bounds.top) / m_scale - ContentTop() - m_bottomInset);
    const float above = kLinesAbove * m_referenceLineHeight;
    if (H >= 2 * above + lineH) return above;
    if (H >= above + lineH) return H - lineH - above;
    return 0;
}

float LyricsView::AnchorY() const {
    const float lineH = m_focus >= 0 && static_cast<size_t>(m_focus) < m_lines.size()
                            ? m_lines[static_cast<size_t>(m_focus)]->height
                            : m_referenceLineHeight;
    return ContentTop() + TopSpace(lineH);
}

float LyricsView::MinHeight() const {
    float tallest = m_referenceLineHeight;
    for (const auto& l : m_lines) tallest = std::max(tallest, l->height);
    return ContentTop() + tallest;
}

// ---- Layout -----------------------------------------------------------------------------

void LyricsView::Relayout() {
    for (auto& l : m_lines) ReleaseLine(*l);
    m_lines.clear();
    m_dc->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    if (W <= 0 || !m_lyrics) return;
    const float F = FontSize();
    m_layoutWidth = std::max(40.0f, W - 2 * kSideMargin - 2 * kTextInset * F);
    lyrics::Line reference;
    // Measured in the lines' own fonts (and their fallback): a Latin pair and a CJK character,
    // so that a tall fallback font sets the reference too.
    reference.text = reference.translation = L"Ag\x56FD";
    LineView referenceView;
    referenceView.line = &reference;
    BuildLine(referenceView, m_layoutWidth);
    m_referenceLineHeight = referenceView.height;
    for (const auto& line : m_lyrics->lines) {
        auto view = std::make_unique<LineView>();
        view->line = &line;
        BuildLine(*view, m_layoutWidth);
        view->y.SetParams(kPosSlow);
        view->scale.SetParams(kScaleSpring);
        view->press.SetParams(kPressSpring);
        m_lines.push_back(std::move(view));
    }
}

void LyricsView::BuildLine(LineView& v, float width) {
    auto* f = gfx::CompositionHost::Get().DWrite();
    const float F = FontSize();
    const lyrics::Line& line = *v.line;
    static thread_local float cachedSize = 0;
    static thread_local int cachedStyle = -1;
    static thread_local com_ptr<IDWriteTextFormat> mainFormat, subFormat;
    if (cachedSize != F || cachedStyle != settings::LyricStyleGeneration()) {
        // The translation's size is the user's percentage of the same automatic size.
        const float subSize = F * settings::TranslationSizePercent() / std::max(1, settings::LyricSizePercent());
        mainFormat = MakeLyricFormat(F, DWRITE_FONT_WEIGHT_BOLD);
        subFormat = MakeLyricFormat(subSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        cachedSize = F;
        cachedStyle = settings::LyricStyleGeneration();
    }
    const std::wstring& text = line.text;
    winrt::check_hresult(f->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), mainFormat.get(), width,
                                             100000.0f, v.main.put()));
    // One drawing effect per word, so glyph runs end at word boundaries.
    const size_t wordCount = std::max<size_t>(1, line.words.size());
    v.words.assign(wordCount, {});
    if (!line.words.empty()) {
        UINT32 at = 0;
        for (size_t i = 0; i < line.words.size(); ++i) {
            const UINT32 len = static_cast<UINT32>(line.words[i].text.size());
            const UINT32 clamped = at >= text.size() ? 0 : std::min<UINT32>(len, static_cast<UINT32>(text.size()) - at);
            if (clamped > 0) {
                auto* tag = new WordTag(static_cast<int>(i));
                v.main->SetDrawingEffect(tag, DWRITE_TEXT_RANGE{at, clamped});
                tag->Release();
            }
            at += len;
            auto& wv = v.words[i];
            wv.start = line.words[i].start;
            wv.end = line.words[i].end;
            wv.emphasize = ShouldEmphasize(line.words[i]);
            wv.last = i + 1 == line.words.size();
        }
    } else {
        v.words[0].start = line.start;
        v.words[0].end = line.end;
    }
    RunCollector collector;
    collector.runs = &v.runs;
    v.main->Draw(nullptr, &collector, 0, 0);
    // Word extents (a wrapped word has several runs).
    for (auto& wv : v.words) wv.box = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (auto& r : v.runs) {
        if (r.word < 0 || static_cast<size_t>(r.word) >= v.words.size()) r.word = 0;
        auto& wv = v.words[static_cast<size_t>(r.word)];
        r.wordX = wv.width;
        wv.width += r.width;
        wv.box.left = std::min(wv.box.left, r.origin.x);
        wv.box.right = std::max(wv.box.right, r.origin.x + r.width);
        wv.box.top = std::min(wv.box.top, r.origin.y - r.ascent);
        wv.box.bottom = std::max(wv.box.bottom, r.origin.y + r.descent);
    }
    DWRITE_TEXT_METRICS tm{};
    v.main->GetMetrics(&tm);
    v.mainH = tm.height;
    v.subH = 0;
    v.subRuns.clear();
    v.subTotalWidth = 0;
    if (!line.translation.empty()) {
        winrt::check_hresult(f->CreateTextLayout(line.translation.c_str(), static_cast<UINT32>(line.translation.size()),
                                                 subFormat.get(), width, 100000.0f, v.sub.put()));
        RunCollector subCollector;
        subCollector.runs = &v.subRuns;
        v.sub->Draw(nullptr, &subCollector, 0, 0);
        for (auto& r : v.subRuns) {
            r.wordX = v.subTotalWidth;
            v.subTotalWidth += r.width;
        }
        DWRITE_TEXT_METRICS sm{};
        v.sub->GetMetrics(&sm);
        v.subH = sm.height;
    }
    v.height = 2 * kLinePadding * F + v.mainH + (v.sub ? kSubGap * F + v.subH : 0);
    // The smaller line (the translation, or the original when they are swapped) under the lyric.
    v.mainY = 0;
    v.subY = v.mainH + kSubGap * F;
}

// Where every line goes for the current focus, with Apple Music's staggered start: lines
// start one after another from the top, 50 ms apart, the gap shrinking below the focus.
void LyricsView::UpdateTargets(bool stagger, bool snap, bool slow) {
    if (m_lines.empty()) return;
    const float F = FontSize();
    const float interH = 1.3f * F;
    const bool synced = m_lyrics && m_lyrics->synced;
    std::vector<float> tops(m_lines.size());
    float y = 0, interTop = 0;
    const bool interlude = synced && m_interlude != -2;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (interlude && m_interlude == -1 && i == 0) {
            interTop = y;
            y += interH;
        }
        tops[i] = y;
        y += m_lines[i]->height;
        if (interlude && m_interlude == static_cast<int>(i)) {
            interTop = y;
            y += interH;
        }
    }
    float focalTop = 0;
    if (synced) {
        if (interlude) focalTop = interTop;
        else if (m_focus >= 0) focalTop = tops[static_cast<size_t>(m_focus)];
    }
    if (!synced) focalTop = 0;
    const float anchor = synced ? AnchorY() : ContentTop() + 24;
    // While browsing the layout is frozen where the user left it.
    if (!m_browsing) m_frozenFocal = focalTop;
    const float base = anchor - (m_browsing ? m_frozenFocal : focalTop) + (m_browsing ? m_scroll : 0);

    // Spring for this move: slow for seeks, interludes and the ends; otherwise faster the
    // closer the lines follow each other (AMLL's interval policy).
    SpringParams pos = kPosSlow;
    if (!slow && synced && m_focus > 0 && !interlude) {
        const float interval = static_cast<float>(m_lines[static_cast<size_t>(m_focus)]->line->start -
                                                  m_lines[static_cast<size_t>(m_focus) - 1]->line->start);
        float ratio = 1 - (std::clamp(interval, 100.0f, 800.0f) - 100) / 700;
        ratio = std::pow(ratio, 0.2f);
        const float k = 170 + ratio * 50;
        pos = {0.9f, k, std::sqrt(k) * 2.2f};
    }
    if (m_browsing && !snap) pos = kWheelSpring;

    float delay = 0, step = stagger ? 0.05f : 0;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        auto& l = *m_lines[i];
        const float target = base + tops[i];
        l.y.SetParams(pos);
        if (snap) l.y.Snap(target);
        else l.y.SetTarget(target, delay);
        const bool scaleDown = synced && m_playing && !m_paused && !l.active;
        l.scale.SetTarget(scaleDown ? kInactiveScale : 100, delay);
        // Blur grows with the distance from the focus: quickly for sung lines above, slowly
        // for the upcoming ones below so the next few stay readable.
        float level = 0;
        if (synced && !l.active && !m_browsing) {
            const int fi = std::max(m_focus, 0);
            const int idx = static_cast<int>(i);
            level = idx < fi ? (fi - idx + 1) * kBlurAbove : kBlurBelow0 + (idx - fi - 1) * kBlurBelow;
            level = std::min(kMaxBlur, std::max(0.0f, level));
        }
        l.blurTarget = level;
        if (target + l.height >= ContentTop()) {
            delay += step;
            if (static_cast<int>(i) >= m_focus) step /= 1.05f;
        }
    }
    const float interTarget = base + interTop;
    m_interludeY.SetParams(pos);
    if (snap) m_interludeY.Snap(interTarget);
    else m_interludeY.SetTarget(interTarget);
}

// ---- Frame ------------------------------------------------------------------------------

bool LyricsView::NeedsFrames() const {
    if (!m_visible) return false;
    if (m_lyrics && m_lyrics->synced && m_playing && !m_paused) {
        // Only the interlude's dots and a word-timed line's fill follow the time itself.
        if (m_interlude != -2) return true;
        const int ms = static_cast<int>(m_lastNow * 1000);
        for (const auto& l : m_lines)
            if (!l->line->words.empty() && l->glow > 0.01f && ms <= l->line->end + 1200) return true;
    }
    if (m_drag == Drag::Scroll || m_velocity != 0) return true;
    for (const auto& l : m_lines)
        if (l->y.Moving() || l->scale.Moving() || l->press.Moving() || l->blur != l->blurTarget ||
            l->glow != (l->active ? 1.0f : 0.0f) || l->hover != (l->line && m_hover >= 0 &&
                                                                  m_lines[static_cast<size_t>(m_hover)].get() == l.get()
                                                              ? 1.0f
                                                              : 0.0f))
            return true;
    if (m_interludeY.Moving() || m_interludeShow != (m_interlude != -2 ? 1.0f : 0.0f)) return true;
    if (m_browsing) return true;  // waits to return to the playing line
    if (m_dim != kDim) return true;
    return false;
}

bool LyricsView::Tick() {
    if (!m_visible) return false;
    // Asleep while the window's frames run for something else (its seek head ticks through the
    // whole song): nothing here changes until the time jumps or the next line starts or ends,
    // so the frame is neither rendered nor answered with another wake-up.
    if (m_idle && !WakeDue(Now())) return false;
    const auto frame = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(frame - m_lastFrame).count(), 0.0f, 0.1f);
    m_lastFrame = frame;
    const double now = Now();
    const bool synced = m_lyrics && m_lyrics->synced && !m_lines.empty();

    if (synced) {
        // Jumps in time (seeking, a click on a line) move without the stagger, slowly.
        // A sleeping view has not ticked for as long as the line lasted: measure against the
        // clock, not against one frame.
        const double expected =
            m_lastNow +
            ((m_playing && !m_paused) ? std::chrono::duration<double>(frame - m_lastNowAt).count() : 0);
        const bool seek = std::abs(now - expected) > 0.6;
        m_lastNow = now;
        m_lastNowAt = frame;
        const int ms = static_cast<int>(now * 1000);
        int focus = -1;
        for (size_t i = 0; i < m_lines.size(); ++i) {
            if (m_lines[i]->line->start <= ms) focus = static_cast<int>(i);
            else break;
        }
        // Interlude: a gap of 7 s or more with no line playing (also before the first line).
        int interlude = -2;
        int maxEnd = 0;
        for (int i = -1; i + 1 < static_cast<int>(m_lines.size()); ++i) {
            if (i >= 0) maxEnd = std::max(maxEnd, m_lines[static_cast<size_t>(i)]->line->end);
            const int nextStart = m_lines[static_cast<size_t>(i + 1)]->line->start;
            if (nextStart - maxEnd >= kInterludeGapMs && ms >= maxEnd && ms < nextStart) {
                interlude = i;
                m_interludeStart = maxEnd / 1000.0;
                m_interludeEnd = nextStart / 1000.0;
                break;
            }
            if (nextStart > ms) break;
        }
        bool activeChanged = false;
        for (auto& l : m_lines) {
            const bool active = l->line->start <= ms && ms < l->line->end;
            if (active != l->active) {
                l->active = active;
                activeChanged = true;
            }
        }
        const bool focusChanged = focus != m_focus || interlude != m_interlude;
        if (m_browsing && m_drag == Drag::None && m_velocity == 0) {
            const float idle = std::chrono::duration<float>(frame - m_interactionEnd).count();
            if ((focusChanged && idle >= 0.5f) || (!m_hoverInside && idle >= 3.0f) || seek) {
                m_browsing = false;
                m_scroll = 0;
            }
        }
        if (focusChanged || activeChanged || seek) {
            const bool first = m_focus == -2;
            m_focus = focus;
            m_interlude = interlude;
            UpdateTargets(!seek && !first && !m_browsing, first, seek || interlude != -2 || focus <= 0);
        }
    }

    // Drag inertia.
    if (m_velocity != 0) {
        m_scroll += m_velocity * dt;
        m_velocity = FlingTracker::Decay(m_velocity, dt);
        if (std::abs(m_velocity) < 30) {
            m_velocity = 0;
            m_interactionEnd = frame;
        }
        UpdateTargets(false, true, false);
    }

    // Lines move under a resting pointer (the song advancing, the wheel, inertia): the
    // highlight goes to the line now under it instead of travelling with the old one.
    if (m_hoverInside && m_drag == Drag::None) {
        const int hover = (m_lyrics && m_lyrics->synced) ? LineAt(m_pointer) : -1;
        if (hover != m_hover) {
            if (m_pressed >= 0 && m_pressed != hover) {
                m_lines[static_cast<size_t>(m_pressed)]->press.SetTarget(100);
                m_pressed = -1;
            }
            m_hover = hover;
        }
    }

    for (size_t i = 0; i < m_lines.size(); ++i) {
        auto& l = *m_lines[i];
        l.y.Step(dt);
        l.scale.Step(dt);
        l.press.Step(dt);
        const bool hovered = static_cast<int>(i) == m_hover;
        l.blur = Approach(l.blur, hovered ? 0 : l.blurTarget, dt, 0.4f);
        l.glow = Approach(l.glow, l.active ? 1.0f : 0.0f, dt, l.active ? 0.3f : 0.45f);
        l.hover = Approach(l.hover, hovered ? 1.0f : 0.0f, dt, 0.25f);
    }
    m_interludeY.Step(dt);
    m_interludeShow = Approach(m_interludeShow, m_interlude != -2 ? 1.0f : 0.0f, dt, 0.4f);
    m_dim = Approach(m_dim, m_browsing ? kDimBrowsing : kDim, dt, 0.4f);
    Render();
    const bool more = NeedsFrames();
    if (more) {
        m_idle = false;
        ++m_wakeGeneration; // frames run: no wake-up is wanted
    } else {
        SleepUntilNextEvent();
    }
    return more;
}

// ---- Rendering --------------------------------------------------------------------------

float LyricsView::ImagePad() const { return std::max(16.0f, 0.45f * FontSize()); }

void LyricsView::LayoutChrome() {
    const float w = static_cast<float>(std::max<LONG>(0, m_bounds.right - m_bounds.left));
    const float h = static_cast<float>(std::max<LONG>(0, m_bounds.bottom - m_bounds.top));
    const float top = std::round(ContentTop() * m_scale);
    m_layer.Visual().Offset(float3{0, top, 0});
    m_layer.Visual().Size(float2{w, std::max(0.0f, h - top)});
    if (h > top) {
        const int mh = static_cast<int>(h - top);
        const float H = mh / m_scale;
        // The fades keep to the room above and below the focused line, so that in a low view
        // they never dim the line itself.
        const float above = TopSpace(m_referenceLineHeight);
        const float below = std::max(0.0f, H - above - m_referenceLineHeight);
        m_layer.SetFades(mh, m_scale, std::min(kTopFade, above), std::min(kBottomFade, below));
    }
    PaintMessage();
}

void LyricsView::PaintMessage() {
    std::wstring text;
    switch (m_status) {
    case lyrics::Status::None: text = !m_hasTrack ? Tr(L"Nothing playing") : L""; break;
    case lyrics::Status::Searching: text = Tr(L"Searching for lyrics…"); break;
    case lyrics::Status::NotFound: text = Tr(L"No lyrics found"); break;
    case lyrics::Status::Found: break;
    }
    m_message.IsVisible(!text.empty());
    if (text.empty()) return;
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(1, m_bounds.right - m_bounds.left), h = std::max<int>(1, m_bounds.bottom - m_bounds.top);
    m_message.Size(float2{static_cast<float>(w), static_cast<float>(h)});
    m_message.Brush(nullptr);
    auto surface = host.CreateSurface(w, h);
    const bool drawn = host.Draw(surface, m_scale, [&](ID2D1DeviceContext* dc) {
        const float W = w / m_scale, H = h / m_scale;
        auto format = MakeFormat(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        com_ptr<IDWriteTextLayout> layout;
        auto* f = gfx::CompositionHost::Get().DWrite();
        if (FAILED(f->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), W - 40, 200,
                                       layout.put())))
            return;
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        const float usableH = std::max(0.0f, H - ContentTop() - m_bottomInset);
        const float top = ContentTop() + (usableH - tm.height) / 2 - 20;
        dc->DrawTextLayout({20, top}, layout.get(), White(dc, 0.55f).get());
    });
    if (!drawn) return; // a surface that was not drawn holds other surfaces' pixels
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::None);
    brush.HorizontalAlignmentRatio(0);
    brush.VerticalAlignmentRatio(0);
    m_message.Brush(brush);
}

void LyricsView::ReleaseLine(LineView& l) {
    l.view.Release();
    l.surface = nullptr;
    l.surfaceBrush = nullptr;
    l.blurBrush = nullptr;
    l.blurred = false;
    l.image = -1;
    l.imageGlow = -1;
    l.shownBlur = -1;
}

// Command lists of the emphasized words in white, blurred into their glow while sung. They are
// recorded outside any surface draw.
void LyricsView::PrepareGlow(LineView& l) {
    for (size_t w = 0; w < l.words.size(); ++w) {
        auto& wv = l.words[w];
        if (!wv.emphasize || wv.glow) continue;
        com_ptr<ID2D1CommandList> g;
        if (FAILED(m_dc->CreateCommandList(g.put()))) continue;
        m_dc->SetTarget(g.get());
        m_dc->BeginDraw();
        m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
        auto ink = White(m_dc.get(), 1.0f);
        for (const auto& r : l.runs) {
            if (r.word != static_cast<int>(w)) continue;
            const auto run = r.Glyphs();
            m_dc->DrawGlyphRun(r.origin, &run, ink.get());
        }
        m_dc->EndDraw();
        g->Close();
        m_dc->SetTarget(nullptr);
        wv.glow = g;
    }
}

// The line's surface: the dim image (white, the visual's opacity sets the level) or the live
// image of an active line (exact alphas, word fill, float, glow).
void LyricsView::DrawLineImage(LineView& l, double now, bool live) {
    auto& host = gfx::CompositionHost::Get();
    const float F = FontSize(), pad = ImagePad();
    const float textH = l.mainH + (l.sub ? kSubGap * F + l.subH : 0);
    const int w = static_cast<int>(std::ceil((m_layoutWidth + 2 * pad) * m_scale));
    const int h = static_cast<int>(std::ceil((textH + 2 * pad) * m_scale));
    const bool freshSurface = !l.surface;
    if (freshSurface) l.surface = host.CreateSurface(w, h);
    if (live) PrepareGlow(l);
    const bool drawn = host.Draw(l.surface, m_scale, [&](ID2D1DeviceContext* dc) {
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(pad, pad) * base);
        if (live) {
            DrawLiveText(dc, l, now);
        } else {
            // Main text at 0.75 and the translation at 1: the visual's opacity scales both.
            dc->DrawTextLayout({0, l.mainY}, l.main.get(), White(dc, 0.75f).get());
            if (l.sub) dc->DrawTextLayout({0, l.subY}, l.sub.get(), White(dc, 0.75f * kSub / kDim).get());
        }
    });
    if (!drawn) {
        if (freshSurface) {
            l.view.content.Brush(nullptr);
            l.surface = nullptr;
            l.surfaceBrush = nullptr;
        }
        l.image = -1;
        l.imageGlow = l.glow;
        StartFrames();
        return;
    }
    if (freshSurface) {
        l.surfaceBrush = host.Compositor().CreateSurfaceBrush(l.surface);
        l.surfaceBrush.Stretch(wuc::CompositionStretch::None);
        l.surfaceBrush.HorizontalAlignmentRatio(0);
        l.surfaceBrush.VerticalAlignmentRatio(0);
        l.view.content.Size(float2{static_cast<float>(w), static_cast<float>(h)});
        l.view.content.Brush(l.surfaceBrush);
        l.blurred = false;
        l.shownBlur = -1;
    }
    l.image = live ? 1 : 0;
    l.imageGlow = l.glow;
}

// Glyph runs with per-word fill, float and emphasis; the transform maps the text origin.
void LyricsView::DrawLiveText(ID2D1DeviceContext* dc, LineView& l, double now) {
    const float F = FontSize();
    D2D1_MATRIX_3X2_F blockM;
    dc->GetTransform(&blockM);
    const D2D1_MATRIX_3X2_F lineM = D2D1::Matrix3x2F::Translation(0, l.mainY) * blockM;
    const int ms = static_cast<int>(now * 1000);
    const bool karaoke = !l.line->words.empty();
    const float fade = kFadeWidth * F;
    const float dim = m_dim;
    double mainTotalW = 0.0, mainFilledW = 0.0;
    for (size_t wi = 0; wi < l.words.size(); ++wi) {
        auto& wv = l.words[wi];
        const float dur = static_cast<float>(std::max(1, wv.end - wv.start));
        const float p = karaoke ? Clamp01((ms - wv.start) / dur) : (l.active ? 1.0f : 0.0f);
        if (karaoke) {
            const double wordWeight = wv.width > 0 ? static_cast<double>(wv.width) : 1.0;
            mainTotalW += wordWeight;
            mainFilledW += wordWeight * p;
        }
        float lift = 0, grow = 1, glowA = 0, glowR = 0;
        if (karaoke) {
            if (wv.emphasize) {
                float du = std::max(1000.0f, dur);
                float amount = du / 2000, blur = du / 3000;
                amount = amount > 1 ? std::sqrt(amount) : amount * amount * amount;
                blur = blur > 1 ? std::sqrt(blur) : blur * blur * blur;
                amount *= 0.6f;
                blur *= 0.5f;
                if (wv.last) {
                    amount *= 1.6f;
                    blur *= 1.5f;
                    du *= 1.2f;
                }
                amount = std::min(1.2f, amount);
                blur = std::min(0.8f, blur);
                const float e = EmphasisCurve((ms - wv.start) / du);
                grow = 1 + e * 0.1f * amount;
                lift = -e * 0.025f * amount * F;
                const float fx = Clamp01((ms - (wv.start - 400)) / (du * 1.4f));
                lift += -std::sin(fx * 3.14159265f) * kFloat * F;
                glowA = e * blur;
                glowR = std::min(0.3f, blur * 0.3f) * F;
            } else {
                lift = -EaseOut((ms - wv.start) / std::max(1000.0f, dur)) * kFloat * F;
            }
            lift *= l.glow;
            grow = 1 + (grow - 1) * l.glow;
            glowA *= l.glow;
        }
        const D2D1_POINT_2F center{(wv.box.left + wv.box.right) / 2, wv.box.bottom};
        const D2D1_MATRIX_3X2_F wordM =
            D2D1::Matrix3x2F::Scale(grow, grow, center) * D2D1::Matrix3x2F::Translation(0, lift) * lineM;
        if (glowA > 0.01f && wv.glow) {
            com_ptr<ID2D1Effect> glowBlur, glowOpacity;
            if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, glowBlur.put())) &&
                SUCCEEDED(dc->CreateEffect(CLSID_D2D1Opacity, glowOpacity.put()))) {
                glowBlur->SetInput(0, wv.glow.get());
                glowBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::max(0.5f, glowR / 2));
                glowOpacity->SetInputEffect(0, glowBlur.get());
                glowOpacity->SetValue(D2D1_OPACITY_PROP_OPACITY, Clamp01(glowA));
                dc->SetTransform(wordM);
                com_ptr<ID2D1Image> out;
                glowOpacity->GetOutput(out.put());
                dc->DrawImage(out.get(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
            }
        }
        dc->SetTransform(wordM);
        const float brightA = Mix(dim, kBright, l.glow), darkA = Mix(dim, kUnsung, l.glow);
        for (const auto& r : l.runs) {
            if (r.word != static_cast<int>(wi)) continue;
            const auto run = r.Glyphs();
            if (!karaoke || p >= 1 || l.glow < 0.01f) {
                dc->DrawGlyphRun(r.origin, &run, White(dc, p >= 1 ? brightA : darkA).get());
                continue;
            }
            if (p <= 0) {
                dc->DrawGlyphRun(r.origin, &run, White(dc, darkA).get());
                continue;
            }
            // Fill edge, in this run's coordinates: it crosses the whole word plus the fade.
            const float edge = -fade / 2 + p * (wv.width + fade) - r.wordX;
            const float x0 = r.origin.x + edge - fade / 2, x1 = r.origin.x + edge + fade / 2;
            const D2D1_GRADIENT_STOP g[] = {{0, D2D1::ColorF(1, 1, 1, brightA)}, {1, D2D1::ColorF(1, 1, 1, darkA)}};
            com_ptr<ID2D1GradientStopCollection> gs;
            dc->CreateGradientStopCollection(g, 2, gs.put());
            com_ptr<ID2D1LinearGradientBrush> brush;
            dc->CreateLinearGradientBrush({{x0, 0}, {x1, 0}}, gs.get(), brush.put());
            dc->DrawGlyphRun(r.origin, &run, brush.get());
        }
    }
    dc->SetTransform(blockM);
    if (l.sub) {
        const auto subMode = settings::GetSecondaryLyricHighlight();
        const float subBaseA = kSub * dim / kDim;
        if (subMode == settings::SecondaryLyricHighlight::Off) {
            // Secondary line stays dim whether the line is active or not.
            dc->DrawTextLayout({0, l.subY}, l.sub.get(),
                               White(dc, Mix(subBaseA, kSub * 0.85f, l.glow)).get());
        } else if (subMode == settings::SecondaryLyricHighlight::Line || !karaoke || l.subRuns.empty()) {
            dc->DrawTextLayout({0, l.subY}, l.sub.get(), White(dc, Mix(subBaseA, kSubBright, l.glow)).get());
        } else {
            const float subP = mainTotalW > 0.0 ? Clamp01(static_cast<float>(mainFilledW / mainTotalW))
                                                : (l.active ? 1.0f : 0.0f);
            const float subBrightA = Mix(subBaseA, kSubBright, l.glow);
            const float subDarkA = Mix(subBaseA, kSubUnsung, l.glow);
            const float subF = F * settings::TranslationSizePercent() / std::max(1, settings::LyricSizePercent());
            const float subFade = kFadeWidth * subF;
            const D2D1_MATRIX_3X2_F subM = D2D1::Matrix3x2F::Translation(0, l.subY) * blockM;
            dc->SetTransform(subM);
            for (const auto& r : l.subRuns) {
                const auto run = r.Glyphs();
                if (subP >= 1.0f || l.glow < 0.01f) {
                    dc->DrawGlyphRun(r.origin, &run, White(dc, subP >= 1.0f ? subBrightA : subDarkA).get());
                    continue;
                }
                if (subP <= 0.0f) {
                    dc->DrawGlyphRun(r.origin, &run, White(dc, subDarkA).get());
                    continue;
                }
                const float edge = -subFade / 2 + subP * (l.subTotalWidth + subFade) - r.wordX;
                if (edge + subFade / 2 <= 0.0f) {
                    dc->DrawGlyphRun(r.origin, &run, White(dc, subDarkA).get());
                    continue;
                }
                if (edge - subFade / 2 >= r.width) {
                    dc->DrawGlyphRun(r.origin, &run, White(dc, subBrightA).get());
                    continue;
                }
                const float x0 = r.origin.x + edge - subFade / 2;
                const float x1 = r.origin.x + edge + subFade / 2;
                const D2D1_GRADIENT_STOP g[] = {{0, D2D1::ColorF(1, 1, 1, subBrightA)},
                                                {1, D2D1::ColorF(1, 1, 1, subDarkA)}};
                com_ptr<ID2D1GradientStopCollection> gs;
                dc->CreateGradientStopCollection(g, 2, gs.put());
                com_ptr<ID2D1LinearGradientBrush> brush;
                dc->CreateLinearGradientBrush({{x0, 0}, {x1, 0}}, gs.get(), brush.put());
                dc->DrawGlyphRun(r.origin, &run, brush.get());
            }
            dc->SetTransform(blockM);
        }
    }
}

void LyricsView::RenderLine(LineView& l, double now, float areaH) {
    const float F = FontSize(), pad = ImagePad();
    const float top = ContentTop();
    const float y = l.y.Position();
    // Far off screen: no visuals, no surface.
    if (y + l.height < top - areaH || y > top + 2 * areaH) {
        if (l.view) ReleaseLine(l);
        return;
    }
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    if (!l.view) {
        const float s = m_scale;
        l.view.Create(m_layer.Visual(), W, l.height, kPlateRadius * F, s);
        l.view.content.Offset(float3{std::round((kSideMargin + kTextInset * F - pad) * s),
                                     std::round((kLinePadding * F - pad) * s), 0});
        l.view.root.CenterPoint(float3{kSideMargin * s, l.height * s / 2, 0});
    }
    const bool visible = y + l.height >= top - 40 && y <= top + areaH + 40;
    l.view.root.IsVisible(visible);
    if (!visible) return;

    const bool synced = m_lyrics && m_lyrics->synced;
    // Live image while the line is (or is fading out of being) active; else the dim image.
    const bool live = synced && l.glow > 0.01f;
    const bool karaokeActive = live && !l.line->words.empty() &&
                               static_cast<int>(now * 1000) <= l.line->end + 1200;  // floats settle after the end
    if (live) {
        if (l.image != 1 || karaokeActive || std::abs(l.imageGlow - l.glow) > 0.002f) DrawLineImage(l, now, true);
    } else if (l.image != 0) {
        DrawLineImage(l, now, false);
    }

    const float s = m_scale;
    l.view.root.Offset(float3{0, std::round((y - top) * s), 0});
    const float k = l.scale.Position() / 100.0f * l.press.Position() / 100.0f;
    l.view.root.Scale(float3{k, k, 1});
    const bool pressed = m_pressed >= 0 && m_lines[static_cast<size_t>(m_pressed)].get() == &l;
    l.view.SetPlate(l.hover, pressed ? kPressAlpha : kHoverAlpha);
    l.view.content.Opacity(live ? 1.0f : (synced ? m_dim : kUnsynced) / 0.75f);

    // Blur on the GPU: an effect brush while it shows, the plain surface otherwise. A line that
    // has just been sung blurs while it rises and shrinks, not after its glow has faded
    // (owner, 2026-11-13): only the line being sung stays sharp.
    const float blur = l.active ? 0.0f : l.blur * s;
    if (blur > 0.15f) {
        if (!l.blurBrush) {
            l.blurBrush = m_blurFactory.CreateBrush();
            l.blurBrush.SetSourceParameter(L"source", l.surfaceBrush);
        }
        if (!l.blurred) {
            l.view.content.Brush(l.blurBrush);
            l.blurred = true;
        }
        if (std::abs(blur - l.shownBlur) > 0.01f) {
            l.blurBrush.Properties().InsertScalar(L"Blur.BlurAmount", blur);
            l.shownBlur = blur;
        }
    } else if (l.blurred) {
        l.view.content.Brush(l.surfaceBrush);
        l.blurred = false;
        l.shownBlur = -1;
    }
}

// Three dots that light up one after another through a long instrumental gap while the
// group breathes; at the end they swell and vanish.
void LyricsView::RenderInterlude(double now) {
    const bool show = m_lyrics && m_lyrics->synced && m_interludeShow >= 0.01f;
    m_dots.IsVisible(show);
    if (!show) return;
    const float F = FontSize(), s = m_scale;
    const float d = 0.3f * F, gap = 0.18f * F;
    const float x = kSideMargin + kTextInset * F;
    const float cy = m_interludeY.Position() - ContentTop() + 0.65f * F;
    const double span = std::max(0.001, m_interludeEnd - m_interludeStart);
    const float progress = Clamp01(static_cast<float>((now - m_interludeStart) / span));
    const float remaining = static_cast<float>((m_interludeEnd - now) * 1000.0);
    float scale = 1 + 0.08f * std::sin(static_cast<float>(now) * 2 * 3.14159265f / 4.0f);
    float alpha = m_interludeShow;
    if (remaining < 1000) {
        if (remaining > 250) scale *= Mix(1.25f, 1.0f, (remaining - 250) / 750);  // swell
        else {
            scale *= 1.25f * Clamp01(remaining / 250);  // and vanish
            alpha *= Clamp01(remaining / 250);
        }
    }
    const float groupW = 3 * d + 2 * gap;
    m_dots.Offset(float3{x * s, (cy - d / 2) * s, 0});
    m_dots.Size(float2{groupW * s, d * s});
    m_dots.CenterPoint(float3{groupW * s / 2, d * s / 2, 0});
    m_dots.Scale(float3{scale, scale, 1});
    m_dots.Opacity(alpha);
    size_t k = 0;
    for (auto child : m_dots.Children()) {
        auto shape = child.as<wuc::ShapeVisual>();
        shape.Offset(float3{k * (d + gap) * s, 0, 0});
        shape.Size(float2{d * s, d * s});
        auto ellipse = shape.Shapes().GetAt(0).as<wuc::CompositionSpriteShape>().Geometry().as<wuc::CompositionEllipseGeometry>();
        ellipse.Center(float2{d * s / 2, d * s / 2});
        ellipse.Radius(float2{d * s / 2, d * s / 2});
        const float lit = Clamp01(progress * 3 - static_cast<float>(k));
        m_dotBrush[k].Color(winrt::Windows::UI::ColorHelper::FromArgb(
            static_cast<uint8_t>(std::lround(Mix(0.25f, 0.9f, lit) * 255)), 255, 255, 255));
        ++k;
    }
}

void LyricsView::Render() {
    if (!m_visible) return;
    const float areaH = (m_bounds.bottom - m_bounds.top) / m_scale - ContentTop();
    const bool found = m_status == lyrics::Status::Found;
    m_layer.Visual().IsVisible(found);
    if (!found) {
        m_plateGlass.Hide();
        return;
    }
    const double now = Now();
    for (auto& l : m_lines) RenderLine(*l, now, areaH);
    RenderInterlude(now);
    // The glass under the plate showing most, transformed as its line (root scaled round the
    // plate's left middle), in the root's space: the layer sits ContentTop() down.
    const LineView* best = nullptr;
    for (const auto& l : m_lines)
        if (l->view && l->view.root.IsVisible() && l->hover > (best ? best->hover : 0.01f)) best = l.get();
    if (!best) {
        m_plateGlass.Hide();
        return;
    }
    const float s = m_scale, top = ContentTop();
    const float W = (m_bounds.right - m_bounds.left) / s;
    const float pw = (W - 2 * kSideMargin) * s, ph = best->height * s;
    const float k = best->scale.Position() / 100.0f * best->press.Position() / 100.0f;
    const float y = std::round(top * s) + std::round((best->y.Position() - top) * s);
    m_plateGlass.Show(float3{kSideMargin * s, y, 0}, float2{pw, ph}, kPlateRadius * FontSize() * s, k,
                      float2{0, ph / 2}, best->hover, s);
}

// ---- Input ------------------------------------------------------------------------------

bool LyricsView::HitTest(POINT pt) const { return m_visible && PtInRect(&m_bounds, pt) != FALSE; }

int LyricsView::LineAt(POINT pt) const {
    const float x = (pt.x - m_bounds.left) / m_scale, y = (pt.y - m_bounds.top) / m_scale;
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    if (y < ContentTop() || x < kSideMargin || x > W - kSideMargin) return -1;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const float top = m_lines[i]->y.Position();
        if (y >= top && y < top + m_lines[i]->height) return static_cast<int>(i);
    }
    return -1;
}

void LyricsView::OnMouseMove(POINT pt) {
    m_hoverInside = true;
    m_pointer = pt;
    if (m_drag == Drag::Pending) {
        if (std::abs(pt.y - m_dragStart.y) > 4 * m_scale || std::abs(pt.x - m_dragStart.x) > 4 * m_scale) {
            m_drag = Drag::Scroll;
            if (!m_browsing) {
                m_browsing = true;
                m_scroll = 0;
                UpdateTargets(false, false, false);  // freezes the layout where it is
            }
            m_dragOffset0 = m_scroll;
            m_fling.Reset();
            if (m_pressed >= 0) m_lines[static_cast<size_t>(m_pressed)]->press.SetTarget(100);
            m_pressed = -1;
            m_hover = -1;
        }
    }
    if (m_drag == Drag::Scroll) {
        m_scroll = m_dragOffset0 + (pt.y - m_dragStart.y) / m_scale;
        m_fling.Sample(m_scroll);
        UpdateTargets(false, true, false);
        StartFrames();
        return;
    }
    const int hover = (m_lyrics && m_lyrics->synced) ? LineAt(pt) : -1;
    if (hover != m_hover) {
        m_hover = hover;
        StartFrames();
    }
}

void LyricsView::OnMouseLeave() {
    m_hoverInside = false;
    if (m_drag != Drag::None) return;
    if (m_hover != -1) {
        m_hover = -1;
        StartFrames();
    }
}

void LyricsView::OnLButtonDown(POINT pt) {
    m_drag = Drag::Pending;
    m_dragStart = pt;
    m_velocity = 0;
    m_pressed = (m_lyrics && m_lyrics->synced) ? LineAt(pt) : -1;
    if (m_pressed >= 0) m_lines[static_cast<size_t>(m_pressed)]->press.SetTarget(kPressScale);
    StartFrames();
}

void LyricsView::OnLButtonUp(POINT pt) {
    if (m_drag == Drag::Scroll) {
        // Inertia from the last ~100 ms of the drag.
        m_velocity = m_fling.ReleaseVelocity();
        m_drag = Drag::None;
        m_interactionEnd = Clock::now();
        StartFrames();
        return;
    }
    m_drag = Drag::None;
    const int pressed = m_pressed;
    m_pressed = -1;
    if (pressed >= 0) {
        m_lines[static_cast<size_t>(pressed)]->press.SetTarget(100);
        if (LineAt(pt) == pressed && onSeek) {
            m_browsing = false;
            m_scroll = 0;
            onSeek(m_lines[static_cast<size_t>(pressed)]->line->start / 1000.0 + 0.001);
        }
    }
    StartFrames();
}

void LyricsView::OnCaptureLost() {
    if (m_drag == Drag::Scroll) m_interactionEnd = Clock::now();
    m_drag = Drag::None;
    if (m_pressed >= 0) m_lines[static_cast<size_t>(m_pressed)]->press.SetTarget(100);
    m_pressed = -1;
    StartFrames();
}

void LyricsView::OnWheel(int delta) {
    if (m_lines.empty()) return;
    if (!m_browsing) {
        m_browsing = true;
        m_scroll = 0;
    }
    m_velocity = 0;
    m_scroll += WheelNotches(delta) * 90.0f;
    // Keep the first line reachable at the anchor and the last one too.
    float total = 0;
    for (const auto& l : m_lines) total += l->height;
    m_scroll = std::clamp(m_scroll, -total, total);
    m_interactionEnd = Clock::now();
    m_hover = -1;
    UpdateTargets(false, false, false);
    StartFrames();
}


}  // namespace glasswave::ui
