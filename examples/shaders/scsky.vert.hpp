#pragma once

#include "scobject.vert.hpp"

namespace sc {

// ---------------------------------------------------------------------------
// sky - one fullscreen quad, a camera ray per pixel
// ---------------------------------------------------------------------------
struct SkyVertex   { Vec2f ndc; };
struct SkyVaryings { Vec2f ndc; };
struct SkyUniforms {
    Vec3f right, up, forward;
    float tanHalfFov, aspect;
};

inline void skyVS(const SkyVertex& in, const SkyUniforms&, SkyVaryings& out) {
    out.ndc = in.ndc;
    valo_Position = {in.ndc.x, in.ndc.y, 0.99999f, 1.f};    // pinned to the far plane
}

VALO_VERTEX_SHADER(skyVS)

} // namespace sc
