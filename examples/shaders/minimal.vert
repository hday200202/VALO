#version 450

/*
    The GLSL half of MinimalEngine's shader pair. The C++ half is me::vs in
    engines/MinimalEngine.hpp; both implement the same spec.

    The block mirrors me::Uniforms byte for byte, which is why baseColor is
    four loose floats rather than a vec4: it sits at offset 84 in the C++
    struct, and std430 would round a vec4 up to 96. Scalars have 4-byte
    alignment, so spelling it out this way needs no scalar-block-layout
    feature and cannot drift.
*/
layout(push_constant) uniform Push {
    mat4  mvp;                                  // 0
    uint  baseColorTex;                         // 64
    uint  emissiveTex;                          // 68
    uint  metallicRoughnessTex;                 // 72
    uint  normalTex;                            // 76
    uint  occlusionTex;                         // 80
    float bcR, bcG, bcB, bcA;                   // 84
    float emR, emG, emB;                        // 100
    float metallic;                             // 112
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 vUV;

void main() {
    vUV = inUV;
    gl_Position = pc.mvp * vec4(inPos, 1.0);
}
