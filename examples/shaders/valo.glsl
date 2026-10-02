#extension GL_EXT_nonuniform_qualifier : require

const uint VALO_REPEAT_NEAREST = 0u;
const uint VALO_REPEAT_LINEAR  = 1u;
const uint VALO_CLAMP_LINEAR   = 9u;

layout(set = 1, binding = 0) uniform texture2D uTextures[];
layout(set = 1, binding = 1) uniform sampler   uSamplers[];

vec4 valoTexture(uint tex, vec2 uv, uint samp) {
    return texture(sampler2D(uTextures[nonuniformEXT(tex)], uSamplers[samp]), uv);
}