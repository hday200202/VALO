#version 450

#include <valo.glsl>

// mirrors me::Material; see minimal.vert
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

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    uint tex = pc.material.baseColorTex;

    vec4 color = tex == VALO_EMPTY_TEXTURE
        ? pc.material.baseColor
        : valoTexture(tex, vUV, VALO_REPEAT_LINEAR);

    if (color.a < 0.1) discard;
    color.a = 1;

    outColor = color;
}
