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

}  // namespace glasswave::gfx
