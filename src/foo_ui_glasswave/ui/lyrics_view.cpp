#include "pch.h"

#include "ui/lyrics_view.h"

#include "gfx/composition_host.h"
#include "ui/i18n.h"

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using Clock = std::chrono::steady_clock;

namespace {

// ---- Tuning (docs/design/lyrics-view.md) -----------------------------------------------
constexpr float kHeaderHeight = 84;      // DIP: cover, title and artist above the lyrics
constexpr float kCover = 52;             // DIP
constexpr float kSideMargin = 12;        // DIP: hover plate inset from the pane edge
constexpr float kTextInset = 0.45f;      // em: text inset inside the hover plate
constexpr float kLinePadding = 0.4f;     // em: above and below each line block
constexpr float kSubGap = 0.3f;          // em: main line to translation
constexpr float kSubSize = 0.55f;        // em: translation size
constexpr float kAlign = 0.35f;          // focused line's top, fraction of the lyrics area
constexpr float kFadeWidth = 0.5f;       // em: soft edge of the word fill
constexpr float kFloat = 0.05f;          // em: how far a sung word rises
constexpr float kPlateRadius = 0.35f;    // em
constexpr float kHoverAlpha = 0.09f, kPressAlpha = 0.05f;
constexpr float kInactiveScale = 97;     // percent, lines other than the active one while playing
constexpr float kPressScale = 95;        // percent, a pressed line
constexpr float kMaxBlur = 5;            // DIP
constexpr float kBlurFactor = 0.8f;      // AMLL's narrow-viewport factor (our pane is always narrow)
constexpr int kInterludeGapMs = 7000;    // gaps at least this long show the breathing dots
constexpr float kTopFade = 36, kBottomFade = 72;  // DIP
constexpr float kPaneTint = 0.30f;       // black over the glass so white text reads

// Text alphas (white).
constexpr float kDim = 0.24f;            // a line that is not active
constexpr float kDimBrowsing = 0.45f;    // while the user scrolls the lines
constexpr float kSub = 0.32f;            // translations
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
float Bezier(float x1, float y1, float x2, float y2, float x) {
    auto bx = [&](float t) { return 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t; };
    auto by = [&](float t) { return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t; };
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 24; ++i) {
        const float v = bx(t);
        if (std::abs(v - x) < 1e-4f) break;
        if (v < x) lo = t;
        else hi = t;
        t = (lo + hi) / 2;
    }
    return by(t);
}
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

std::wstring DefaultLocale() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) return name;
    return L"en-us";
}

std::wstring PickFamily(IDWriteFactory* f) {
    com_ptr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(f->GetSystemFontCollection(fonts.put()))) {
        for (const wchar_t* family : {L"Segoe UI Variable Display", L"Segoe UI"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) return family;
        }
    }
    return L"Segoe UI";
}

com_ptr<IDWriteTextFormat> MakeFormat(float size, DWRITE_FONT_WEIGHT weight) {
    auto* f = gfx::CompositionHost::Get().DWrite();
    static const std::wstring family = PickFamily(f);
    static const std::wstring locale = DefaultLocale();
    com_ptr<IDWriteTextFormat> format;
    winrt::check_hresult(f->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, size, locale.c_str(), format.put()));
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return format;
}

com_ptr<ID2D1SolidColorBrush> White(ID2D1DeviceContext* dc, float alpha) {
    com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, alpha), b.put());
    return b;
}

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
    std::vector<Run> runs;
    struct WordView {
        int start = 0, end = 0;
        bool emphasize = false, last = false;
        float width = 0;               // sum of its runs
        D2D1_RECT_F box{};             // union of its runs, relative to the text origin
        com_ptr<ID2D1CommandList> glow;  // the word in white, blurred into its glow
        com_ptr<ID2D1Effect> glowBlur, glowOpacity;
    };
    std::vector<WordView> words;
    float mainH = 0, subH = 0, height = 0;
    Spring y{0}, scale{100}, press{100};
    float blur = 0, blurTarget = 0;
    float glow = 0;  // 0 inactive .. 1 active (bright layer)
    float hover = 0;
    bool active = false;
    com_ptr<ID2D1CommandList> image;  // dim look, for the blurred path
    com_ptr<ID2D1Effect> blurFx, opacityFx;
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
    m_visual = host.Compositor().CreateSpriteVisual();
    m_visual.IsVisible(false);
    parent.Children().InsertAtTop(m_visual);
    m_dc = host.CreateDeviceContext();
}

