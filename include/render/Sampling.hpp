#pragma once

#include <vector>
#include <memory>
#include "RenderTarget.hpp"
#include "common/Math.hpp"

namespace valo {

using TextureID = uint32_t;

namespace detail {
    inline thread_local const std::vector<std::unique_ptr<RenderTarget>>* boundTargets = nullptr;
}

inline thread_local float valo_PointSize = 1.f;
inline thread_local uint32_t valo_InstanceIndex = 0.f;
inline thread_local bool valo_Discard = false;
inline void vDiscard() { valo_Discard = true; }

enum class Wrap     { Repeat, Clamp, Mirror };
enum class Filter   { Nearest, Linear };

struct Sampler {
    Sampler(
        const Wrap wrapU = Wrap::Repeat, 
        const Wrap wrapV = Wrap::Repeat, 
        const Filter filter = Filter::Linear
    ) : wrapU(wrapU), wrapV(wrapV), filter(filter) {}

    Wrap    wrapU   = Wrap::Repeat;
    Wrap    wrapV   = Wrap::Repeat;
    Filter  filter  = Filter::Linear;
};


inline constexpr float wrap(float t, Wrap mode) {
    switch (mode) {
        case Wrap::Clamp:   return std::clamp(t, 0.f, 1.f);
        case Wrap::Repeat:  return t - std::floor(t);
        case Wrap::Mirror:  {
            const float tile    = std::floor(t);
            const float f       = t - tile;
            const bool  odd     = (int(tile) & 1) != 0;
            return odd ? 1.f - f : f;
        }
    }
    return 0.f;
}


inline constexpr int wrapPixel(int x, int size, Wrap mode) {
    switch (mode) {
        case Wrap::Clamp: return std::clamp(x, 0, size - 1);
        case Wrap::Repeat: return ((x % size) + size) % size;
        case Wrap::Mirror: {
            const int period = 2 * size;
            const int m = ((x % period) + period) % period;
            return m < size ? m : period - 1 - m;
        }
    }
    return 0;
}

inline uint32_t lerpABGR32(const uint32_t a, const uint32_t b, const uint32_t t) {
    const uint32_t s = 256 - t;
    const uint32_t r = (( a        & 0xFF) * s + ( b        & 0xFF) * t) >> 8;
    const uint32_t g = (((a >>  8) & 0xFF) * s + ((b >>  8) & 0xFF) * t) >> 8;
    const uint32_t bl= (((a >> 16) & 0xFF) * s + ((b >> 16) & 0xFF) * t) >> 8;
    const uint32_t al= (((a >> 24) & 0xFF) * s + ((b >> 24) & 0xFF) * t) >> 8;
    return (al << 24) | (bl << 16) | (g << 8) | r;
}

inline Color texture(const TextureID tex, const Vec2f& uv, const Sampler& sampler = {}) {
    assert(detail::boundTargets && "texture() called outside of a draw");
    const RenderTarget& t = *(*detail::boundTargets)[tex];

    float u = wrap(uv.x, sampler.wrapU) * t.width();
    float v = wrap(uv.y, sampler.wrapV) * t.height();

    if (sampler.filter == Filter::Nearest) {
        const uint32_t x = std::min(uint32_t(u), t.width()  - 1);
        const uint32_t y = std::min(uint32_t(v), t.height() - 1);
        return t.getPixel(x, y);
    }

    const float fx = u - 0.5f, fy = v - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;

    const int w = int(t.width()), h = int(t.height());
    const int x0 = wrapPixel(int(x0f),      w, sampler.wrapU);
    const int x1 = wrapPixel(int(x0f) + 1,  w, sampler.wrapU);
    const int y0 = wrapPixel(int(y0f),      h, sampler.wrapV);
    const int y1 = wrapPixel(int(y0f) + 1,  h, sampler.wrapV);

    const uint32_t* px = t.data();
    const uint32_t  W  = t.width();
    const uint32_t p00 = px[y0 * W + x0], p10 = px[y0 * W + x1];
    const uint32_t p01 = px[y1 * W + x0], p11 = px[y1 * W + x1];

    const uint32_t wx = uint32_t(tx * 256.f + 0.5f);
    const uint32_t wy = uint32_t(ty * 256.f + 0.5f);

    const uint32_t top    = lerpABGR32(p00, p10, wx);
    const uint32_t bottom = lerpABGR32(p01, p11, wx);
    return ABGR32ToColor(lerpABGR32(top, bottom, wy));
}

inline float textureDepth(const TextureID tex, const Vec2f& uv) {
    assert(detail::boundTargets && "textureDepth() called outside of a draw");
    const RenderTarget& t = *(*detail::boundTargets)[tex];
    float u = std::clamp<float>(uv.x, 0, 1) * (t.width() - 1);
    float v = std::clamp<float>(uv.y, 0, 1) * (t.height() - 1);

    uint32_t x = static_cast<uint32_t>(u);
    uint32_t y = static_cast<uint32_t>(v);

    return t.getDepth(x, y);
}

#define discard valo::vDiscard()

}