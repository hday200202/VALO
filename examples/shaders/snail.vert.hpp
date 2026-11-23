#pragma once

#include <render/ShaderRegistry.hpp>

namespace snail {

using namespace valo;

struct SnailVertex   { Vec2f ndc; };
struct SnailUniforms { float time; float width; float height; int aa; TextureID noise; TextureID grain; TextureID sky; };
struct SnailVaryings { Vec2f ndc; };

inline void snailVS(const SnailVertex& in, const SnailUniforms&, SnailVaryings& out) {
    out.ndc = in.ndc;
    valo_Position = Vec4f(in.ndc.x, in.ndc.y, 0.5f, 1.f);
}

VALO_VERTEX_SHADER(snailVS)

} // namespace snail
