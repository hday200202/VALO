#pragma once

#include "helmet.vert.hpp"

#include <cmath>
#include <algorithm>

inline void helmetFS(const Varyings& in, const Uniforms& u) {
    using namespace valo;

    const Vec3f n = in.normal.normalized();
    const float key  = std::max(n.dot(Vec3f{-0.5f, 0.6f, 0.6f}.normalized()), 0.f);
    const float fill = std::max(n.dot(Vec3f{ 0.7f, 0.1f, 0.4f}.normalized()), 0.f) * 0.4f;
    const Vec3f albedo = texture(u.base, in.uv).xyz;
    const Vec3f lit = albedo * (0.18f + 0.9f * key + fill);
    valo_FragColor = {{std::pow(lit.x, 1.f / 2.2f), std::pow(lit.y, 1.f / 2.2f), std::pow(lit.z, 1.f / 2.2f)}, 1.f};
}

VALO_FRAGMENT_SHADER(helmetFS)
