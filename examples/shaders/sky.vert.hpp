#pragma once

#include "pbr.vert.hpp"

struct SkyVertex   { Vec2f ndc; };
struct SkyUniforms { float aspect, tanHalf; };
struct SkyVaryings { Vec2f ndc; };

inline void skyVS(const SkyVertex& in, const SkyUniforms&, SkyVaryings& out) {
    out.ndc = in.ndc;
    valo::valo_Position = {in.ndc.x, in.ndc.y, 0.9999f, 1.f};   // just inside the far plane
}

VALO_VERTEX_SHADER(skyVS)
