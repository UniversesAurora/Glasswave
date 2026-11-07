#include "pch.h"
#include "gfx/color_matrix_effect.h"

namespace glasswave::gfx {

namespace wge = winrt::Windows::Graphics::Effects;
namespace abi_fx = ABI::Windows::Graphics::Effects;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::PropertyValue;

namespace {

struct ColorMatrixEffect
    : winrt::implements<ColorMatrixEffect, wge::IGraphicsEffect, wge::IGraphicsEffectSource,
                        abi_fx::IGraphicsEffectD2D1Interop> {
    ColorMatrixEffect(const std::array<float, 20>& matrix, wge::IGraphicsEffectSource source)
        : m_matrix(matrix), m_source(std::move(source)) {}

    // IGraphicsEffect
    winrt::hstring Name() const { return m_name; }
    void Name(const winrt::hstring& name) { m_name = name; }

    // IGraphicsEffectD2D1Interop
    IFACEMETHODIMP GetEffectId(GUID* id) noexcept override {
        if (!id) return E_POINTER;
        *id = CLSID_D2D1ColorMatrix;
        return S_OK;
    }

    IFACEMETHODIMP GetNamedPropertyMapping(LPCWSTR, UINT*, abi_fx::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override {
        return E_INVALIDARG;  // no animatable properties
    }

    IFACEMETHODIMP GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 3;
        return S_OK;
    }

    IFACEMETHODIMP GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        try {
            IInspectable v;
            switch (index) {
            case D2D1_COLORMATRIX_PROP_COLOR_MATRIX:
                v = PropertyValue::CreateSingleArray(winrt::array_view<const float>(m_matrix));
                break;
            case D2D1_COLORMATRIX_PROP_ALPHA_MODE:
                v = PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED);
                break;
            case D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT:
                v = PropertyValue::CreateBoolean(true);
                break;
            default:
                return E_INVALIDARG;
            }
            *value = v.as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    IFACEMETHODIMP GetSource(UINT index, abi_fx::IGraphicsEffectSource** source) noexcept override {
        if (!source) return E_POINTER;
        *source = nullptr;
        if (index != 0 || !m_source) return E_INVALIDARG;
        try {
            *source = m_source.as<abi_fx::IGraphicsEffectSource>().detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    IFACEMETHODIMP GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }

private:
    winrt::hstring m_name;
    std::array<float, 20> m_matrix;
    wge::IGraphicsEffectSource m_source;
};

}  // namespace

wge::IGraphicsEffect MakeColorMatrixEffect(const std::array<float, 20>& matrix,
                                           const wge::IGraphicsEffectSource& source) {
    return winrt::make<ColorMatrixEffect>(matrix, source);
}

}  // namespace glasswave::gfx
