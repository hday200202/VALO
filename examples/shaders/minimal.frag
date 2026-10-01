#version 450

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
    // the texture branch of me::fs has no Vulkan half yet, so baseColor always
    // wins here - the C++ version falls back to it when baseColorTex is empty
    outColor = vec4(pc.bcR, pc.bcG, pc.bcB, pc.bcA);
}