LyricsView::~LyricsView() {
    if (m_visual) {
        if (auto parent = m_visual.Parent()) parent.Children().Remove(m_visual);
    }
}

void LyricsView::SetBounds(const RECT& px, float scale) {
    const bool sizeChanged = px.right - px.left != m_bounds.right - m_bounds.left ||
                             px.bottom - px.top != m_bounds.bottom - m_bounds.top || scale != m_scale;
    m_bounds = px;
    m_scale = scale;
    m_visual.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    m_visual.Size(float2{static_cast<float>(std::max<LONG>(0, px.right - px.left)),
                         static_cast<float>(std::max<LONG>(0, px.bottom - px.top))});
    if (sizeChanged && m_visible) {
        Relayout();
        UpdateTargets(false, true, false);
        Paint();
    }
}

void LyricsView::SetVisible(bool visible) {
    if (visible == m_visible) return;
    m_visible = visible;
    m_visual.IsVisible(visible);
    if (visible) {
        Relayout();
        m_lastNow = Now();
        m_focus = -2;  // recomputed by Tick
        m_lastFrame = Clock::now();
        Tick();
        for (auto& l : m_lines) {
            l->y.Snap(l->y.Target());
            l->scale.Snap(l->scale.Target());
            l->blur = l->blurTarget;
        }
        Paint();
        StartFrames();
    }
}

void LyricsView::SetTrack(const std::wstring& title, const std::wstring& artist, const com_ptr<IWICBitmapSource>& art) {
    m_title = title;
    m_artist = artist;
    if (art.get() != m_art.get()) m_artBitmap = nullptr;
    m_art = art;
    if (m_visible) Paint();
}

void LyricsView::SetResult(const lyrics::Result& result) {
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
    // New lyrics fly in from below, line after line.
    m_lastNow = Now();
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
    if (m_visible) {
        UpdateTargets(false, false, true);
        StartFrames();
    }
}

void LyricsView::StartFrames() {
    m_lastFrame = Clock::now();
    if (onFrames) onFrames();
}

double LyricsView::Now() const {
    try {
        auto pc = playback_control::get();
        if (pc->is_playing()) return pc->playback_get_position();
    } catch (...) {
    }
    return 0;
}

float LyricsView::FontSize() const {
    const float w = (m_bounds.right - m_bounds.left) / m_scale;
    return std::clamp(std::round(w * 0.075f), 20.0f, 42.0f);
}

float LyricsView::ContentTop() const { return kHeaderHeight; }

float LyricsView::AnchorY() const {
    const float H = (m_bounds.bottom - m_bounds.top) / m_scale;
    return ContentTop() + (H - ContentTop()) * kAlign;
}

// ---- Layout -----------------------------------------------------------------------------

