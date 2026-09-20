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

inline void writePixel(
    RenderTarget* target, 
    const int x, 
    const int y, 
    const Color& src, 
    const BlendMode mode
) {
    switch (mode) {
        case BlendMode::Opaque: {
            target->setPixel(x, y, src); 
            break;
        }

        case BlendMode::Alpha: {
            const Color dst = target->getPixel(x, y);
            const float alpha = src.w;
            Color out = src * alpha + dst * (1.f - alpha);
            out.w = 1.f;
            target->setPixel(x, y, out);
            break;
        }

        case BlendMode::Additive: {
            const Color dst = target->getPixel(x, y);
            const float alpha = src.w;
            Color out = dst + src * alpha;
            out.w = 1.f;
            target->setPixel(x, y, out);
            break;
        }

        default: break;
    }
}


inline void rasterizePoint(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip,
    const float* varys,
    const float size
) {
    Vec4f ndc = {clip.xyz / clip.w, clip.w};
    
    Vec4f screen = {
        (ndc.x + 1) * 0.5f * target->width(),
        (1 - ndc.y) * 0.5f * target->height(),
        ndc.z, ndc.w
    };

    const int w = int(target->width()), h = int(target->height());

    int px = int(screen.x), py = int(screen.y);
    if (px < 0 || py < 0 || px >= w - 1 || py >= h - 1) return;
    if (cmd.depthTest && screen.z >= target->getDepth(px, py)) return;
    else if (cmd.depthWrite) target->setDepth(px, py, screen.z);

    int n = std::max(1, int(size + 0.5f));
    int half = int(n / 2);
    Color color = program.fsThunk(program.fsFn, varys, cmd.uniforms);

    for (int y = py - half; y < py - half + n; ++y) {
        for (int x = px - half; x < px - half + n; ++x) {
            if (x < 0 || y < 0 || x >= w - 1 || y >= h - 1) continue;
            writePixel(target, x, y, color, cmd.blendmode);
        }
    }
}

inline void rasterizeLine(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip[2],
    const float* varys[2]
) {
    Vec4f ndc1 = {clip[0].xyz / clip[0].w, 1.f / clip[0].w};
    Vec4f ndc2 = {clip[1].xyz / clip[1].w, 1.f / clip[1].w};

    Vec4f s1 = {
        (ndc1.x + 1) * 0.5f * target->width(),
        (1 - ndc1.y) * 0.5f * target->height(),
        ndc1.z, ndc1.w
    };

    Vec4f s2 = {
        (ndc2.x + 1) * 0.5f * target->width(),
        (1 - ndc2.y) * 0.5f * target->height(),
        ndc2.z, ndc2.w
    };

    float dx = s2.x - s1.x;
    float dy = s2.y - s1.y;

    int steps = int(std::max(std::abs(dx), std::abs(dy)));

    const int w = int(target->width()), h = int(target->height());
    
    for (int i = 0; i <= steps; ++i) {
        float t = float(i) / std::max(steps, 1);
        float x = s1.x + dx * t;
        float y = s1.y + dy * t;
        int px = int(x), py = int(y);
        if (px < 0 || py < 0 || px >= w - 1 || h - 1) continue;

        // early z-test
        float z = s1.z + (s2.z - s1.z) * t;
        if (cmd.depthTest && z >= target->getDepth(px, py)) continue;
        if (cmd.depthWrite) target->setDepth(px, py, z);

        float invW = s1.w + (s2.w - s1.w) * t;
        const float w = 1.f / invW;

        float interp[MAX_VARYINGS];
        for (uint32_t k = 0; k < program.varyingCount; ++k) {
            interp[k] = (varys[0][k]*s1.w*(1-t) + varys[1][k]*s2.w*t) * w;
        }

        Color color = program.fsThunk(program.fsFn, interp, cmd.uniforms);
        writePixel(target, x, y, color, cmd.blendmode);
    }
}

