#version 450

#include <valo.glsl>

/*
    The GLSL half of MinimalEngine's shader pair. The C++ half is me::vs in
    engines/MinimalEngine.hpp; both implement the same spec.

    scalar layout, so these blocks mirror me::Uniforms and me::Material field
    for field rather than being hand-flattened around std140's padding. Keep
    the two in step - the field order here is the field order there.
*/
struct Material {
    uint  baseColorTex;
    uint  emissiveTex;
    uint  metallicRoughnessTex;
    uint  normalTex;
    uint  occlusionTex;

    vec4  baseColor;
    vec3  emissive;
    float metallic;
};

layout(scalar, push_constant) uniform Push {
    mat4     mvp;
    Material material;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 vUV;

void main() {
    vUV = inUV;
    gl_Position = pc.mvp * vec4(inPos, 1.0);
}