void LyricsView::Relayout() {
    m_lines.clear();
    m_dc->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    if (W <= 0 || !m_lyrics) return;
    const float F = FontSize();
    m_layoutWidth = std::max(40.0f, W - 2 * kSideMargin - 2 * kTextInset * F);
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
    static thread_local com_ptr<IDWriteTextFormat> mainFormat, subFormat;
    if (cachedSize != F) {
        mainFormat = MakeFormat(F, DWRITE_FONT_WEIGHT_BOLD);
        subFormat = MakeFormat(F * kSubSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        cachedSize = F;
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
    if (!line.translation.empty()) {
        winrt::check_hresult(f->CreateTextLayout(line.translation.c_str(), static_cast<UINT32>(line.translation.size()),
                                                 subFormat.get(), width, 100000.0f, v.sub.put()));
        DWRITE_TEXT_METRICS sm{};
        v.sub->GetMetrics(&sm);
        v.subH = sm.height;
    }
    v.height = 2 * kLinePadding * F + v.mainH + (v.sub ? kSubGap * F + v.subH : 0);
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
        // Blur grows with the distance from the focus (lines above count one more).
        float level = 0;
        if (synced && !l.active && !m_browsing) {
            const int fi = std::max(m_focus, 0);
            const int idx = static_cast<int>(i);
            const int distance = idx < fi ? fi - idx + 1 : idx - fi;
            level = std::min(kMaxBlur, (1.0f + distance) * kBlurFactor);
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
    if (m_lyrics && m_lyrics->synced && m_playing && !m_paused) return true;
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
    return false;
}

bool LyricsView::Tick() {
    if (!m_visible) return false;
    const auto frame = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(frame - m_lastFrame).count(), 0.0f, 0.1f);
    m_lastFrame = frame;
    const double now = Now();
    const bool synced = m_lyrics && m_lyrics->synced && !m_lines.empty();

    if (synced) {
        // Jumps in time (seeking, a click on a line) move without the stagger, slowly.
        const double expected = m_lastNow + ((m_playing && !m_paused) ? dt : 0);
        const bool seek = std::abs(now - expected) > 0.6;
        m_lastNow = now;
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
        m_velocity *= std::pow(0.95f, dt * 60.0f);
        if (std::abs(m_velocity) < 30) {
            m_velocity = 0;
            m_interactionEnd = frame;
        }
        UpdateTargets(false, true, false);
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
    Paint();
    return NeedsFrames();
}

// ---- Painting ---------------------------------------------------------------------------

void LyricsView::EnsureCommandList(ID2D1DeviceContext* dc, LineView& l) {
    if (l.image) return;
    const float F = FontSize();
    com_ptr<ID2D1CommandList> list;
    if (FAILED(dc->CreateCommandList(list.put()))) return;
    dc->SetTarget(list.get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // Main text at 0.75 and the translation at 1: the opacity effect scales both.
    auto mainInk = White(dc, 0.75f), subInk = White(dc, 0.75f * kSub / kDim);
    dc->DrawTextLayout({0, 0}, l.main.get(), mainInk.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    if (l.sub) dc->DrawTextLayout({0, l.mainH + kSubGap * F}, l.sub.get(), subInk.get());
    dc->EndDraw();
    list->Close();
    dc->SetTarget(nullptr);
    l.image = list;
    dc->CreateEffect(CLSID_D2D1GaussianBlur, l.blurFx.put());
    dc->CreateEffect(CLSID_D2D1Opacity, l.opacityFx.put());
    if (l.blurFx && l.opacityFx) {
        l.blurFx->SetInput(0, l.image.get());
        l.blurFx->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
        l.opacityFx->SetInputEffect(0, l.blurFx.get());
    }
    // Glow images for the emphasized words.
    for (size_t w = 0; w < l.words.size(); ++w) {
        auto& wv = l.words[w];
        if (!wv.emphasize || wv.glow) continue;
        com_ptr<ID2D1CommandList> g;
        if (FAILED(dc->CreateCommandList(g.put()))) continue;
        dc->SetTarget(g.get());
        dc->BeginDraw();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        auto ink = White(dc, 1.0f);
        for (const auto& r : l.runs) {
            if (r.word != static_cast<int>(w)) continue;
            const auto run = r.Glyphs();
            dc->DrawGlyphRun(r.origin, &run, ink.get());
        }
        dc->EndDraw();
        g->Close();
        dc->SetTarget(nullptr);
        wv.glow = g;
        dc->CreateEffect(CLSID_D2D1GaussianBlur, wv.glowBlur.put());
        dc->CreateEffect(CLSID_D2D1Opacity, wv.glowOpacity.put());
        if (wv.glowBlur && wv.glowOpacity) {
            wv.glowBlur->SetInput(0, g.get());
            wv.glowOpacity->SetInputEffect(0, wv.glowBlur.get());
        }
    }
}

void LyricsView::Paint() {
    if (!m_visible) return;
    const int w = std::max<int>(1, m_bounds.right - m_bounds.left), h = std::max<int>(1, m_bounds.bottom - m_bounds.top);
    auto& host = gfx::CompositionHost::Get();
    if (!m_surface || m_surfaceSize.cx != w || m_surfaceSize.cy != h) {
        m_surface = host.CreateSurface(w, h);
        m_surfaceSize = {w, h};
        auto brush = host.Compositor().CreateSurfaceBrush(m_surface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        m_visual.Brush(brush);
    }
    // Command lists are recorded outside the surface's draw.
    m_dc->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    for (auto& l : m_lines) EnsureCommandList(m_dc.get(), *l);
    host.Draw(m_surface, m_scale, m_dc.get(), [&](ID2D1DeviceContext* dc) {
        const float W = w / m_scale, H = h / m_scale;
        dc->FillRectangle({0, 0, W, H}, White(dc, 0).get());
        com_ptr<ID2D1SolidColorBrush> tint;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, kPaneTint), tint.put());
        dc->FillRectangle({0, 0, W, H}, tint.get());
        PaintHeader(dc);
        switch (m_status) {
        case lyrics::Status::None: PaintMessage(dc, m_title.empty() ? Tr(L"Nothing playing") : L""); break;
        case lyrics::Status::Searching: PaintMessage(dc, Tr(L"Searching for lyrics…")); break;
        case lyrics::Status::NotFound: PaintMessage(dc, Tr(L"No lyrics found")); break;
        case lyrics::Status::Found: PaintLines(dc); break;
        }
    });
}

void LyricsView::PaintHeader(ID2D1DeviceContext* dc) {
    if (m_title.empty()) return;
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    const float x0 = kSideMargin + 8, y0 = (kHeaderHeight - kCover) / 2;
    float textX = x0;
    if (m_art && !m_artBitmap) dc->CreateBitmapFromWicBitmap(m_art.get(), nullptr, m_artBitmap.put());
    if (m_artBitmap) {
        const D2D1_RECT_F box{x0, y0, x0 + kCover, y0 + kCover};
        com_ptr<ID2D1Factory> factory;
        dc->GetFactory(factory.put());
        com_ptr<ID2D1RoundedRectangleGeometry> clip;
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box, 6, 6), clip.put());
        // Soft shadow below the cover.
        com_ptr<ID2D1SolidColorBrush> shade;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.18f), shade.put());
        dc->FillRoundedRectangle(D2D1::RoundedRect({box.left, box.top + 2, box.right, box.bottom + 2}, 7, 7), shade.get());
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get()), nullptr);
        const D2D1_SIZE_F s = m_artBitmap->GetSize();
        const float k = std::max(kCover / std::max(1.0f, s.width), kCover / std::max(1.0f, s.height));
        const float dw = s.width * k, dh = s.height * k;
        const D2D1_RECT_F dst{box.left + (kCover - dw) / 2, box.top + (kCover - dh) / 2, box.left + (kCover + dw) / 2,
                              box.top + (kCover + dh) / 2};
        dc->DrawBitmap(m_artBitmap.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        dc->PopLayer();
        textX = box.right + 14;
    }
    auto* f = gfx::CompositionHost::Get().DWrite();
    const float textW = std::max(10.0f, W - textX - kSideMargin - 8);
    auto line = [&](const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight, float y, float alpha) {
        if (text.empty()) return;
        auto format = MakeFormat(size, weight);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        com_ptr<IDWriteTextLayout> layout;
        if (FAILED(f->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), textW, size * 1.5f,
                                       layout.put())))
            return;
        com_ptr<IDWriteInlineObject> ellipsis;
        f->CreateEllipsisTrimmingSign(layout.get(), ellipsis.put());
        const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trim, ellipsis.get());
        dc->DrawTextLayout({textX, y}, layout.get(), White(dc, alpha).get());
    };
    const float mid = kHeaderHeight / 2;
    line(m_title, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, mid - 22, 0.95f);
    line(m_artist, 14, DWRITE_FONT_WEIGHT_NORMAL, mid + 1, 0.6f);
}