inline void rasterizeTriangle(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip[3],
    const float* varys[3],
    const int bandY0, const int bandY1
) {
    Vec4f ndc1 = {clip[0].xyz / clip[0].w, 1.f / clip[0].w};
    Vec4f ndc2 = {clip[1].xyz / clip[1].w, 1.f / clip[1].w};
    Vec4f ndc3 = {clip[2].xyz / clip[2].w, 1.f / clip[2].w};

    Vec4f s1 = {
        (ndc1.x + 1) * 0.5f * target->width(), 
        (1 - ndc1.y) * 0.5f * target->height(),
        ndc1.z, ndc1.w
    };

    Vec4f s2 = {
        (ndc2.x + 1) * 0.5f * target->width(), 
        (1 - ndc2.y) * 0.5f * target->height(),
        ndc2.z, ndc2.w
    };

    Vec4f s3 = {
        (ndc3.x + 1) * 0.5f * target->width(), 
        (1 - ndc3.y) * 0.5f * target->height(),
        ndc3.z, ndc3.w
    };

    float area = (s2.xy-s1.xy).cross(s3.xy-s1.xy);
    bool ccw = area < 0;
    bool front = (cmd.winding == VtxWinding::CCW) ? ccw : !ccw;

    if (cmd.cullmode == CullMode::Back && !front) return;
    if (cmd.cullmode == CullMode::Front && front) return;

    float sign = area < 0 ? -1.f : 1.f;
    area *= sign;

    // create bounding box for looping
    auto minScr = min(min(s1.xy, s2.xy), s3.xy);
    auto maxScr = max(max(s1.xy, s2.xy), s3.xy);

    uint32_t xmin = std::floor(std::clamp<float>(minScr.x, 0, target->width() - 1));
    uint32_t ymin = std::floor(std::clamp<float>(minScr.y, 0, target->height() - 1));
    uint32_t xmax = std::ceil(std::clamp<float>(maxScr.x, 0, target->width() - 1));
    uint32_t ymax = std::ceil(std::clamp<float>(maxScr.y, 0, target->height() - 1));
    ymin = std::max<int>(ymin, bandY0);
    ymax = std::min<int>(ymax, bandY1 - 1);
    if (ymin > ymax) return;

    const float d1 = -(s2.y - s1.y) * sign;
    const float d2 = -(s3.y - s2.y) * sign;
    const float d3 = -(s1.y - s3.y) * sign;

    for (uint32_t y = ymin; y <= ymax; ++y) {
        Vec2f p0 = { xmin + 0.5f, y + 0.5f };
        float e1 = (s2.xy-s1.xy).cross(p0-s1.xy) * sign;
        float e2 = (s3.xy-s2.xy).cross(p0-s2.xy) * sign;
        float e3 = (s1.xy-s3.xy).cross(p0-s3.xy) * sign;
        
        for (uint32_t x = xmin; x <= xmax; ++x, e1 += d1, e2 += d2, e3 += d3) {            
            if (e1 >= 0 && e2 >= 0 && e3 >= 0) {
                // pixel is inside triangle

                // barycentrics
                float b1 = e2 / area;
                float b2 = e3 / area;
                float b3 = e1 / area;

                // early z-test
                float z = b1 * s1.z + b2 * s2.z + b3 * s3.z;
                if (z > 1.f) continue;
                if (cmd.depthTest && z >= target->getDepth(x, y)) continue;
                if (cmd.depthWrite) target->setDepth(x, y, z);

                // interpolate varyings
                float invW = b1 * s1.w + b2 * s2.w + b3 * s3.w;
                const float w = 1.f / invW;
                float interp[MAX_VARYINGS];
                for (uint32_t k = 0; k < program.varyingCount; ++k) {
                    interp[k] = 
                        (b1*varys[0][k]*s1.w + 
                         b2*varys[1][k]*s2.w + 
                         b3*varys[2][k]*s3.w) * w;
                }

                // execute fragment shader for this pixel, and write it into the backbuffer
                Color color = program.fsThunk(program.fsFn, interp, cmd.uniforms);
                writePixel(target, x, y, color, cmd.blendmode);
            }
        }
    }
}


struct ClipVert { Vec4f pos; float vary[MAX_VARYINGS]; };

inline int clipNear(
    const ClipVert in[3], 
    ClipVert out[4], 
    uint32_t varyingCount
) {
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % 3];
        float da = a.pos.z, db = b.pos.z;
        if (da >= 0) out[n++] = a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            out[n].pos = lerp(a.pos, b.pos, t);
            for (uint32_t k = 0; k < varyingCount; ++k)
                out[n].vary[k] = a.vary[k] + (b.vary[k] - a.vary[k]) * t;
            ++n;
        }
    }

    return n;
}


template <typename F>
void forEachBand(
    ThreadPool& pool,
    const uint32_t height,
    const std::vector<std::unique_ptr<RenderTarget>>* targets,
    F&& fn
) {
    const uint32_t bands = std::max(1u, std::min(pool.size() * 8, height));
    const uint32_t rows  = (height + bands - 1) / bands;

    pool.dispatch(bands, [&](uint32_t b) {
        detail::boundTargets = targets;
        const int y0 = int(b * rows);
        const int y1 = int(std::min((b + 1) * rows, height));
        if (y0 < y1) fn(y0, y1);
    });
}


