#pragma once

#include <render/ShaderRegistry.hpp>

#include <cmath>
#include <algorithm>

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Color;

// one fullscreen triangle; all the work is in the fragment shader
struct Vertex   { Vec2f pos; };                                       // NDC
struct Uniforms { Vec3f ro, cu, cv, cw; float aspect; };              // camera origin + basis
struct Varyings { Vec2f ndc; };

inline void sdfVS(const Vertex& in, const Uniforms&, Varyings& out) {
    out.ndc = in.pos;
    valo::valo_Position = {in.pos.x, in.pos.y, 0.5f, 1.f};
}

VALO_VERTEX_SHADER(sdfVS)