void LyricsView::PaintMessage(ID2D1DeviceContext* dc, const std::wstring& text) {
    if (text.empty()) return;
    const float W = (m_bounds.right - m_bounds.left) / m_scale, H = (m_bounds.bottom - m_bounds.top) / m_scale;
    auto format = MakeFormat(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    com_ptr<IDWriteTextLayout> layout;
    auto* f = gfx::CompositionHost::Get().DWrite();
    if (FAILED(f->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), W - 40, 200,
                                   layout.put())))
        return;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    const float top = ContentTop() + (H - ContentTop() - tm.height) / 2 - 20;
    dc->DrawTextLayout({20, top}, layout.get(), White(dc, 0.55f).get());
}

void LyricsView::PaintLines(ID2D1DeviceContext* dc) {
    const float W = (m_bounds.right - m_bounds.left) / m_scale, H = (m_bounds.bottom - m_bounds.top) / m_scale;
    const D2D1_MATRIX_3X2_F base = [&] {
        D2D1_MATRIX_3X2_F m;
        dc->GetTransform(&m);
        return m;
    }();
    // Lyrics fade out under the header and at the bottom edge.
    com_ptr<ID2D1GradientStopCollection> stops;
    const float top = ContentTop();
    const D2D1_GRADIENT_STOP s[] = {{0.0f, D2D1::ColorF(0, 0, 0, 0)},
                                    {kTopFade / std::max(1.0f, H - top), D2D1::ColorF(0, 0, 0, 1)},
                                    {1.0f - kBottomFade / std::max(1.0f, H - top), D2D1::ColorF(0, 0, 0, 1)},
                                    {1.0f, D2D1::ColorF(0, 0, 0, 0)}};
    dc->CreateGradientStopCollection(s, 4, stops.put());
    com_ptr<ID2D1LinearGradientBrush> mask;
    dc->CreateLinearGradientBrush({{0, top}, {0, H}}, stops.get(), mask.put());
    D2D1_LAYER_PARAMETERS1 layer = D2D1::LayerParameters1(D2D1::RectF(0, top, W, H), nullptr,
                                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f,
                                                          mask.get());
    dc->PushLayer(layer, nullptr);
    const double now = Now();
    for (auto& l : m_lines) {
        const float y = l->y.Position();
        if (y + l->height < top - 40 || y > H + 40) continue;
        PaintLine(dc, *l, now);
        dc->SetTransform(base);
    }
    if (m_lyrics && m_lyrics->synced) PaintInterlude(dc, now);
    dc->SetTransform(base);
    dc->PopLayer();
}

