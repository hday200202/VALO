#extension GL_EXT_nonuniform_qualifier  : require

// lets a uniform/push-constant block mirror a C++ struct field for field,
// instead of hand-flattening around std140's padding rules
#extension GL_EXT_scalar_block_layout   : require

// matches valo::EMPTY_TEXTURE
const uint VALO_EMPTY_TEXTURE  = 0xFFFFFFFFu;

const uint VALO_REPEAT_NEAREST = 0u;
const uint VALO_REPEAT_LINEAR  = 1u;
const uint VALO_CLAMP_LINEAR   = 9u;

layout(set = 1, binding = 0) uniform texture2D uTextures[];
layout(set = 1, binding = 1) uniform sampler   uSamplers[];

vec4 valoTexture(uint tex, vec2 uv, uint samp) {
    return texture(sampler2D(uTextures[nonuniformEXT(tex)], uSamplers[samp]), uv);
}