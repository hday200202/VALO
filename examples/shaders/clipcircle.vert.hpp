#pragma once

#include <render/ShaderRegistry.hpp>

// clip pass: fullscreen quad, discards outside a circle. colorWrite is off, so
// all it leaves behind is stencil.
struct ClipVertex   { valo::Vec2f ndc; };
struct ClipUniforms { float aspect, radius; };
struct ClipVaryings { valo::Vec2f ndc; };

inline void clipVS(const ClipVertex& in, const ClipUniforms&, ClipVaryings& out) {
    out.ndc = in.ndc;
    valo::valo_Position = {in.ndc.x, in.ndc.y, 0.5f, 1.f};
}

VALO_VERTEX_SHADER(clipVS)