void LyricsView::PaintLine(ID2D1DeviceContext* dc, LineView& l, double now) {
    const float F = FontSize();
    const float W = (m_bounds.right - m_bounds.left) / m_scale;
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    const float y = l.y.Position();
    const float s = l.scale.Position() / 100.0f * l.press.Position() / 100.0f;
    const D2D1_POINT_2F pivot{kSideMargin, y + l.height / 2};
    const D2D1_MATRIX_3X2_F lineM = D2D1::Matrix3x2F::Scale(s, s, pivot) * base;
    dc->SetTransform(lineM);

    // Hover plate (pressed: fainter).
    if (l.hover > 0.001f) {
        const bool pressed = m_pressed >= 0 && m_lines[static_cast<size_t>(m_pressed)].get() == &l;
        const float a = (pressed ? kPressAlpha : kHoverAlpha) * l.hover;
        const D2D1_RECT_F plate{kSideMargin, y, W - kSideMargin, y + l.height};
        dc->FillRoundedRectangle(D2D1::RoundedRect(plate, kPlateRadius * F, kPlateRadius * F), White(dc, a).get());
    }

    const float textX = kSideMargin + kTextInset * F, textY = y + kLinePadding * F;
    const bool synced = m_lyrics && m_lyrics->synced;
    const float dim = m_browsing ? kDimBrowsing : kDim;

    if (!synced) {
        dc->DrawTextLayout({textX, textY}, l.main.get(), White(dc, kUnsynced).get());
        if (l.sub) dc->DrawTextLayout({textX, textY + l.mainH + kSubGap * F}, l.sub.get(), White(dc, kUnsynced * 0.6f).get());
        return;
    }

    // Far from the focus: the recorded dim image, blurred.
    if (l.blur > 0.15f && l.glow < 0.01f && l.blurFx && l.opacityFx) {
        l.blurFx->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, l.blur);
        l.opacityFx->SetValue(D2D1_OPACITY_PROP_OPACITY, dim / 0.75f);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(textX, textY) * lineM);
        com_ptr<ID2D1Image> out;
        l.opacityFx->GetOutput(out.put());
        dc->DrawImage(out.get());
        return;
    }

    // Live: glyph runs with per-word fill, float and emphasis.
    const int ms = static_cast<int>(now * 1000);
    const bool karaoke = !l.line->words.empty();
    const float fade = kFadeWidth * F;
    for (size_t wi = 0; wi < l.words.size(); ++wi) {
        auto& wv = l.words[wi];
        const float dur = static_cast<float>(std::max(1, wv.end - wv.start));
        const float p = karaoke ? Clamp01((ms - wv.start) / dur) : (l.active ? 1.0f : 0.0f);
        // Float: sung words rise a little and stay up while the line is active.
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
        const D2D1_POINT_2F center{textX + (wv.box.left + wv.box.right) / 2, textY + wv.box.bottom};
        const D2D1_MATRIX_3X2_F wordM =
            D2D1::Matrix3x2F::Scale(grow, grow, center) * D2D1::Matrix3x2F::Translation(0, lift) * lineM;
        if (glowA > 0.01f && wv.glowBlur && wv.glowOpacity) {
            wv.glowBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::max(0.5f, glowR / 2));
            wv.glowOpacity->SetValue(D2D1_OPACITY_PROP_OPACITY, Clamp01(glowA));
            dc->SetTransform(D2D1::Matrix3x2F::Translation(textX, textY) * wordM);
            com_ptr<ID2D1Image> out;
            wv.glowOpacity->GetOutput(out.put());
            dc->DrawImage(out.get(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
        }
        dc->SetTransform(wordM);
        // Colors: dim when inactive; when active, bright up to the fill edge and darker after.
        const float brightA = Mix(dim, kBright, l.glow), darkA = Mix(dim, kUnsung, l.glow);
        for (const auto& r : l.runs) {
            if (r.word != static_cast<int>(wi)) continue;
            const D2D1_POINT_2F origin{textX + r.origin.x, textY + r.origin.y};
            const auto run = r.Glyphs();
            if (!karaoke || p >= 1 || l.glow < 0.01f) {
                dc->DrawGlyphRun(origin, &run, White(dc, p >= 1 ? brightA : darkA).get());
                continue;
            }
            if (p <= 0) {
                dc->DrawGlyphRun(origin, &run, White(dc, darkA).get());
                continue;
            }
            // Fill edge, in this run's coordinates: it crosses the whole word plus the fade.
            const float edge = -fade / 2 + p * (wv.width + fade) - r.wordX;
            const float x0 = origin.x + edge - fade / 2, x1 = origin.x + edge + fade / 2;
            const D2D1_GRADIENT_STOP g[] = {{0, D2D1::ColorF(1, 1, 1, brightA)}, {1, D2D1::ColorF(1, 1, 1, darkA)}};
            com_ptr<ID2D1GradientStopCollection> gs;
            dc->CreateGradientStopCollection(g, 2, gs.put());
            com_ptr<ID2D1LinearGradientBrush> brush;
            dc->CreateLinearGradientBrush({{x0, 0}, {x1, 0}}, gs.get(), brush.put());
            dc->DrawGlyphRun(origin, &run, brush.get());
        }
    }
    dc->SetTransform(lineM);
    if (l.sub) {
        // Translations stay at the same level whether the line is active or not.
        dc->DrawTextLayout({textX, textY + l.mainH + kSubGap * F}, l.sub.get(),
                           White(dc, Mix(kSub * dim / kDim, kSub * 0.85f, l.glow)).get());
    }
}

