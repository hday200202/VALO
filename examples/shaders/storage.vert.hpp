#pragma once

#include <render/ShaderRegistry.hpp>

struct Particle { valo::Vec4f pos; valo::Vec4f colour; };

struct StorageVertex   { valo::Vec2f ndc; };
struct StorageUniforms { uint32_t expectedCount; float scale; };
struct StorageVaryings { valo::Vec2f ndc; };

inline void storageVS(const StorageVertex& in, const StorageUniforms&, StorageVaryings& out) {
    out.ndc = in.ndc;
    valo::valo_Position = valo::Vec4f(in.ndc.x, in.ndc.y, 0.5f, 1.f);
}

VALO_VERTEX_SHADER(storageVS)
