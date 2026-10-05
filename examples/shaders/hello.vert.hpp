#pragma once

#include <render/ShaderRegistry.hpp>

#include <cmath>

using namespace valo;

// what each vertex in the vertex buffer holds
struct HelloVertex {
    Vec2f pos;
    Vec3f colour;
};

// set once per draw from the CPU
struct HelloUniforms {
    float angle;
    float aspect;
};

// handed from the vertex shader to the fragment shader, interpolated
struct HelloVaryings {
    Vec3f colour;
};

inline void helloVS(const HelloVertex& in, const HelloUniforms& u, HelloVaryings& out) {
    const float c = std::cos(u.angle);
    const float s = std::sin(u.angle);

    const Vec2f p = Vec2f(in.pos.x * c - in.pos.y * s,
                          in.pos.x * s + in.pos.y * c);

    out.colour    = in.colour;
    valo_Position = Vec4f(p.x / u.aspect, p.y, 0.5f, 1.f);
}

VALO_VERTEX_SHADER(helloVS)