// Three dots that light up one after another through a long instrumental gap while the
// group breathes; at the end they swell and vanish.
void LyricsView::PaintInterlude(ID2D1DeviceContext* dc, double now) {
    if (m_interludeShow < 0.01f) return;
    const float F = FontSize();
    const float y = m_interludeY.Position();
    const float d = 0.3f * F, gap = 0.18f * F;
    const float x = kSideMargin + kTextInset * F;
    const float cy = y + 0.65f * F;
    const double span = std::max(0.001, m_interludeEnd - m_interludeStart);
    const float progress = static_cast<float>(Clamp01(static_cast<float>((now - m_interludeStart) / span)));
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
    const D2D1_POINT_2F center{x + (3 * d + 2 * gap) / 2, cy};
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) * base);
    for (int k = 0; k < 3; ++k) {
        const float lit = Clamp01(progress * 3 - k);
        const float a = Mix(0.25f, 0.9f, lit) * alpha;
        const float cx = x + d / 2 + k * (d + gap);
        dc->FillEllipse(D2D1::Ellipse({cx, cy}, d / 2, d / 2), White(dc, a).get());
    }
    dc->SetTransform(base);
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
    if (m_drag == Drag::Pending) {
        if (std::abs(pt.y - m_dragStart.y) > 4 * m_scale || std::abs(pt.x - m_dragStart.x) > 4 * m_scale) {
            m_drag = Drag::Scroll;
            if (!m_browsing) {
                m_browsing = true;
                m_scroll = 0;
                UpdateTargets(false, false, false);  // freezes the layout where it is
            }
            m_dragOffset0 = m_scroll;
            m_dragTrail.clear();
            if (m_pressed >= 0) m_lines[static_cast<size_t>(m_pressed)]->press.SetTarget(100);
            m_pressed = -1;
            m_hover = -1;
        }
    }
    if (m_drag == Drag::Scroll) {
        m_scroll = m_dragOffset0 + (pt.y - m_dragStart.y) / m_scale;
        m_dragTrail.emplace_back(Clock::now(), m_scroll);
        if (m_dragTrail.size() > 8) m_dragTrail.erase(m_dragTrail.begin());
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
        m_velocity = 0;
        if (m_dragTrail.size() >= 2) {
            const auto& a = m_dragTrail.front();
            const auto& b = m_dragTrail.back();
            const float secs = std::chrono::duration<float>(b.first - a.first).count();
            const float age = std::chrono::duration<float>(Clock::now() - b.first).count();
            if (secs > 0.005f && age < 0.08f) m_velocity = (b.second - a.second) / secs;
            if (std::abs(m_velocity) < 50) m_velocity = 0;
        }
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
    m_scroll += delta / 120.0f * 90.0f;
    // Keep the first line reachable at the anchor and the last one too.
    float total = 0;
    for (const auto& l : m_lines) total += l->height;
    m_scroll = std::clamp(m_scroll, -total, total);
    m_interactionEnd = Clock::now();
    m_hover = -1;
    UpdateTargets(false, false, false);
    StartFrames();
}

void LyricsView::EndInteraction() { m_interactionEnd = Clock::now(); }

}  // namespace glasswave::ui
