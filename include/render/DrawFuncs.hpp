#pragma once

#include <type_traits>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "RenderCommand.hpp"
#include "RenderTarget.hpp"
#include "VertexBuffer.hpp"
#include "IndexBuffer.hpp"
#include "ShaderProgram.hpp"
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
};

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
};

template <typename F>
void forEachBand(
    ThreadPool& pool,
    const uint32_t height,
    const DrawContext& ctx,
    F&& fn
) {
    const uint32_t bands = std::max(1u, std::min(pool.size() * 8, height));
    const uint32_t rows  = (height + bands - 1) / bands;

    pool.dispatch(bands, [&](uint32_t b) {
        detail::boundTargets = ctx.targets;
        detail::boundUniforms = ctx.uniforms;
        const int y0 = int(b * rows);
        const int y1 = int(std::min((b + 1) * rows, height));
        if (y0 < y1) fn(y0, y1);
    });
}


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
    const DrawContext& ctx
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