#pragma once

#include <render/ShaderRegistry.hpp>

// a unit quad placed through the uniforms, carrying its own 0..1 uv so the
// fragment half can sample whatever texture the draw points at
struct BlitVertex   { valo::Vec2f pos; };
struct BlitUniforms { valo::Mat4f proj; valo::Vec2f pos, size; valo::TextureID tex; };
struct BlitVaryings { valo::Vec2f uv; };

inline void blitVS(const BlitVertex& in, const BlitUniforms& u, BlitVaryings& out) {
    const valo::Vec2f p = u.pos + in.pos * u.size;
    out.uv = in.pos;
    valo::valo_Position = u.proj * valo::Vec4f(p.x, p.y, 0.f, 1.f);
}

VALO_VERTEX_SHADER(blitVS)
