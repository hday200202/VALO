#pragma once

#include <render/ShaderRegistry.hpp>
#include <utils/GLTFParser.hpp>

namespace me {

using namespace valo;

struct Material {
    TextureID baseColorTex          = EMPTY_TEXTURE;
    TextureID emissiveTex           = EMPTY_TEXTURE;
    TextureID metallicRoughnessTex  = EMPTY_TEXTURE;
    TextureID normalTex             = EMPTY_TEXTURE;
    TextureID occlusionTex          = EMPTY_TEXTURE;

    Vec4f     baseColor = {1.f, 1.f, 1.f, 1.f};
    Vec3f     emissive  = {0.f, 0.f, 0.f};
    float     metallic  = 1.f;
};

/*
    Uniforms are memcpy'd into a fixed-size buffer inside RenderCommand, so this
    has to stay a POD - no vectors, no pointers to anything that can move. The
    draw loop walks the nodes and primitives and hands the shader one already
    resolved matrix and one material.
*/
struct Uniforms {
    Mat4f    mvp;
    Material material;
};

struct Varyings { Vec2f uv; };

inline void minimalVS(const GLTFVertex& in, const Uniforms& u, Varyings& out) {
    out.uv = in.uv;
    valo_Position = u.mvp * Vec4f{ in.pos, 1.f };
}

VALO_VERTEX_SHADER(minimalVS)

} // namespace me
