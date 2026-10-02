#pragma once

#include "gltf.vert.hpp"

/* ===========================================================================
    Sky

    A fullscreen quad pinned to the far plane. The fragment shader turns each
    NDC position back into a camera ray and asks the environment what colour it
    sees. Nothing is transformed, so the sky costs one quad.
=========================================================================== */
struct SkyVertex   { Vec2f ndc; };
struct SkyUniforms { float aspect, tanHalf; };
struct SkyVaryings { Vec2f ndc; };

inline void gltfSkyVS(const SkyVertex& in, const SkyUniforms&, SkyVaryings& out) {
    out.ndc = in.ndc;
    valo::valo_Position = { in.ndc.x, in.ndc.y, 0.9999f, 1.f };   // just inside the far plane
}

VALO_VERTEX_SHADER(gltfSkyVS)
