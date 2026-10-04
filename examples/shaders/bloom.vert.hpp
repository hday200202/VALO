#pragma once

#include <render/ShaderRegistry.hpp>

/*
    Shared vertex half for the three bloom passes. Each pass is the same unit
    quad placed through the uniforms, so the uniform block is a superset and
    each fragment half uses the fields it needs - one block means one vertex
    shader rather than three near-identical ones.

    No `bool` in here on purpose: a C++ bool is 1 byte and a GLSL block bool is
    32-bit, so the two layouts drift. Flags are floats.
*/
struct BloomVertex { valo::Vec2f pos; };            // 0..1

struct BloomUniforms {
    valo::Mat4f     proj;
    valo::Vec2f     pos, size;                      // where the quad lands, in pixels
    valo::Vec2f     texelStep;                      // blur direction / texture size
    valo::TextureID srcTex;
    valo::TextureID bloomTex;                       // composite only
    float           threshold;
    float           intensity;
};

struct BloomVaryings { valo::Vec2f uv; };

inline void bloomVS(const BloomVertex& in, const BloomUniforms& u, BloomVaryings& out) {
    const valo::Vec2f p = u.pos + in.pos * u.size;
    out.uv = in.pos;
    valo::valo_Position = u.proj * valo::Vec4f(p.x, p.y, 0.f, 1.f);
}

VALO_VERTEX_SHADER(bloomVS)
