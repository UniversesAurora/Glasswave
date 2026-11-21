#include "pch.h"
#include "ui/cover_tint.h"
#include "gfx/composition_host.h"

#include <array>

namespace glasswave::ui {

namespace {

struct Hsv {
    float h = 0, s = 0, v = 0; // h 0..360
};

Hsv ToHsv(float r, float g, float b) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsv o;
    o.v = mx;
    o.s = mx > 0 ? d / mx : 0;
    if (d <= 0) return o;
    if (mx == r)
        o.h = 60 * std::fmod((g - b) / d, 6.0f);
    else if (mx == g)
        o.h = 60 * ((b - r) / d + 2);
    else
        o.h = 60 * ((r - g) / d + 4);
    if (o.h < 0) o.h += 360;
    return o;
}

uint32_t FromHsv(const Hsv& c) {
    const float k = c.v * c.s, x = k * (1 - std::fabs(std::fmod(c.h / 60, 2.0f) - 1)), m = c.v - k;
    float r = 0, g = 0, b = 0;
    if (c.h < 60) r = k, g = x;
    else if (c.h < 120) r = x, g = k;
    else if (c.h < 180) g = k, b = x;
    else if (c.h < 240) g = x, b = k;
    else if (c.h < 300) r = x, b = k;
    else r = k, b = x;
    const auto byte = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255 + 0.5f); };
    return (byte(r + m) << 16) | (byte(g + m) << 8) | byte(b + m);
}

}  // namespace

std::optional<uint32_t> CoverTint(IWICBitmapSource* art) {
    if (!art) return std::nullopt;
    // 32 x 32 is plenty to find where the colour lies.
    constexpr UINT kSide = 32;
    auto* wic = gfx::CompositionHost::Get().Wic();
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> conv;
    std::array<uint32_t, kSide * kSide> px{};
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(art, kSide, kSide, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(scaler.get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->CopyPixels(nullptr, kSide * 4, static_cast<UINT>(px.size() * 4),
                                reinterpret_cast<BYTE*>(px.data()))))
        return std::nullopt;

    // Hue histogram weighted by how vivid each pixel is; the winning hue's pixels averaged.
    constexpr int kBins = 24;
    std::array<float, kBins> weight{};
    std::array<std::array<float, 3>, kBins> sum{};
    float grey[3] = {}, greyWeight = 0, vividWeight = 0;
    for (uint32_t p : px) {
        const float r = ((p >> 16) & 0xFF) / 255.0f, g = ((p >> 8) & 0xFF) / 255.0f, b = (p & 0xFF) / 255.0f;
        const Hsv c = ToHsv(r, g, b);
        grey[0] += r, grey[1] += g, grey[2] += b, greyWeight += 1;
        if (c.s < 0.18f || c.v < 0.16f) continue; // greys and near-black say little about the hue
        const float w = c.s * c.v * c.s;
        const int bin = std::min(kBins - 1, static_cast<int>(c.h / 360 * kBins));
        weight[bin] += w;
        sum[bin][0] += r * w, sum[bin][1] += g * w, sum[bin][2] += b * w;
        vividWeight += w;
    }
    Hsv out;
    if (vividWeight < 0.6f) { // a black-and-white picture: its grey, as Win7's Slate or Frost
        out = ToHsv(grey[0] / greyWeight, grey[1] / greyWeight, grey[2] / greyWeight);
        out.s = std::min(out.s, 0.12f);
        out.v = std::clamp(out.v, 0.33f, 0.87f);
        return FromHsv(out);
    }
    int best = 0;
    float bestWeight = -1;
    for (int i = 0; i < kBins; ++i) { // a bin together with its neighbours: hues straddle bins
        const float w = weight[i] + 0.5f * (weight[(i + 1) % kBins] + weight[(i + kBins - 1) % kBins]);
        if (w > bestWeight) bestWeight = w, best = i;
    }
    float rgb[3] = {};
    float total = 0;
    for (int d = -1; d <= 1; ++d) {
        const int i = (best + d + kBins) % kBins;
        for (int k = 0; k < 3; ++k) rgb[k] += sum[i][k];
        total += weight[i];
    }
    out = ToHsv(rgb[0] / total, rgb[1] / total, rgb[2] / total);
    // Win7's swatches are clear, fairly bright colours: lift a dull or dark average to them.
    out.s = std::clamp(out.s * 1.15f, 0.35f, 0.95f);
    out.v = std::clamp(out.v * 1.1f, 0.55f, 0.98f);
    return FromHsv(out);
}

}  // namespace glasswave::ui
