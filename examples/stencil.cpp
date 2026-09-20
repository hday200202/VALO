// Stencil buffer demo on DamagedHelmet.
//
// Two classic stencil techniques, both pure RenderCommand state:
//
//   OUTLINE  - pass 1 draws the helmet and stamps stencil = 1 wherever it lands
//              (stencilOp = Replace). Pass 2 draws the helmet again, extruded
//              along its normals and solid black, but only where stencil != 1
//              (stencilTest = NotEqual) - so only the extruded rim outside the
//              original silhouette survives. No depth test, so the outline is
//              never hidden by the model itself.
//
//   CLIP     - a fullscreen quad whose fragment shader `discard`s outside a
//              circle is drawn with colorWrite = false and stencilOp = Incr,
//              leaving stencil = 1 inside the circle. The helmet is then drawn
//              with stencilTest = Equal, ref 1: it only appears through the hole.
//              This is exactly what a UI clip stack does with rounded rects.
//
// Controls
//   left click / drag   rotate (right click releases)
//   o                   outline on / off
//   c                   circular clip on / off
//   wheel               outline width
//   escape              quit

#include <VALO.hpp>
#include <utils/GLTFParser.hpp>
#include <cmath>
#include <numbers>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Mat4f;
using valo::Color;

constexpr float kFovY = 0.9f;

// ---------------------------------------------------------------------------
// helmet: base color texture * simple two-light lambert. The shading is
// deliberately plain so the stencil work is what you're looking at.
// ---------------------------------------------------------------------------
// smoothNormal: normals averaged across every vertex sharing a position. The
// helmet has split normals at its hard edges; extruding along those tears the
// outline shell open at every seam, extruding along the smoothed ones doesn't.
struct Vertex   { Vec3f pos; Vec3f normal; Vec2f uv; Vec3f smoothNormal; };
struct Uniforms { Mat4f mvp, model; valo::TextureID base; float outline; };
struct Varyings { Vec3f normal; Vec2f uv; };

static Vec4f vs(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.normal = (u.model * Vec4f(in.normal, 0.f)).xyz;
    out.uv = in.uv;
    return u.mvp * Vec4f(in.pos, 1.f);
}

static Color fs(const Varyings& in, const Uniforms& u) {
    const Vec3f n = in.normal.normalized();
    const float key  = std::max(n.dot(Vec3f{-0.5f, 0.6f, 0.6f}.normalized()), 0.f);
    const float fill = std::max(n.dot(Vec3f{ 0.7f, 0.1f, 0.4f}.normalized()), 0.f) * 0.4f;
    const Vec3f albedo = texture(u.base, in.uv).xyz;
    const Vec3f lit = albedo * (0.18f + 0.9f * key + fill);
    return {{std::pow(lit.x, 1.f / 2.2f), std::pow(lit.y, 1.f / 2.2f), std::pow(lit.z, 1.f / 2.2f)}, 1.f};
}

// outline pass: same mesh, pushed out along the normal in object space, flat black
static Vec4f outlineVs(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.normal = in.normal; out.uv = in.uv;
    return u.mvp * Vec4f(in.pos + in.smoothNormal * u.outline, 1.f);
}
static Color outlineFs(const Varyings&, const Uniforms&) { return {0.f, 0.f, 0.f, 1.f}; }

// clip pass: fullscreen quad, discards outside a circle. colorWrite is off, so
// all it leaves behind is stencil.
struct ClipVertex { Vec2f ndc; }; struct ClipUniforms { float aspect, radius; }; struct ClipVaryings { Vec2f ndc; };
static Vec4f clipVs(const ClipVertex& in, const ClipUniforms&, ClipVaryings& out) { out.ndc = in.ndc; return {in.ndc.x, in.ndc.y, 0.5f, 1.f}; }
static Color clipFs(const ClipVaryings& in, const ClipUniforms& u) {
    if ((Vec2f{in.ndc.x * u.aspect, in.ndc.y}).length() > u.radius) discard;
    return {1.f, 1.f, 1.f, 1.f};
}

