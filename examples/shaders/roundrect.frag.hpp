#pragma once

#include "roundrect.vert.hpp"

#include <algorithm>

// rounded-rect SDF: distance from the rect shrunk by the radius, then re-expanded
inline void roundRectFS(const Varyings& in, const Uniforms& u) {
    using namespace valo;

    const Vec2f half = u.size * 0.5f;
    const Vec2f p = in.local - half;
    const Vec2f q = valo::abs(p) - Vec2f{half.x - u.radius, half.y - u.radius};
    const Vec2f qp = {std::max(q.x, 0.f), std::max(q.y, 0.f)};
    const float d = qp.length() + std::min(std::max(q.x, q.y), 0.f) - u.radius;
    if (d > 0.f) discard;
    valo_FragColor = u.color;
}

VALO_FRAGMENT_SHADER(roundRectFS)
