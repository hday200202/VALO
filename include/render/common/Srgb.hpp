#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

#include "Math.hpp"

namespace valo {

/* =========================================================================
    sRGB

    Textures and targets flagged sRGB store encoded bytes; shaders always work
    in linear. Decoding on sample rather than on load is what a Vulkan _SRGB
    format does, and it matters: 8 bits of *linear* would spend most of its
    codes above mid-grey and band visibly in the darks.

    Alpha is never sRGB - it is a coverage value, not a colour.
// ========================================================================= */
namespace detail {
    // 256 -> 256, exact
    inline const std::array<float, 256> kSrgbToLinear = []{
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.f;
            t[size_t(i)] = (c <= 0.04045f) ? (c / 12.92f)
                                           : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();

    /*
        The encode direction samples a curve that is steep near zero on a
        uniformly spaced linear axis, so it is not exact: at 12 bits the error
        is at most one code in the darkest handful of values and zero
        everywhere else. The +1 entry exists so linear == 1.0f, which indexes
        exactly 1 << kEncodeBits, stays in bounds.
    */
    inline constexpr int kEncodeBits = 12;
    inline constexpr size_t kEncodeSize = (size_t(1) << kEncodeBits) + 1;

    inline const std::array<uint8_t, kEncodeSize> kLinearToSrgb = []{
        std::array<uint8_t, kEncodeSize> t{};
        for (size_t i = 0; i < kEncodeSize; ++i) {
            const float c = float(i) / float(size_t(1) << kEncodeBits);
            const float e = (c <= 0.0031308f) ? (c * 12.92f)
                                              : (1.055f * std::pow(c, 1.f / 2.4f) - 0.055f);
            t[i] = uint8_t(std::lround(std::clamp(e, 0.f, 1.f) * 255.f));
        }
        return t;
    }();
}

inline float srgbToLinear(const uint8_t encoded) { return detail::kSrgbToLinear[encoded]; }

inline uint8_t linearToSrgb(const float linear) {
    const float c = std::clamp(linear, 0.f, 1.f);
    return detail::kLinearToSrgb[size_t(c * float(size_t(1) << detail::kEncodeBits) + 0.5f)];
}

// packed ABGR32 <-> linear Color, for targets whose bytes are sRGB-encoded
inline Color abgr32ToLinear(const uint32_t c) {
    return {
        srgbToLinear(uint8_t(c)),
        srgbToLinear(uint8_t(c >> 8)),
        srgbToLinear(uint8_t(c >> 16)),
        float(uint8_t(c >> 24)) / 255.f,
    };
}

inline uint32_t linearToABGR32Srgb(const Color& c) {
    const uint32_t r = linearToSrgb(c.x);
    const uint32_t g = linearToSrgb(c.y);
    const uint32_t b = linearToSrgb(c.z);
    const uint32_t a = uint32_t(std::clamp(c.w, 0.f, 1.f) * 255.f + 0.5f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

} // namespace valo
