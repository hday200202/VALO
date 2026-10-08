#pragma once

#include <render/ShaderRegistry.hpp>
#include <utils/GLTFParser.hpp>

namespace me {

using namespace valo;

/*
    ModelMaterial is what loadModel() hands back for each glTF material, with
    its textures already loaded - it is laid out to drop straight into Uniforms.
    Uniforms are memcpy'd into the command, so this stays plain data.
*/
struct Uniforms {
    Mat4f         mvp;
    ModelMaterial material;
};

struct Varyings { Vec2f uv; };

inline void minimalVS(const GLTFVertex& in, const Uniforms& u, Varyings& out) {
    out.uv = in.uv;
    valo_Position = u.mvp * Vec4f{ in.pos, 1.f };
}

VALO_VERTEX_SHADER(minimalVS)

} // namespace me