// ---------------------------------------------------------------------------
int main() {
    valo::Window window("VALO - stencil", {1280, 960});
    valo::VALO valo; valo::RenderConfig cfg; cfg.vsync = true;
    if (!valo.init(window, cfg)) return 1;
    valo.setLayerClearColor(valo::VALO::BASE_LAYER, {0.82f, 0.84f, 0.88f, 1.f});

    // ---- helmet ----
    valo::GLTF g = valo::parseGLTF("examples/gltf/DamagedHelmet/DamagedHelmet.gltf");
    if (g.vertices.empty()) return 1;
    std::vector<Vertex> verts(g.vertices.size());
    for (size_t i = 0; i < verts.size(); ++i) verts[i] = {g.vertices[i].pos, g.vertices[i].normal, g.vertices[i].uv, {0, 0, 0}};
    {   // smoothed normals: accumulate per unique position, then normalize
        struct Key { int x, y, z; bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; } };
        struct Hash { size_t operator()(const Key& k) const { return size_t(k.x) * 73856093u ^ size_t(k.y) * 19349663u ^ size_t(k.z) * 83492791u; } };
        auto key = [](const Vec3f& p) { return Key{int(std::lround(p.x * 1e4f)), int(std::lround(p.y * 1e4f)), int(std::lround(p.z * 1e4f))}; };
        std::unordered_map<Key, Vec3f, Hash> acc;
        for (const Vertex& v : verts) acc[key(v.pos)] += v.normal;
        for (Vertex& v : verts) v.smoothNormal = acc[key(v.pos)].normalized();
    }
    Vec3f lo = {1e9f, 1e9f, 1e9f}, hi = {-1e9f, -1e9f, -1e9f};
    for (const Vertex& v : verts) { lo = min(lo, v.pos); hi = max(hi, v.pos); }
    const Vec3f center = (lo + hi) * 0.5f;
    const float fit = 2.f / std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});

    valo::VtxLayout layout({valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec2f, valo::VtxAttrib::Vec3f});
    const valo::VertexBufferID vb = valo.createVertexBuffer(verts.data(), verts.size(), layout);
    const valo::IndexBufferID  ib = valo.createIndexBuffer(g.indices.data(), g.indices.size());
    const valo::TextureID base = valo.createTexture(g.materials[0].baseColorTex);
    const Mat4f nodeWorld = g.nodes[0].world;

    // pass 1: the helmet, stamping stencil = 1
    valo::RenderCommand helmet;
    helmet.verts = vb; helmet.indices = ib;
    helmet.program    = valo.createProgram(vs, fs);
    helmet.stencilOp  = valo::StencilOp::Replace;
    helmet.stencilRef = 1;

    // pass 2: extruded black shell, only where stencil != 1
    valo::RenderCommand outline = helmet;
    outline.program     = valo.createProgram(outlineVs, outlineFs);
    outline.stencilOp   = valo::StencilOp::Keep;
    outline.stencilTest = valo::StencilTest::NotEqual;
    outline.stencilRef  = 1;
    outline.depthTest   = false;
    outline.depthWrite  = false;

    // clip: circle -> stencil 1, drawn before the helmet when enabled
    ClipVertex quad[4] = {{{-1, -1}}, {{1, -1}}, {{1, 1}}, {{-1, 1}}}; uint32_t quadIdx[6] = {0, 1, 2, 0, 2, 3};
    valo::RenderCommand clip;
    clip.verts = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    clip.indices = valo.createIndexBuffer(quadIdx, 6);
    clip.program    = valo.createProgram(clipVs, clipFs);
    clip.cullmode   = valo::CullMode::None;
    clip.colorWrite = false; clip.depthTest = false; clip.depthWrite = false;
    clip.stencilOp  = valo::StencilOp::Incr;

    Mat4f orientation = Mat4f::rotationY(0.6f);
    bool showOutline = true, showClip = false;
    float outlineWidth = 0.03f;
    const Vec3f camPos = {0.f, 0.f, 4.2f};
    const Mat4f view = Mat4f::lookAt(camPos, {0, 0, 0}, {0, 1, 0});
    using clk = std::chrono::steady_clock; auto fpsStart = clk::now(); uint32_t fpsFrames = 0; int fps = 0;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::O)) showOutline = !showOutline;
        if (window.wasKeyPressed(valo::Key::C)) showClip = !showClip;
        if (window.wasMousePressed(valo::MouseButton::Left)) window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();
        if (window.isMouseGrabbed()) { const Vec2f d = window.mouseDelta(); orientation = Mat4f::rotationX(d.y * 0.005f) * Mat4f::rotationY(d.x * 0.005f) * orientation; }
        outlineWidth = std::clamp(outlineWidth + window.scrollDelta() * 0.005f, 0.f, 0.2f);

        ++fpsFrames; const auto now = clk::now();
        if (std::chrono::duration<float>(now - fpsStart).count() >= 1.f) { fps = int(fpsFrames); fpsFrames = 0; fpsStart = now; }
        window.setTitle(std::string("VALO - stencil   outline ") + (showOutline ? "on" : "off") + "  clip " + (showClip ? "on" : "off")
                        + "   width " + std::to_string(outlineWidth).substr(0, 5) + "   " + std::to_string(fps) + " fps");

        const float aspect = window.getAspect();
        const Mat4f proj  = Mat4f::perspective(kFovY, aspect, 0.1f, 100.f);
        const Mat4f model = orientation * Mat4f::scale({fit, fit, fit}) * Mat4f::translation(-center) * nodeWorld;
        const Uniforms u{proj * view * model, model, base, outlineWidth / fit};

        // Stencil plan per frame:
        //   clip off:  helmet Replace->1 everywhere it lands; outline where != 1
        //   clip on:   circle Incr->1; helmet where > 0 (inside the circle) and
        //              Incr - Greater rather than Equal so overlapping helmet
        //              triangles still get depth-tested instead of stencil-blocked;
        //              outline where == 1 (inside the circle, never hit by the helmet)
        valo.beginFrame();
        if (showClip) {
            clip.setUniforms(ClipUniforms{aspect, 0.32f});
            valo.submit(clip);
            helmet.stencilTest  = valo::StencilTest::Greater; helmet.stencilRef = 0; helmet.stencilOp = valo::StencilOp::Incr;
            outline.stencilTest = valo::StencilTest::Equal;  outline.stencilRef = 1;
        } else {
            helmet.stencilTest  = valo::StencilTest::Always; helmet.stencilRef  = 1; helmet.stencilOp = valo::StencilOp::Replace;
            outline.stencilTest = valo::StencilTest::NotEqual; outline.stencilRef = 1;
        }
        helmet.setUniforms(u);
        valo.submit(helmet);
        if (showOutline) { outline.setUniforms(u); valo.submit(outline); }
        valo.endFrame();
    }
    return 0;
}
