#pragma once

#include <render/ShaderRegistry.hpp>

// one shared unit quad; every rect is drawn by placing it through the uniforms
struct Vertex   { valo::Vec2f pos; };                           // 0..1
struct Uniforms { valo::Mat4f proj; valo::Vec2f pos, size; valo::Vec4f color; float radius; };
struct Varyings { valo::Vec2f local; };                         // pixel offset inside the rect

inline void roundRectVS(const Vertex& in, const Uniforms& u, Varyings& out) {
    const valo::Vec2f p = u.pos + in.pos * u.size;
    out.local = in.pos * u.size;
    valo::valo_Position = u.proj * valo::Vec4f(p.x, p.y, 0.f, 1.f);
}

VALO_VERTEX_SHADER(roundRectVS)
