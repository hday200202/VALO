#pragma once

#include <render/ShaderRegistry.hpp>

namespace sea {

using namespace valo;

struct SeaVertex   { Vec2f ndc; };
struct SeaUniforms { float time; float aspect; float width; float pad0; };
struct SeaVaryings { Vec2f ndc; };

inline void seaVS(const SeaVertex& in, const SeaUniforms&, SeaVaryings& out) {
    out.ndc = in.ndc;
    valo_Position = Vec4f(in.ndc.x, in.ndc.y, 0.5f, 1.f);
}

VALO_VERTEX_SHADER(seaVS)

} // namespace sea
