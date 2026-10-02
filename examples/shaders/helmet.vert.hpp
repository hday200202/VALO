#pragma once

#include <render/ShaderRegistry.hpp>

// helmet: base color texture * simple two-light lambert. The shading is
// deliberately plain so the stencil work is what you're looking at.
//
// smoothNormal: normals averaged across every vertex sharing a position. The
// helmet has split normals at its hard edges; extruding along those tears the
// outline shell open at every seam, extruding along the smoothed ones doesn't.
struct Vertex   { valo::Vec3f pos; valo::Vec3f normal; valo::Vec2f uv; valo::Vec3f smoothNormal; };
struct Uniforms { valo::Mat4f mvp, model; valo::TextureID base; float outline; };
struct Varyings { valo::Vec3f normal; valo::Vec2f uv; };

inline void helmetVS(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.normal = (u.model * valo::Vec4f(in.normal, 0.f)).xyz;
    out.uv = in.uv;
    valo::valo_Position = u.mvp * valo::Vec4f(in.pos, 1.f);
}

VALO_VERTEX_SHADER(helmetVS)
