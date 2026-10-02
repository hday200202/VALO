#version 450

#include <valo.glsl>

layout(push_constant) uniform Push {
    mat4  mvp;
    uint  baseColorTex;
    uint  emissiveTex;
    uint  metallicRoughnessTex;
    uint  normalTex;
    uint  occlusionTex;
    float bcR, bcG, bcB, bcA;
    float emR, emG, emB;
    float metallic;
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    uint tex = pc.baseColorTex;

    outColor = tex == 0xFFFFFFFFu
        ? vec4(pc.bcR, pc.bcG, pc.bcB, pc.bcA)
        : valoTexture(tex, vUV, VALO_REPEAT_LINEAR) * vec4(pc.bcR, pc.bcG, pc.bcB, pc.bcA);
}
