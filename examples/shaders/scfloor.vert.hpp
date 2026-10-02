#pragma once

#include "scobject.vert.hpp"

namespace sc {

struct FloorVertex   { Vec3f pos; };
struct FloorVaryings { Vec3f worldPos; };
struct FloorUniforms {
    Mat4f viewProj;
    Vec3f shadowCentre;
    Vec2f shadowRadius;     // xz half extents of the model footprint
    float fadeStart, fadeEnd, gridSpacing;
};
static_assert(sizeof(FloorUniforms) <= RenderCommand::MAX_UNIFORM_BYTES,
              "FloorUniforms must fit in RenderCommand's fixed uniform block");

inline void floorVS(const FloorVertex& in, const FloorUniforms& u, FloorVaryings& out) {
    out.worldPos = in.pos;
    valo_Position = u.viewProj * Vec4f{in.pos, 1.f};
}

VALO_VERTEX_SHADER(floorVS)

} // namespace sc
