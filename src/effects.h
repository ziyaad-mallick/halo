// Minimal Win2D-free effect graph nodes for CompositionEffectBrush.
// Composition accepts any object implementing IGraphicsEffect +
// IGraphicsEffectD2D1Interop; we only need Gaussian blur.
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <d2d1effects.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <windows.graphics.effects.h>

enum GRAPHICS_EFFECT_PROPERTY_MAPPING {
    GRAPHICS_EFFECT_PROPERTY_MAPPING_UNKNOWN,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORX,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORY,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORZ,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_VECTORW,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_RECT_TO_VECTOR4,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_RADIANS_TO_DEGREES,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_COLORMATRIX_ALPHA_MODE,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_COLOR_TO_VECTOR3,
    GRAPHICS_EFFECT_PROPERTY_MAPPING_COLOR_TO_VECTOR4,
};

// Not shipped in the MinGW SDK headers; layout follows the public Windows SDK
// windows.graphics.effects.interop.h.
struct IGraphicsEffectD2D1Interop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(
        LPCWSTR name, UINT* index, GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(
        UINT index, ABI::Windows::Foundation::IPropertyValue** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSource(
        UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) = 0;
};

__CRT_UUID_DECL(IGraphicsEffectD2D1Interop, 0x2FC57384, 0xA068, 0x44D7, 0xA3, 0x31, 0x30, 0x98, 0x2F, 0xCF, 0x71, 0x77)

namespace halo {

namespace wge = winrt::Windows::Graphics::Effects;

// D2D1 Gaussian blur: props are [StandardDeviation, Optimization, BorderMode].
struct GaussianBlurEffect : winrt::implements<GaussianBlurEffect, wge::IGraphicsEffect,
                                              wge::IGraphicsEffectSource, IGraphicsEffectD2D1Interop> {
    GaussianBlurEffect(wge::IGraphicsEffectSource source, float stddev)
        : m_source(std::move(source)), m_stddev(stddev) {}

    winrt::hstring Name() { return m_name; }
    void Name(winrt::hstring const& n) { m_name = n; }

    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) override {
        *id = CLSID_D2D1GaussianBlur;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*, GRAPHICS_EFFECT_PROPERTY_MAPPING*) override {
        return E_INVALIDARG;  // no animatable named properties
    }
    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) override {
        *count = 3;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) override try {
        using winrt::Windows::Foundation::PropertyValue;
        winrt::Windows::Foundation::IInspectable v;
        switch (index) {
            case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION: v = PropertyValue::CreateSingle(m_stddev); break;
            case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION:
                v = PropertyValue::CreateUInt32(1 /* D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED */); break;
            case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE:
                v = PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD); break;  // no transparent fringe
            default: return E_BOUNDS;
        }
        *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(
            winrt::detach_abi(v.as<winrt::Windows::Foundation::IPropertyValue>()));
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
    HRESULT STDMETHODCALLTYPE GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** s) override {
        if (index != 0) return E_BOUNDS;
        *s = reinterpret_cast<ABI::Windows::Graphics::Effects::IGraphicsEffectSource*>(
            winrt::detach_abi(wge::IGraphicsEffectSource(m_source)));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) override {
        *count = 1;
        return S_OK;
    }

private:
    wge::IGraphicsEffectSource m_source;
    float m_stddev;
    winrt::hstring m_name;
};

}  // namespace halo