inline void drawTriangles(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    std::vector<std::unique_ptr<RenderTarget>>* targets
) {
    const size_t stride = vb.layout.stride();

    constexpr size_t CHUNK = 4096;
    static std::vector<ClipVert> tris;
    tris.clear();

    auto rasterChunk = [&]() {
        forEachBand(pool, target->height(), targets, [&](int y0, int y1) {
            for (size_t k = 0; k + 2 < tris.size(); k += 3) {
                Vec4f pos[3]            = { tris[k].pos,    tris[k + 1].pos,    tris[k + 2].pos  };
                const float* varys[3]   = { tris[k].vary,   tris[k + 1].vary,   tris[k + 2].vary };

                if (cmd.fillmode == FillMode::Wireframe) {
                    Vec4f line1[2] = {pos[0], pos[1]};
                    Vec4f line2[2] = {pos[1], pos[2]};
                    Vec4f line3[2] = {pos[2], pos[0]};
                    const float* varys1[2] = {varys[0], varys[1]};
                    const float* varys2[2] = {varys[1], varys[2]};
                    const float* varys3[2] = {varys[2], varys[0]};
                    rasterizeLine(target, program, cmd, line1, varys1);
                    rasterizeLine(target, program, cmd, line2, varys2);
                    rasterizeLine(target, program, cmd, line3, varys3);
                }
                else rasterizeTriangle(target, program, cmd, pos, varys, y0, y1);
            }
        });
        tris.clear();
    };

    for (size_t i = 0; i < ib.indices.size(); i += 3) {
        // collect three verts
        const uint8_t* v1 = vb.verts.data() + ib.indices[i + 0] * stride;
        const uint8_t* v2 = vb.verts.data() + ib.indices[i + 1] * stride;
        const uint8_t* v3 = vb.verts.data() + ib.indices[i + 2] * stride;

        ClipVert in[3], clipped[4];

        in[0].pos = program.vsThunk(program.vsFn, v1, cmd.uniforms, in[0].vary);
        in[1].pos = program.vsThunk(program.vsFn, v2, cmd.uniforms, in[1].vary);
        in[2].pos = program.vsThunk(program.vsFn, v3, cmd.uniforms, in[2].vary);

        int n = clipNear(in, clipped, program.varyingCount);
        if (n < 3) continue;

        for (int t = 1; t + 1 < n; ++t) {
            tris.push_back(clipped[0]);
            tris.push_back(clipped[t]);
            tris.push_back(clipped[t + 1]);
        }

        if (tris.size() >= CHUNK * 3) rasterChunk();
    }

    rasterChunk();
}


inline void drawLines(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    std::vector<std::unique_ptr<RenderTarget>>* targets
) {
    const size_t stride = vb.layout.stride();

    for (size_t i = 0; i + 1 < ib.indices.size(); i += 2) {
        const uint8_t* v1 = vb.verts.data() + ib.indices[i + 0] * stride;
        const uint8_t* v2 = vb.verts.data() + ib.indices[i + 1] * stride;

        ClipVert a, b;
        a.pos = program.vsThunk(program.vsFn, v1, cmd.uniforms, a.vary);
        b.pos = program.vsThunk(program.vsFn, v2, cmd.uniforms, b.vary);

        if (a.pos.z < 0 && b.pos.z < 0) continue;
        if ((a.pos.z < 0) != (b.pos.z < 0)) {
            float t = a.pos.z / (a.pos.z - b.pos.z);
            ClipVert& out = (a.pos.z < 0) ? a : b;
            out.pos = lerp(a.pos, b.pos, t);

            for (uint32_t k = 0; k < program.varyingCount; ++k)
                out.vary[k] = a.vary[k] + (b.vary[k] - a.vary[k]) * t;
        }

        Vec4f pos[2]            = {a.pos, b.pos};
        const float* varys[2]   = {a.vary, b.vary};
        rasterizeLine(target, program, cmd, pos, varys);
    }
}


inline void drawPoints(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    std::vector<std::unique_ptr<RenderTarget>>* targets
) {
    size_t stride = vb.layout.stride();

    for (size_t i = 0; i < ib.indices.size(); ++i) {
        const uint8_t* v = vb.verts.data() + ib.indices[i] * stride;
        ClipVert a;

        valo_PointSize = 1.f;
        a.pos = program.vsThunk(program.vsFn, v, cmd.uniforms, a.vary);
        const float size = valo_PointSize;

        if (a.pos.z < 0) continue;
        rasterizePoint(target, program, cmd, a.pos, a.vary, size);
    }
}

}; // namespace valo