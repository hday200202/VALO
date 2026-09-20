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

inline Color texture(const TextureID tex, const Vec2f& uv) {
    assert(detail::boundTargets && "texture() called outside of a draw");
    const RenderTarget& t = *(*detail::boundTargets)[tex];
    float u = std::clamp<float>(uv.x, 0, 1) * (t.width() - 1);
    float v = std::clamp<float>(uv.y, 0, 1) * (t.height() - 1);

    uint32_t x = static_cast<uint32_t>(u);
    uint32_t y = static_cast<uint32_t>(v);

    return t.getPixel(x, y);
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

}