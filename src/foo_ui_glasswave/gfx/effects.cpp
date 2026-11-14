#include "pch.h"
#include "gfx/effects.h"

namespace glasswave::gfx {

namespace abi_fx = ABI::Windows::Graphics::Effects;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::PropertyValue;

namespace {

// One D2D effect: its CLSID, property values by index, sources, and the names under which
// composition may animate properties.
struct Effect : winrt::implements<Effect, wge::IGraphicsEffect, wge::IGraphicsEffectSource,
                                  abi_fx::IGraphicsEffectD2D1Interop> {
    GUID clsid{};
    std::vector<IInspectable> properties;
    std::vector<wge::IGraphicsEffectSource> sources;
    std::vector<std::pair<std::wstring, UINT>> animatable;
    winrt::hstring name;

    winrt::hstring Name() const { return name; }
    void Name(const winrt::hstring& value) { name = value; }

    IFACEMETHODIMP GetEffectId(GUID* id) noexcept override {
        if (!id) return E_POINTER;
        *id = clsid;
        return S_OK;
    }
    IFACEMETHODIMP GetNamedPropertyMapping(LPCWSTR propertyName, UINT* index,
                                           abi_fx::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!propertyName || !index || !mapping) return E_POINTER;
        for (const auto& [n, i] : animatable)
            if (_wcsicmp(n.c_str(), propertyName) == 0) {
                *index = i;
                *mapping = abi_fx::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
                return S_OK;
            }
        return E_INVALIDARG;
    }
    IFACEMETHODIMP GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = static_cast<UINT>(properties.size());
        return S_OK;
    }
    IFACEMETHODIMP GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index >= properties.size()) return E_INVALIDARG;
        try {
            *value = properties[index].as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }
    IFACEMETHODIMP GetSource(UINT index, abi_fx::IGraphicsEffectSource** source) noexcept override {
        if (!source) return E_POINTER;
        *source = nullptr;
        if (index >= sources.size() || !sources[index]) return E_INVALIDARG;
        try {
            *source = sources[index].as<abi_fx::IGraphicsEffectSource>().detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }
    IFACEMETHODIMP GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = static_cast<UINT>(sources.size());
        return S_OK;
    }
};

}  // namespace

wge::IGraphicsEffect MakeGaussianBlurEffect(const winrt::hstring& name, float blurAmount,
                                            const wge::IGraphicsEffectSource& source) {
    auto e = winrt::make_self<Effect>();
    e->clsid = CLSID_D2D1GaussianBlur;
    e->name = name;
    e->properties = {PropertyValue::CreateSingle(blurAmount),
                     PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED),
                     PropertyValue::CreateUInt32(D2D1_BORDER_MODE_SOFT)};
    e->sources = {source};
    e->animatable = {{L"BlurAmount", D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION}};
    return e.as<wge::IGraphicsEffect>();
}

wge::IGraphicsEffect MakeAlphaMaskEffect(const wge::IGraphicsEffectSource& source,
                                         const wge::IGraphicsEffectSource& mask) {
    auto e = winrt::make_self<Effect>();
    e->clsid = CLSID_D2D1AlphaMask;
    e->sources = {source, mask};
    return e.as<wge::IGraphicsEffect>();
}

winrt::Windows::UI::Composition::CompositionBrush MakeFrostBrush(const winrt::Windows::UI::Composition::Compositor& comp,
                                                                 float blurPx) {
    namespace wuc = winrt::Windows::UI::Composition;
    try {
        auto factory = comp.CreateEffectFactory(
            MakeGaussianBlurEffect(L"Blur", blurPx, wuc::CompositionEffectSourceParameter(L"backdrop")));
        auto brush = factory.CreateBrush();
        brush.SetSourceParameter(L"backdrop", comp.CreateBackdropBrush());
        return brush;
    } catch (...) {
        return nullptr;
    }
}

winrt::Windows::UI::Composition::CompositionBrush MakeGlassBrush(const winrt::Windows::UI::Composition::Compositor& comp,
                                                                 float blurPx, float saturation, D2D1_COLOR_F tint,
                                                                 const winrt::Windows::UI::Composition::CompositionBrush& mask) {
    namespace wuc = winrt::Windows::UI::Composition;
    try {
        const auto blur = MakeGaussianBlurEffect(L"Blur", blurPx, wuc::CompositionEffectSourceParameter(L"backdrop"));
        auto saturate = winrt::make_self<Effect>();
        saturate->clsid = CLSID_D2D1Saturation;
        saturate->properties = {PropertyValue::CreateSingle(saturation)};
        saturate->sources = {blur.as<wge::IGraphicsEffectSource>()};
        auto flood = winrt::make_self<Effect>();
        flood->clsid = CLSID_D2D1Flood;
        flood->properties = {PropertyValue::CreateSingleArray({tint.r, tint.g, tint.b, 1.0f})};
        // (1 - a) x the saturated backdrop + a x the tint, opaque.
        auto mix = winrt::make_self<Effect>();
        mix->clsid = CLSID_D2D1ArithmeticComposite;
        mix->properties = {PropertyValue::CreateSingleArray({0.0f, 1.0f - tint.a, tint.a, 0.0f}),
                           PropertyValue::CreateBoolean(true)};
        mix->sources = {saturate.as<wge::IGraphicsEffectSource>(), flood.as<wge::IGraphicsEffectSource>()};
        wge::IGraphicsEffect graph = mix.as<wge::IGraphicsEffect>();
        if (mask)
            graph = MakeAlphaMaskEffect(graph.as<wge::IGraphicsEffectSource>(),
                                        wuc::CompositionEffectSourceParameter(L"mask"));
        auto factory = comp.CreateEffectFactory(graph);
        auto brush = factory.CreateBrush();
        brush.SetSourceParameter(L"backdrop", comp.CreateBackdropBrush());
        if (mask) brush.SetSourceParameter(L"mask", mask);
        return brush;
    } catch (...) {
        return MakeFrostBrush(comp, blurPx);
    }
}

}  // namespace glasswave::gfx
