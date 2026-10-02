#pragma once

#include "scobject.vert.hpp"

namespace sc {

struct OrbVertex   { Vec3f pos; Vec3f normal; };
struct OrbVaryings { Vec3f normal; Vec3f worldPos; };
struct OrbUniforms { Mat4f viewProj; Vec3f centre, colour; float brightness, radius; };

inline void orbVS(const OrbVertex& in, const OrbUniforms& u, OrbVaryings& out) {
    out.normal   = in.normal;
    out.worldPos = in.pos * u.radius + u.centre;
    valo_Position = u.viewProj * Vec4f{out.worldPos, 1.f};
}

VALO_VERTEX_SHADER(orbVS)

} // namespace sc
