#pragma once

#include <render/ShaderRegistry.hpp>

namespace sc {

using namespace valo;

/*
    UVs and tangents are here because the raster path gets its smoothness from
    a normal map, not from vertex normals - a traced hit with only the
    interpolated normal reads as faceted however well the normals are welded.
*/
struct TraceTri {
    Vec3f p0;  uint32_t material;
    Vec3f p1;  float    pad1;
    Vec3f p2;  float    pad2;
    Vec3f n0;  float    pad3;
    Vec3f n1;  float    pad4;
    Vec3f n2;  float    pad5;
    Vec4f t0, t1, t2;               // tangent xyz, handedness w
    Vec2f uv0, uv1, uv2;
    Vec2f pad6;
};

// What traversal actually reads per triangle test: 40 bytes instead of the
// 176-byte TraceTri, which only the one hit that gets shaded needs. The edges
// are precomputed because every test would otherwise subtract them again.
// opacity is the material's alpha, here so a shadow ray can see through glass
// without a material lookup per test.
struct TriPos {
    Vec3f p0;
    Vec3f e1;
    Vec3f e2;
    float opacity;
};

struct BVHNode {
    Vec3f    lo;  uint32_t leftFirst;
    Vec3f    hi;  uint32_t count;
};

struct TraceMaterial {
    Vec4f baseColor;
    Vec3f emissive;   float metallic;
    float roughness;  float pad0, pad1, pad2;

    uint32_t baseColorTex, metallicRoughnessTex, normalTex;
    uint32_t occlusionTex, emissiveTex, pad3, pad4, pad5;
};

struct TraceLight {
    Vec3f pos;     float pad0;
    Vec3f colour;  float intensity;
};

struct TraceVertex   { Vec2f ndc; };
struct TraceVaryings { Vec2f ndc; };

/*
    The camera basis is passed rather than a matrix: the shader builds a ray per
    pixel instead of transforming a vertex, so an inverse projection would only
    be undone again.
*/
struct TraceUniforms {
    Vec3f camPos;    float tanHalf;
    Vec3f camRight;  float aspect;
    Vec3f camUp;     float exposure;
    Vec3f camFwd;    float floorY;

    TraceLight lights[4];

    uint32_t lightCount;
    uint32_t showFloor;
    uint32_t reflections;
    uint32_t pad0;
};

inline void traceVS(const TraceVertex& in, const TraceUniforms&, TraceVaryings& out) {
    out.ndc = in.ndc;
    valo_Position = Vec4f(in.ndc.x, in.ndc.y, 0.5f, 1.f);
}

VALO_VERTEX_SHADER(traceVS)

} // namespace sc
