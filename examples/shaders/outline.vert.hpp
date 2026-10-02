#pragma once

#include "helmet.vert.hpp"

// outline pass: same mesh, pushed out along the normal in object space, flat black
inline void outlineVS(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.normal = in.normal; out.uv = in.uv;
    valo::valo_Position = u.mvp * valo::Vec4f(in.pos + in.smoothNormal * u.outline, 1.f);
}

VALO_VERTEX_SHADER(outlineVS)
