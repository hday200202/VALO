#pragma once

#include <render/ShaderRegistry.hpp>

// N x N x N cubes from ONE submit: the vertex shader places each instance
// on the grid from valo_InstanceIndex.
constexpr uint32_t GRID    = 25;          // cubes per axis -> GRID^3 total
constexpr float    SPACING = 8.f;         // distance between cube centers
constexpr float    CUBE    = 1.f;         // cube half-size (mesh is -1..1)

struct Vertex   { valo::Vec3f pos; valo::Vec2f uv; };
struct Uniforms { valo::Mat4f viewProj; valo::TextureID tex; };
struct Varyings { valo::Vec2f uv; };

inline void cubesVS(const Vertex& in, const Uniforms& u, Varyings& out) {
    const uint32_t i  = valo::valo_InstanceIndex;
    const float    gx = float(i % GRID);
    const float    gy = float((i / GRID) % GRID);
    const float    gz = float(i / (GRID * GRID));
    const float    half = (GRID - 1) * 0.5f;                       // center the grid on the origin

    const valo::Vec3f offset = valo::Vec3f{gx - half, gy - half, gz - half} * SPACING;
    out.uv = in.uv;
    valo::valo_Position = u.viewProj * valo::Vec4f(in.pos * CUBE + offset, 1.f);
}

VALO_VERTEX_SHADER(cubesVS)
