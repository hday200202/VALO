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

namespace valo {

inline void rasterizeTriangle(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip[3],
    const float* varys[3]
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

    for (uint32_t y = ymin; y <= ymax; ++y) {
        for (uint32_t x = xmin; x <= xmax; ++x) {
            Vec2f p = {x + 0.5f, y +0.5f};
            float e1 = (s2.xy-s1.xy).cross(p-s1.xy) * sign;
            float e2 = (s3.xy-s2.xy).cross(p-s2.xy) * sign;
            float e3 = (s1.xy-s3.xy).cross(p-s3.xy) * sign;
            
            if (e1 >= 0 && e2 >= 0 && e3 >= 0) {
                // pixel is inside triangle
                float b1 = e2 / area;
                float b2 = e3 / area;
                float b3 = e1 / area;

                float z = b1 * s1.z + b2 * s2.z + b3 * s3.z;
                if (z > 1.f) continue;

                float invW = b1 * s1.w + b2 * s2.w + b3 * s3.w;

                float interp[MAX_VARYINGS];
                for (int k = 0; k < program.varyingCount; ++k) {
                    interp[k] = 
                        (b1*varys[0][k]*s1.w + 
                         b2*varys[1][k]*s2.w + 
                         b3*varys[2][k]*s3.w) / invW;
                }

                if (cmd.depthTest && z >= target->getDepth(x, y)) continue;
                else if (cmd.depthWrite) target->setDepth(x, y, z);

                Color color = program.fsThunk(program.fsFn, interp, cmd.uniforms);
                target->setPixel(x, y, color);
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
            for (int k = 0; k < varyingCount; ++k)
                out[n].vary[k] = a.vary[k] + (b.vary[k] - a.vary[k]) * t;
            ++n;
        }
    }

    return n;
}


inline void drawTriangles(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd
) {
    const size_t stride = vb.layout.stride();

    for (size_t i = 0; i < ib.indices.size(); i += 3) {
        // collect thre verts
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
            Vec4f pos[3] = { clipped[0].pos,  clipped[t].pos,  clipped[t + 1].pos  };
            const float* varys[3] = { clipped[0].vary, clipped[t].vary, clipped[t + 1].vary };
            rasterizeTriangle(target, program, cmd, pos, varys);
        }
    }
}

}; // namespace valo