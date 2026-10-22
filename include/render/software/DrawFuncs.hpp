#pragma once

#include <type_traits>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "../RenderCommand.hpp"
#include "RenderTarget.hpp"
#include "../buffers/VertexBuffer.hpp"
#include "../buffers/IndexBuffer.hpp"
#include "../ShaderProgram.hpp"
#include "ThreadPool.hpp"
#include "Sampling.hpp"

namespace valo {

struct ClipVert { Vec4f pos; float vary[MAX_VARYINGS]; };

struct ScreenTri {
    Vec4f s[3];
    float area;
    float sign;
    int xmin, ymin, xmax, ymax;
    float vary[3][MAX_VARYINGS];
    float invArea;
    bool frontFacing;
    bool topLeft[3];
    uint32_t state = 0;
    float dBdx[3], dBdy[3];
    float dDenDx, dDenDy;
};

inline bool depthPasses(const DepthCompare cmp, const float z, const float stored) {
    switch (cmp) {
        case DepthCompare::Less:         return z <  stored;
        case DepthCompare::LessEqual:    return z <= stored;
        case DepthCompare::Greater:      return z >  stored;
        case DepthCompare::GreaterEqual: return z >= stored;
        case DepthCompare::Equal:        return z == stored;
        case DepthCompare::NotEqual:     return z != stored;
        case DepthCompare::Always:       return true;
        case DepthCompare::Never:        return false;
    }
    return false;
}


bool stencilTest(
    const RenderTarget* target, 
    const RenderCommand& cmd, 
    int x, int y
);


void stencilApply(
    RenderTarget* target, 
    const RenderCommand& cmd, 
    int x, int y
);


struct PixelRect { int x0, y0, x1, y1; };

Viewport resolveViewport(const RenderTarget* target, const RenderCommand& cmd);
PixelRect resolveClipRect(const RenderTarget* target, const RenderCommand& cmd);


bool setupTriangle(
    const RenderTarget* target,
    const RenderCommand& cmd,
    const ClipVert& c0, const ClipVert& c1, const ClipVert& c2,
    const uint32_t varyingCount,
    ScreenTri& out
);

int clipNear(
    const ClipVert in[3], 
    ClipVert out[4], 
    uint32_t varyingCount
);

struct DrawContext {
    const std::vector<std::unique_ptr<RenderTarget>>* targets = nullptr;
    const void* const* uniforms = nullptr;
    const void* const* storage = nullptr;
    const uint32_t* storageBytes = nullptr;
};

struct DrawState {
    RenderTarget*   target = nullptr;
    Program         program = {};
    RenderCommand   cmd = {};
    const void*     uniforms[MAX_UNIFORM_SLOTS] = {};

    // the workers get their own thread_local copies, so the binding has to
    // travel with the state rather than being set once on the calling thread
    const void*     storage[MAX_STORAGE_SLOTS]      = {};
    uint32_t        storageBytes[MAX_STORAGE_SLOTS] = {};
};


// wireframe keeps raw clip verts rather than a ScreenTri, so it batches in its
// own list; a batch never holds both kinds at once
struct WireTri {
    ClipVert v[3];
    uint32_t state = 0;
};

struct TriBatch {
    std::vector<ScreenTri> tris;
    std::vector<WireTri>   wires;
    std::vector<DrawState> states;
    RenderTarget* target = nullptr;

    bool empty() const { return tris.empty() && wires.empty(); }
};

void flushTriBatch(TriBatch& batch, ThreadPool& pool, const DrawContext& ctx);

template <typename F>
void forEachBand(
    ThreadPool& pool,
    const uint32_t height,
    const DrawContext& ctx,
    F&& fn
) {
    const uint32_t bands = std::max(1u, std::min(pool.size() * 8, height));
    const uint32_t rows  = (height + bands - 1) / bands;

    // the calling thread takes bands too, and a batched band rebinds per triangle;
    // put its bindings back so the rest of the current draw reads its own buffers
    const auto targets      = detail::boundTargets;
    const auto uniforms     = detail::boundUniforms;
    const auto storage      = detail::boundStorage;
    const auto storageBytes = detail::boundStorageBytes;

    pool.dispatch(bands, [&](uint32_t b) {
        detail::boundTargets      = ctx.targets;
        detail::boundUniforms     = ctx.uniforms;
        detail::boundStorage      = ctx.storage;
        detail::boundStorageBytes = ctx.storageBytes;
        const int y0 = int(b * rows);
        const int y1 = int(std::min((b + 1) * rows, height));
        if (y0 < y1) fn(y0, y1);
    });

    detail::boundTargets      = targets;
    detail::boundUniforms     = uniforms;
    detail::boundStorage      = storage;
    detail::boundStorageBytes = storageBytes;
}


// blends `src` into the samples of (x, y) in `mask`; a full mask is writePixel
void writeSamples(
    RenderTarget* target,
    const int x,
    const int y,
    const uint32_t mask,
    const Color& src,
    const BlendMode mode
);

void writePixel(
    RenderTarget* target, 
    const int x, 
    const int y, 
    const Color& src, 
    const BlendMode mode
);


void rasterizePoint(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip,
    const float* varys,
    const float size,
    const int bandY0, const int bandY1
);


void rasterizeLine(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip[2],
    const float* varys[2],
    const int bandY0, const int bandY1
);


void rasterizeTriangle(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const ScreenTri& tri,
    const int bandY0, const int bandY1
);


void drawTriangles(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    const DrawContext& ctx,
    TriBatch& batch
);


void drawLines(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    const DrawContext& ctx
);


void drawPoints(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    const DrawContext& ctx
);

}; // namespace valo