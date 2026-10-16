#include "DrawFuncs.hpp"

namespace valo {

bool stencilTest(const RenderTarget* target, const RenderCommand& cmd, int x, int y) {
    if (cmd.stencilTest == StencilTest::Always) return true;
    const uint8_t s = target->getStencil(x, y);
    switch (cmd.stencilTest) {
        case StencilTest::Equal:    return s == cmd.stencilRef;
        case StencilTest::NotEqual: return s != cmd.stencilRef;
        case StencilTest::Less:     return s <  cmd.stencilRef;
        case StencilTest::Greater:  return s >  cmd.stencilRef;
        default:                    return true;
    }
}

void stencilApply(RenderTarget* target, const RenderCommand& cmd, int x, int y) {
    if (cmd.stencilOp == StencilOp::Keep) return;
    const uint8_t s = target->getStencil(x, y);
    switch (cmd.stencilOp) {
        case StencilOp::Zero:    target->setStencil(x, y, 0);              break;
        case StencilOp::Replace: target->setStencil(x, y, cmd.stencilRef); break;
        case StencilOp::Incr:    target->setStencil(x, y, uint8_t(s + 1)); break;
        case StencilOp::Decr:    target->setStencil(x, y, uint8_t(s - 1)); break;
        default: break;
    }
}


Viewport resolveViewport(const RenderTarget* target, const RenderCommand& cmd) {
    Viewport vp = cmd.viewport;
    if (vp.width == 0.f) vp.width = float(target->width());
    if (vp.height == 0.f) vp.height = float(target->height());
    return vp;
}


PixelRect resolveClipRect(const RenderTarget* target, const RenderCommand& cmd) {
    const Viewport vp = resolveViewport(target, cmd);

    PixelRect r = {
        std::max(0, int(std::floor(vp.x))),
        std::max(0, int(std::floor(vp.y))),
        std::min(int(target->width()) - 1, int(std::ceil(vp.x + vp.width)) - 1),
        std::min(int(target->height()) - 1, int(std::ceil(vp.y + vp.height)) - 1)
    };

    const Scissor& sc = cmd.scissor;
    if (sc.width) {
        r.x0 = std::max(r.x0, sc.x);
        r.x1 = std::min(r.x1, sc.x + int(sc.width) - 1);
    }

    if (sc.height) {
        r.y0 = std::max(r.y0, sc.y);
        r.y1 = std::min(r.y1, sc.y + int(sc.height) - 1);
    }

    return r;
}


bool setupTriangle(
    const RenderTarget* target,
    const RenderCommand& cmd,
    const ClipVert& c0, const ClipVert& c1, const ClipVert& c2,
    const uint32_t varyingCount,
    ScreenTri& out
) {
    const ClipVert* c[3] = { &c0, &c1, &c2 };

    const Viewport vp = resolveViewport(target, cmd);

    for (int i = 0; i < 3; ++i) {
        const Vec4f& clip = c[i]->pos;
        const Vec4f ndc = { clip.xyz / clip.w, 1.f / clip.w };
        out.s[i] = {
            vp.x + (ndc.x + 1) * 0.5f * vp.width,
            vp.y + (1 - ndc.y) * 0.5f * vp.height,
            vp.minDepth + ndc.z * (vp.maxDepth - vp.minDepth),
            ndc.w
        };
    }

    float area = (out.s[1].xy - out.s[0].xy).cross(out.s[2].xy - out.s[0].xy);
    if (area == 0.f) return false;

    const bool ccw   = area < 0;
    const bool front = (cmd.winding == VtxWinding::CCW) ? ccw : !ccw;
    out.frontFacing = front;
    if (cmd.cullmode == CullMode::Back  && !front) return false;
    if (cmd.cullmode == CullMode::Front &&  front) return false;

    out.sign = area < 0 ? -1.f : 1.f;
    out.area = area * out.sign;
    out.invArea = 1.f / out.area;

    const Vec2f e[3] = {
        (out.s[1].xy - out.s[0].xy) * out.sign,
        (out.s[2].xy - out.s[1].xy) * out.sign,
        (out.s[0].xy - out.s[2].xy) * out.sign,
    };

    const float dE1dx = -e[0].y, dE1dy = e[0].x;
    const float dE2dx = -e[1].y, dE2dy = e[1].x;
    const float dE3dx = -e[2].y, dE3dy = e[2].x;

    out.dBdx[0] = dE2dx * out.invArea;
    out.dBdx[1] = dE3dx * out.invArea;
    out.dBdx[2] = dE1dx * out.invArea;

    out.dBdy[0] = dE2dy * out.invArea;
    out.dBdy[1] = dE3dy * out.invArea;
    out.dBdy[2] = dE1dy * out.invArea;

    out.dDenDx = out.dBdx[0] * out.s[0].w + out.dBdx[1] * out.s[1].w + out.dBdx[2] * out.s[2].w;
    out.dDenDy = out.dBdy[0] * out.s[0].w + out.dBdy[1] * out.s[1].w + out.dBdy[2] * out.s[2].w;

    for (int i = 0; i < 3; ++i)
        out.topLeft[i] = (e[i].y < 0.f) || (e[i].y == 0.f && e[i].x > 0.f);

    const PixelRect r = resolveClipRect(target, cmd);
    if (r.x0 > r.x1 || r.y0 > r.y1) return false;

    const Vec2f lo = min(min(out.s[0].xy, out.s[1].xy), out.s[2].xy);
    const Vec2f hi = max(max(out.s[0].xy, out.s[1].xy), out.s[2].xy);
    out.xmin = int(std::floor(std::clamp<float>(lo.x, float(r.x0), float(r.x1))));
    out.ymin = int(std::floor(std::clamp<float>(lo.y, float(r.y0), float(r.y1))));
    out.xmax = int(std::ceil (std::clamp<float>(hi.x, float(r.x0), float(r.x1))));
    out.ymax = int(std::ceil (std::clamp<float>(hi.y, float(r.y0), float(r.y1))));

    for (int i = 0; i < 3; ++i)
        std::memcpy(out.vary[i], c[i]->vary, varyingCount * sizeof(float));

    return true;
}


int clipNear(
    const ClipVert in[3], 
    ClipVert out[4], 
    uint32_t varyingCount
) {
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % 3];
        float da = a.pos.w - a.pos.z;
        float db = b.pos.w - b.pos.z;
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

void writePixel(
    RenderTarget* target, 
    const int x, 
    const int y, 
    const Color& src, 
    const BlendMode mode
) {
    if (target->isSrgb()) {
        uint32_t* const px = target->data() + (size_t(y) * target->width() + size_t(x));

        switch (mode) {
            case BlendMode::Opaque: {
                *px = linearToABGR32Srgb(src);
                break;
            }

            case BlendMode::Alpha: {
                const Color dst = abgr32ToLinear(*px);
                const float alpha = src.w;
                Color out = src * alpha + dst * (1.f - alpha);
                out.w = alpha + dst.w * (1.f - alpha);
                *px = linearToABGR32Srgb(out);
                break;
            }

            case BlendMode::Additive: {
                const Color dst = abgr32ToLinear(*px);
                const float alpha = src.w;
                Color out = dst + src * alpha;
                out.w = dst.w;
                *px = linearToABGR32Srgb(out);
                break;
            }

            default: break;
        }
        return;
    }

    switch (mode) {
        case BlendMode::Opaque: {
            target->setPixel(x, y, src); 
            break;
        }

        case BlendMode::Alpha: {
            const Color dst = target->getPixel(x, y);
            const float alpha = src.w;
            Color out = src * alpha + dst * (1.f - alpha);
            out.w = alpha + dst.w * (1.f - alpha);
            target->setPixel(x, y, out);
            break;
        }

        case BlendMode::Additive: {
            const Color dst = target->getPixel(x, y);
            const float alpha = src.w;
            Color out = dst + src * alpha;
            out.w = dst.w;
            target->setPixel(x, y, out);
            break;
        }

        default: break;
    }
}


void rasterizePoint(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip,
    const float* varys,
    const float size,
    const int bandY0, const int bandY1
) {
    const Viewport  vp = resolveViewport(target, cmd);
    const PixelRect r  = resolveClipRect(target, cmd);
    if (r.x0 > r.x1 || r.y0 > r.y1) return;

    Vec4f ndc = {clip.xyz / clip.w, clip.w};

    Vec4f screen = {
        vp.x + (ndc.x + 1) * 0.5f * vp.width,
        vp.y + (1 - ndc.y) * 0.5f * vp.height,
        vp.minDepth + ndc.z * (vp.maxDepth - vp.minDepth),
        ndc.w
    };

    int px = int(screen.x), py = int(screen.y);
    if (px < r.x0 || px > r.x1 || py < r.y0 || py > r.y1) return;

    const bool lateZ = cmd.fragDepthWrite && program.fsThunk;

    detail::varyingDerivs.valid = false;

    const int n    = std::max(1, int(size + 0.5f));
    const int half = n / 2;
    const int x0   = px - half, y0 = py - half;

    // every covered pixel is its own fragment, as on the GPU: tested, shaded and
    // written by the one band that owns its row
    for (int y = y0; y < y0 + n; ++y) {
        if (y < bandY0 || y >= bandY1 || y < r.y0 || y > r.y1) continue;
        for (int x = x0; x < x0 + n; ++x) {
            if (x < r.x0 || x > r.x1) continue;

            if (!lateZ && cmd.depthTest && !depthPasses(cmd.depthCompare, screen.z, target->getDepth(x, y))) continue;
            if (!stencilTest(target, cmd, x, y)) continue;

            float depth = screen.z;

            if (program.fsThunk) {
                valo_Discard     = false;
                valo_FragDepth   = screen.z;
                valo_FragCoord   = { x + 0.5f, y + 0.5f, screen.z, 1.f / screen.w };
                valo_FrontFacing = true;
                valo_PointCoord  = { (x - x0 + 0.5f) / n, (y - y0 + 0.5f) / n };

                const Color color = program.fsThunk(program.fsFn, varys, cmd.uniforms);
                if (valo_Discard) continue;

                if (lateZ) {
                    depth = std::clamp(valo_FragDepth, 0.f, 1.f);
                    if (cmd.depthTest && !depthPasses(cmd.depthCompare, depth, target->getDepth(x, y))) continue;
                }

                if (cmd.colorWrite) writePixel(target, x, y, color, cmd.blendmode);
            }

            if (cmd.depthWrite) target->setDepth(x, y, depth);
            stencilApply(target, cmd, x, y);
        }
    }
}

void rasterizeLine(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const Vec4f clip[2],
    const float* varys[2],
    const int bandY0, const int bandY1
) {
    const bool lateZ = cmd.fragDepthWrite && program.fsThunk;

    const Viewport  vp = resolveViewport(target, cmd);
    const PixelRect r  = resolveClipRect(target, cmd);
    if (r.x0 > r.x1 || r.y0 > r.y1) return;

    Vec4f ndc1 = {clip[0].xyz / clip[0].w, 1.f / clip[0].w};
    Vec4f ndc2 = {clip[1].xyz / clip[1].w, 1.f / clip[1].w};

    Vec4f s1 = {
        vp.x + (ndc1.x + 1) * 0.5f * vp.width,
        vp.y + (1 - ndc1.y) * 0.5f * vp.height,
        vp.minDepth + ndc1.z * (vp.maxDepth - vp.minDepth),
        ndc1.w
    };

    Vec4f s2 = {
        vp.x + (ndc2.x + 1) * 0.5f * vp.width,
        vp.y + (1 - ndc2.y) * 0.5f * vp.height,
        vp.minDepth + ndc2.z * (vp.maxDepth - vp.minDepth),
        ndc2.w
    };

    float dx = s2.x - s1.x;
    float dy = s2.y - s1.y;

    int steps = int(std::max(std::abs(dx), std::abs(dy)));

    const int by0 = std::max(bandY0, r.y0);
    const int by1 = std::min(bandY1 - 1, r.y1);
    if (by0 > by1) return;

    int i0 = 0, i1 = steps;
    if (std::abs(dy) > 1e-6f) {
        const float ta = (float(by0)     - s1.y) / dy;
        const float tb = (float(by1 + 1) - s1.y) / dy;
        const float lo = std::min(ta, tb), hi = std::max(ta, tb);
        i0 = std::max(0,     int(std::floor(lo * float(std::max(steps, 1)))) - 1);
        i1 = std::min(steps, int(std::ceil (hi * float(std::max(steps, 1)))) + 1);
        if (i0 > i1) return;
    } else if (int(s1.y) < by0 || int(s1.y) > by1)
        return;

    for (int i = i0; i <= i1; ++i) {
        float t = float(i) / std::max(steps, 1);
        float x = s1.x + dx * t;
        float y = s1.y + dy * t;
        int px = int(x), py = int(y);
        if (px < r.x0 || px > r.x1 || py < by0 || py > by1) continue;

        float z = s1.z + (s2.z - s1.z) * t;
        if (!lateZ && cmd.depthTest && !depthPasses(cmd.depthCompare, z, target->getDepth(px, py))) continue;
        if (!stencilTest(target, cmd, px, py)) continue;

        valo_FragDepth = z;

        detail::varyingDerivs.valid = false;

        Color color;
        if (program.fsThunk) {
            float invW = s1.w + (s2.w - s1.w) * t;
            const float w = 1.f / invW;

            float interp[MAX_VARYINGS];
            for (uint32_t k = 0; k < program.varyingCount; ++k) {
                interp[k] = (varys[0][k]*s1.w*(1-t) + varys[1][k]*s2.w*t) * w;
            }

            valo_Discard = false;
            valo_FragCoord = { x + 0.5f, y + 0.5f, z, invW };
            valo_FrontFacing = true;
            color = program.fsThunk(program.fsFn, interp, cmd.uniforms);
            if (valo_Discard) continue;
        }

        const float depth = std::clamp(valo_FragDepth, 0.f, 1.f);
        if (lateZ && cmd.depthTest && !depthPasses(cmd.depthCompare, depth, target->getDepth(px, py))) continue;

        if (cmd.depthWrite) target->setDepth(px, py, depth);
        stencilApply(target, cmd, px, py);
        if (cmd.colorWrite && program.fsThunk) writePixel(target, px, py, color, cmd.blendmode);
    }
}

void rasterizeTriangle(
    RenderTarget* target,
    const Program& program,
    const RenderCommand& cmd,
    const ScreenTri& tri,
    const int bandY0, const int bandY1
) {
    const int ymin = std::max(tri.ymin, bandY0);
    const int ymax = std::min(tri.ymax, bandY1 - 1);
    if (ymin > ymax) return;

    const bool lateZ = cmd.fragDepthWrite && program.fsThunk;

    const Vec4f& s1 = tri.s[0]; const Vec4f& s2 = tri.s[1]; const Vec4f& s3 = tri.s[2];
    const float sign = tri.sign;
    const int xmin = tri.xmin, xmax = tri.xmax;
    const float* varys[3] = { tri.vary[0], tri.vary[1], tri.vary[2] };

    const float d1 = -(s2.y - s1.y) * sign;
    const float d2 = -(s3.y - s2.y) * sign;
    const float d3 = -(s1.y - s3.y) * sign;

    auto& dv = detail::varyingDerivs;
    for (int i = 0; i < 3; ++i) {
        dv.vary[i] = tri.vary[i];
        dv.w[i]    = tri.s[i].w;
        dv.dBdx[i] = tri.dBdx[i];
        dv.dBdy[i] = tri.dBdy[i];
    }
    dv.dDenDx = tri.dDenDx;
    dv.dDenDy = tri.dDenDy;
    dv.valid  = true;

    for (int y = ymin; y <= ymax; ++y) {
        Vec2f p0 = { xmin + 0.5f, y + 0.5f };
        float e1 = (s2.xy-s1.xy).cross(p0-s1.xy) * sign;
        float e2 = (s3.xy-s2.xy).cross(p0-s2.xy) * sign;
        float e3 = (s1.xy-s3.xy).cross(p0-s3.xy) * sign;
        
        for (int x = xmin; x <= xmax; ++x, e1 += d1, e2 += d2, e3 += d3) {            
            if (
                (e1 > 0 || (e1 == 0 && tri.topLeft[0])) &&
                (e2 > 0 || (e2 == 0 && tri.topLeft[1])) &&
                (e3 > 0 || (e3 == 0 && tri.topLeft[2]))
            ) {
                // pixel is inside triangle

                // barycentrics
                float b1 = e2 * tri.invArea;
                float b2 = e3 * tri.invArea;
                float b3 = e1 * tri.invArea;

                float z = b1 * s1.z + b2 * s2.z + b3 * s3.z;
                if (z < 0.f || z > 1.f) continue;
                if (!lateZ && cmd.depthTest && !depthPasses(cmd.depthCompare, z, target->getDepth(x, y))) continue;
                if (!stencilTest(target, cmd, x, y)) continue;

                valo_FragDepth = z;

                Color color;
                if (program.fsThunk) {
                    float invW = b1 * s1.w + b2 * s2.w + b3 * s3.w;
                    const float w = 1.f / invW;
                    float interp[MAX_VARYINGS];
                    for (uint32_t k = 0; k < program.varyingCount; ++k) {
                        interp[k] = 
                            (b1*varys[0][k]*s1.w + 
                             b2*varys[1][k]*s2.w + 
                             b3*varys[2][k]*s3.w) * w;
                    }

                    valo_Discard = false;
                    valo_FragCoord = { x + 0.5f, y + 0.5f, z, invW };
                    valo_FrontFacing = tri.frontFacing;

                    dv.interp = interp;
                    dv.invW   = invW;

                    color = program.fsThunk(program.fsFn, interp, cmd.uniforms);
                    if (valo_Discard) continue;
                }

                const float depth = std::clamp(valo_FragDepth, 0.f, 1.f);
                if (lateZ && cmd.depthTest && !depthPasses(cmd.depthCompare, depth, target->getDepth(x, y))) continue;

                if (cmd.depthWrite) target->setDepth(x, y, depth);
                stencilApply(target, cmd, x, y);
                if (cmd.colorWrite && program.fsThunk) writePixel(target, x, y, color, cmd.blendmode);
            }
        }
    }
}


void drawTriangles(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    const DrawContext& ctx,
    TriBatch& batch
) {
    const size_t stride = vb.layout.stride();

    const size_t begin = std::min(static_cast<size_t>(cmd.firstIndex), ib.indices.size());
    const size_t end = (cmd.indexCount == ALL_INDICES) ? 
        ib.indices.size() : 
        std::min(begin + cmd.indexCount, ib.indices.size());

    constexpr size_t CHUNK = 4096;

    if (cmd.fillmode == FillMode::Wireframe) {
        if (!batch.tris.empty() || (batch.target && batch.target != target))
            flushTriBatch(batch, pool, ctx);
        batch.target = target;

        DrawState wstate;
        wstate.target  = target;
        wstate.program = program;
        wstate.cmd     = cmd;
        if (ctx.uniforms)
            for (uint32_t i = 0; i < MAX_UNIFORM_SLOTS; ++i) wstate.uniforms[i] = ctx.uniforms[i];
        for (uint32_t i = 0; i < MAX_STORAGE_SLOTS; ++i) { wstate.storage[i] = ctx.storage[i]; wstate.storageBytes[i] = ctx.storageBytes[i]; }

        batch.states.push_back(wstate);
        uint32_t wi = uint32_t(batch.states.size() - 1);

        for (uint32_t inst = 0; inst < cmd.instanceCount; ++inst) {
            valo_InstanceIndex = inst;

            for (size_t i = begin; i + 2 < end; i += 3) {
                const uint8_t* v1 = vb.verts.data() + ib.indices[i + 0] * stride;
                const uint8_t* v2 = vb.verts.data() + ib.indices[i + 1] * stride;
                const uint8_t* v3 = vb.verts.data() + ib.indices[i + 2] * stride;

                ClipVert in[3], clipped[4];

                valo_VertexIndex = ib.indices[i];
                in[0].pos = program.vsThunk(program.vsFn, v1, cmd.uniforms, in[0].vary);

                valo_VertexIndex = ib.indices[i + 1];
                in[1].pos = program.vsThunk(program.vsFn, v2, cmd.uniforms, in[1].vary);

                valo_VertexIndex = ib.indices[i + 2];
                in[2].pos = program.vsThunk(program.vsFn, v3, cmd.uniforms, in[2].vary);

                int n = clipNear(in, clipped, program.varyingCount);
                if (n < 3) continue;

                for (int t = 1; t + 1 < n; ++t) {
                    WireTri wt;
                    wt.v[0] = clipped[0];
                    wt.v[1] = clipped[t];
                    wt.v[2] = clipped[t + 1];
                    wt.state = wi;
                    batch.wires.push_back(wt);
                }

                if (batch.wires.size() >= CHUNK) {
                    flushTriBatch(batch, pool, ctx);
                    batch.target = target;
                    batch.states.push_back(wstate);
                    wi = uint32_t(batch.states.size() - 1);
                }
            }
        }
        return;
    }

    if (!batch.wires.empty() || (batch.target && batch.target != target))
        flushTriBatch(batch, pool, ctx);
    batch.target = target;

    DrawState state;
    state.target  = target;
    state.program = program;
    state.cmd     = cmd;
    if (ctx.uniforms) {
        for (uint32_t i = 0; i < MAX_UNIFORM_SLOTS; ++i) state.uniforms[i] = ctx.uniforms[i];
        for (uint32_t i = 0; i < MAX_STORAGE_SLOTS; ++i) { state.storage[i] = ctx.storage[i]; state.storageBytes[i] = ctx.storageBytes[i]; }
    }

    batch.states.push_back(state);
    uint32_t stateIndex = uint32_t(batch.states.size() - 1);

    for (uint32_t inst = 0; inst < cmd.instanceCount; ++inst) {
        valo_InstanceIndex = inst;

        for (size_t i = begin; i + 2 < end; i += 3) {
            const uint8_t* v1 = vb.verts.data() + ib.indices[i + 0] * stride;
            const uint8_t* v2 = vb.verts.data() + ib.indices[i + 1] * stride;
            const uint8_t* v3 = vb.verts.data() + ib.indices[i + 2] * stride;

            ClipVert in[3], clipped[4];

            valo_VertexIndex = ib.indices[i];
            in[0].pos = program.vsThunk(program.vsFn, v1, cmd.uniforms, in[0].vary);

            valo_VertexIndex = ib.indices[i + 1];
            in[1].pos = program.vsThunk(program.vsFn, v2, cmd.uniforms, in[1].vary);

            valo_VertexIndex = ib.indices[i + 2];
            in[2].pos = program.vsThunk(program.vsFn, v3, cmd.uniforms, in[2].vary);

            int n = clipNear(in, clipped, program.varyingCount);
            if (n < 3) continue;

            for (int t = 1; t + 1 < n; ++t) {
                ScreenTri st;
                if (setupTriangle(target, cmd, clipped[0], clipped[t], clipped[t + 1], program.varyingCount, st)) {
                    st.state = stateIndex;
                    batch.tris.push_back(st);
                }
            }

            if (batch.tris.size() >= CHUNK) {
                flushTriBatch(batch, pool, ctx);
                batch.target = target;
                batch.states.push_back(state);
                stateIndex = uint32_t(batch.states.size() - 1);
            }
        }
    }
}


void flushTriBatch(TriBatch& batch, ThreadPool& pool, const DrawContext& ctx) {
    if (batch.empty() || !batch.target) {
        batch.tris.clear();
        batch.wires.clear();
        batch.states.clear();
        return;
    }

    RenderTarget* target = batch.target;

    forEachBand(pool, target->height(), ctx, [&](int y0, int y1) {
        for (const ScreenTri& tri : batch.tris) {
            const DrawState& st = batch.states[tri.state];
            detail::boundUniforms     = st.uniforms;
            detail::boundStorage      = st.storage;
            detail::boundStorageBytes = st.storageBytes;
            rasterizeTriangle(target, st.program, st.cmd, tri, y0, y1);
        }

        for (const WireTri& wt : batch.wires) {
            const DrawState& st = batch.states[wt.state];
            detail::boundUniforms     = st.uniforms;
            detail::boundStorage      = st.storage;
            detail::boundStorageBytes = st.storageBytes;

            const Vec4f  pos[3]   = { wt.v[0].pos,  wt.v[1].pos,  wt.v[2].pos  };
            const float* varys[3] = { wt.v[0].vary, wt.v[1].vary, wt.v[2].vary };

            Vec4f line1[2] = {pos[0], pos[1]};
            Vec4f line2[2] = {pos[1], pos[2]};
            Vec4f line3[2] = {pos[2], pos[0]};
            const float* varys1[2] = {varys[0], varys[1]};
            const float* varys2[2] = {varys[1], varys[2]};
            const float* varys3[2] = {varys[2], varys[0]};
            rasterizeLine(target, st.program, st.cmd, line1, varys1, y0, y1);
            rasterizeLine(target, st.program, st.cmd, line2, varys2, y0, y1);
            rasterizeLine(target, st.program, st.cmd, line3, varys3, y0, y1);
        }
    });

    batch.tris.clear();
    batch.wires.clear();
    batch.states.clear();
}


void drawLines(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    const DrawContext& ctx
) {
    const size_t stride = vb.layout.stride();
    const size_t begin = std::min(static_cast<size_t>(cmd.firstIndex), ib.indices.size());
    const size_t end = (cmd.indexCount == ALL_INDICES) ? 
        ib.indices.size() : 
        std::min(begin + cmd.indexCount, ib.indices.size());

    constexpr size_t CHUNK = 4096;
    static std::vector<ClipVert> lines;
    lines.clear();

    auto rasterChunk = [&]() {
        forEachBand(pool, target->height(), ctx, [&](int y0, int y1) {
            for (size_t k = 0; k + 1 < lines.size(); k += 2) {
                Vec4f pos[2]            = { lines[k].pos,   lines[k + 1].pos  };
                const float* varys[2]   = { lines[k].vary,  lines[k + 1].vary };
                rasterizeLine(target, program, cmd, pos, varys, y0, y1);
            }
        });
        lines.clear();
    };

    for (uint32_t inst = 0; inst < cmd.instanceCount; ++inst) {
        valo_InstanceIndex = inst;
        for (size_t i = begin; i + 1 < end; i += 2) {
            const uint8_t* v1 = vb.verts.data() + ib.indices[i + 0] * stride;
            const uint8_t* v2 = vb.verts.data() + ib.indices[i + 1] * stride;

            ClipVert a, b;

            valo_VertexIndex = ib.indices[i];
            a.pos = program.vsThunk(program.vsFn, v1, cmd.uniforms, a.vary);

            valo_VertexIndex = ib.indices[i + 1];
            b.pos = program.vsThunk(program.vsFn, v2, cmd.uniforms, b.vary);

            // near plane at z = w under reversed-Z, the same test clipNear uses
            const float da = a.pos.w - a.pos.z;
            const float db = b.pos.w - b.pos.z;

            if (da < 0 && db < 0) continue;
            if ((da < 0) != (db < 0)) {
                float t = da / (da - db);
                ClipVert& out = (da < 0) ? a : b;
                out.pos = lerp(a.pos, b.pos, t);

                for (uint32_t k = 0; k < program.varyingCount; ++k)
                    out.vary[k] = a.vary[k] + (b.vary[k] - a.vary[k]) * t;
            }

            lines.push_back(a);
            lines.push_back(b);
            if (lines.size() >= CHUNK * 2) rasterChunk();
        }
    }

    rasterChunk();
}


void drawPoints(
    RenderTarget* target,
    const VertexBuffer& vb,
    const IndexBuffer& ib,
    const Program& program,
    const RenderCommand& cmd,
    ThreadPool& pool,
    const DrawContext& ctx
) {
    size_t stride = vb.layout.stride();

    const size_t begin = std::min(static_cast<size_t>(cmd.firstIndex), ib.indices.size());
    const size_t end = (cmd.indexCount == ALL_INDICES) ? 
        ib.indices.size() : 
        std::min(begin + cmd.indexCount, ib.indices.size());

    struct PointRec { ClipVert v; float size; };
    
    constexpr size_t CHUNK = 4096;
    static std::vector<PointRec> points;
    points.clear();

    auto rasterChunk = [&]() {
        forEachBand(pool, target->height(), ctx, [&](int y0, int y1) {
            for (const PointRec& p : points)
                rasterizePoint(target, program, cmd, p.v.pos, p.v.vary, p.size, y0, y1);
        });
        points.clear();
    };

    for (uint32_t inst = 0; inst < cmd.instanceCount; ++inst) {
        valo_InstanceIndex = inst;
        for (size_t i = begin; i < end; ++i) {
            const uint8_t* v = vb.verts.data() + ib.indices[i] * stride;
            ClipVert a;

            valo_PointSize = 1.f;
            valo_VertexIndex = ib.indices[i];
            a.pos = program.vsThunk(program.vsFn, v, cmd.uniforms, a.vary);
            const float size = valo_PointSize;

            if (a.pos.w - a.pos.z < 0 || a.pos.w <= 0) continue;

            points.push_back({a, size});
            if (points.size() >= CHUNK) rasterChunk();
        }
    }

    rasterChunk();
}

} // namespace valo