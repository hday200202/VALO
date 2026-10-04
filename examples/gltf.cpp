// glTF viewer: DamagedHelmet (the PBR reference model) or the Khronos Box,
// through a textured Cook-Torrance shader. Every glTF material slot is used:
// base color, packed metallic-roughness (G = roughness, B = metallic), normal
// map (tangents computed at load), occlusion, emissive. Lighting and sky are
// the same four lights + environment as pbr.cpp.
//
// Controls
//   left click    grab mouse and drag to rotate; right click releases
//   tab           switch model (helmet / box)
//   wheel         zoom
//   n             normal map on / off
//   b             bloom on / off
//   [ ]           bloom threshold down / up
//   1 2 3 4       render scale
//   escape        quit
//
// Bloom is four extra layers on top of the scene: the scene renders into its
// own target, a bright pass thresholds it, a separable Gaussian blurs that
// across two ping-pong targets, and a composite adds the result back over the
// scene on the way to the backbuffer. All three targets are created with
// createTarget(), so they track the window and are rebuilt on resize.

#include <VALO.hpp>
#include <cstring>
#include <utils/GLTFParser.hpp>
#include <cmath>
#include <numbers>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include "shaders/gltf.vert.hpp"
#include "shaders/gltf.frag.hpp"
#include "shaders/gltfsky.vert.hpp"
#include "shaders/gltfsky.frag.hpp"
#include "shaders/bloom.vert.hpp"
#include "shaders/bloomthresh.frag.hpp"
#include "shaders/bloomblur.frag.hpp"
#include "shaders/bloomcomposite.frag.hpp"

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Mat4f;
using valo::Color;
using valo::TextureID;


/* ===========================================================================
    Loading: glTF -> one vertex buffer + one draw per primitive per node
=========================================================================== */
struct DrawItem {
    valo::RenderCommand cmd;
    Mat4f    world;         // node's world transform, baked at load
    Uniforms uniforms;      // everything material-side, filled once
};

struct Model {
    std::string name;
    valo::VertexBufferID vb;
    valo::IndexBufferID  ib;
    std::vector<DrawItem> items;
    Vec3f center;           // of the world-space bounds, for framing
    float extent;
};

/*
    Tangents from UV derivatives (Lengyel's method).

    A normal map needs to know which way is "u" on the surface. glTF only ships
    that when the exporter felt like it - DamagedHelmet does not - so it is
    derived: for each triangle, solve for the direction in which u increases,
    accumulate it onto all three vertices, then normalise at the end. Summing
    per-vertex is what makes the result continuous across shared edges.
*/
static void computeTangents(std::vector<Vertex>& verts, const std::vector<uint32_t>& idx) {
    std::vector<Vec3f> tan(verts.size(), {0.f, 0.f, 0.f});
    std::vector<Vec3f> bit(verts.size(), {0.f, 0.f, 0.f});

    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const Vertex& a = verts[idx[i]];
        const Vertex& b = verts[idx[i + 1]];
        const Vertex& c = verts[idx[i + 2]];

        const Vec3f e1 = b.pos - a.pos;
        const Vec3f e2 = c.pos - a.pos;
        const Vec2f d1 = b.uv  - a.uv;
        const Vec2f d2 = c.uv  - a.uv;

        // the 2x2 UV matrix is singular on degenerate or unmapped triangles
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;

        const float r = 1.f / det;
        const Vec3f t  = (e1 * d2.y - e2 * d1.y) * r;
        const Vec3f bt = (e2 * d1.x - e1 * d2.x) * r;

        for (int k = 0; k < 3; ++k) {
            tan[idx[i + k]] += t;
            bit[idx[i + k]] += bt;
        }
    }

    for (size_t i = 0; i < verts.size(); ++i) {
        const Vec3f n = verts[i].normal;

        // remove any component along the normal, so t lies in the surface
        Vec3f t = tan[i] - n * n.dot(tan[i]);

        // a vertex no triangle contributed to (or a fully degenerate one) needs
        // some valid frame rather than a zero vector
        if (t.length() < 1e-8f) {
            t = std::fabs(n.x) < 0.9f ? Vec3f{1.f, 0.f, 0.f} : Vec3f{0.f, 1.f, 0.f};
            t = t - n * n.dot(t);
        }
        t = t.normalized();

        // handedness: does the bitangent we derived agree with n x t, or is the
        // UV island mirrored? The shader multiplies by this to match.
        const float w = n.cross(t).dot(bit[i]) < 0.f ? -1.f : 1.f;

        verts[i].tangent = { t, w };
    }
}

static Model loadModel(
    valo::VALO& valo,
    const std::string& path,
    const std::string& name,
    std::map<std::string, TextureID>& texCache
) {
    Model model;
    model.name = name;

    valo::GLTF g = valo::parseGLTF(path);
    if (g.vertices.empty()) return model;

    // the parser hands back pos/normal/uv; this example's vertex adds a tangent
    std::vector<Vertex> verts(g.vertices.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        verts[i] = { g.vertices[i].pos, g.vertices[i].normal, g.vertices[i].uv, {1.f, 0.f, 0.f, 1.f} };
    }
    computeTangents(verts, g.indices);

    const valo::VtxLayout layout({
        valo::VtxAttrib::Vec3f,     // pos
        valo::VtxAttrib::Vec3f,     // normal
        valo::VtxAttrib::Vec2f,     // uv
        valo::VtxAttrib::Vec4f,     // tangent
    });

    // one buffer for the whole file; primitives become index ranges into it
    model.vb = valo.createVertexBuffer(verts.data(), verts.size(), layout);
    model.ib = valo.createIndexBuffer(g.indices.data(), g.indices.size());

    // materials share images constantly, and the cache is keyed on path so the
    // same file is decoded once even across the two models
    auto tex = [&](const std::string& p) -> TextureID {
        if (p.empty()) return kNoTex;

        const auto it = texCache.find(p);
        if (it != texCache.end()) return it->second;

        return texCache[p] = valo.createTexture(p);
    };

    Vec3f lo = { 1e9f,  1e9f,  1e9f};
    Vec3f hi = {-1e9f, -1e9f, -1e9f};

    for (const valo::GLTFNode& node : g.nodes) {
        for (const valo::GLTFPrimitive& prim : g.meshes[node.mesh].primitives) {
            DrawItem item;
            item.world = node.world;

            item.cmd.verts      = model.vb;
            item.cmd.indices    = model.ib;
            item.cmd.firstIndex = prim.firstIndex;
            item.cmd.indexCount = prim.indexCount;

            Uniforms& u = item.uniforms;

            if (prim.material >= 0) {
                const valo::GLTFMaterial& m = g.materials[prim.material];

                u.baseColor = m.baseColor;
                u.metallic  = m.metallic;
                u.roughness = m.roughness;
                u.emissive  = m.emissive;

                u.baseTex     = tex(m.baseColorTex);
                u.mrTex       = tex(m.metallicRoughnessTex);
                u.normalTex   = tex(m.normalTex);
                u.aoTex       = tex(m.occlusionTex);
                u.emissiveTex = tex(m.emissiveTex);
            } else {
                // glTF allows a primitive with no material at all
                u.baseColor = {0.8f, 0.8f, 0.8f, 1.f};
                u.metallic  = 0.f;
                u.roughness = 0.6f;
                u.emissive  = {0.f, 0.f, 0.f};

                u.baseTex = u.mrTex = u.normalTex = u.aoTex = u.emissiveTex = kNoTex;
            }

            model.items.push_back(item);

            // bounds in world space, so the framing below works whatever units
            // and placement the file happened to use
            for (uint32_t k = prim.firstIndex; k < prim.firstIndex + prim.indexCount; ++k) {
                const Vec3f p = (node.world * Vec4f(verts[g.indices[k]].pos, 1.f)).xyz;
                lo = min(lo, p);
                hi = max(hi, p);
            }
        }
    }

    model.center = (lo + hi) * 0.5f;
    model.extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});

    return model;
}


/* ===========================================================================
    Main
=========================================================================== */
int main(int argc, char** argv) {
    // one scene, either backend: the software path resolves the shader
    // paths to registered C++ callbacks, the Vulkan path translates them
    bool useVulkan = false, validation = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--vulkan"))     useVulkan  = true;
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    }

    valo::Window window("VALO - glTF", {1280, 960},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = true;
    cfg.validation = validation;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;

    std::map<std::string, TextureID> texCache;
    Model models[2] = {
        loadModel(valo, "examples/gltf/mech/scene.gltf", "mech", texCache),
        loadModel(valo, "examples/gltf/Box/Box.gltf",                     "Box",           texCache),
    };
    if (models[0].items.empty() && models[1].items.empty()) return 1;

    const valo::ProgramID prog = valo.createProgram("examples/shaders/gltf.vert.hpp", "examples/shaders/gltf.frag.hpp");

    // --- sky: one quad, never depth tested, drawn first ---
    SkyVertex quad[4]   = { {{-1.f, -1.f}}, {{1.f, -1.f}}, {{1.f, 1.f}}, {{-1.f, 1.f}} };
    uint32_t  quadIdx[6] = { 0, 1, 2, 0, 2, 3 };

    valo::RenderCommand sky;
    sky.verts      = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    sky.indices    = valo.createIndexBuffer(quadIdx, 6);
    sky.program    = valo.createProgram("examples/shaders/gltfsky.vert.hpp", "examples/shaders/gltfsky.frag.hpp");
    sky.depthTest  = false;
    sky.depthWrite = false;
    sky.cullmode   = valo::CullMode::None;

    /* --- bloom: three window-tracking targets and four layers --------------

        Draw order is scene -> threshold -> blur H -> blur V -> composite, and
        each pass samples the target the previous one wrote. The backend
        inserts the COLOR_ATTACHMENT -> SHADER_READ_ONLY transition when the
        rendering scope closes, so the layers just have to be ordered.
    */
    const valo::RenderTargetID sceneTarget = valo.createTarget();

    /*
        The blur buffers are deliberately small and a fixed size. Blurring at
        full resolution needs the taps spread several texels apart to cover any
        useful radius, and a 9-tap kernel at that spacing is a sparse comb, not
        a Gaussian - a bright pixel lands as nine discrete dots. At 480x270 the
        taps sit one texel apart, so the kernel is continuous, and one texel
        here is ~2.7 screen pixels. The bright pass does the downsample for
        free by rendering the full-res scene into them with a linear sampler.

        Fixed rather than window-tracking because nothing about a low-frequency
        glow needs to follow the window, and the passes sample in normalised uv
        so any window size works without recreating them.
    */
    constexpr uint32_t kBloomW = 480, kBloomH = 270;
    const valo::RenderTargetID bloomA = valo.createTarget(kBloomW, kBloomH);
    const valo::RenderTargetID bloomB = valo.createTarget(kBloomW, kBloomH);

    const valo::LayerID sceneLayer  = valo.addLayer(sceneTarget);
    const valo::LayerID threshLayer = valo.addLayer(bloomA);
    const valo::LayerID blurHLayer  = valo.addLayer(bloomB);
    const valo::LayerID blurVLayer  = valo.addLayer(bloomA);
    valo.setLayerOrder({sceneLayer, threshLayer, blurHLayer, blurVLayer, valo::VALO::BASE_LAYER});

    BloomVertex postQuad[4]  = {{{0.f, 0.f}}, {{1.f, 0.f}}, {{1.f, 1.f}}, {{0.f, 1.f}}};
    uint32_t    postQuadIdx[6] = {0, 1, 2, 0, 2, 3};

    valo::RenderCommand post;
    post.verts     = valo.createVertexBuffer(postQuad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    post.indices   = valo.createIndexBuffer(postQuadIdx, 6);
    post.cullmode  = valo::CullMode::None;
    post.depthTest = false;
    post.depthWrite = false;

    const valo::ProgramID threshProg = valo.createProgram(
        "examples/shaders/bloom.vert.hpp", "examples/shaders/bloomthresh.frag.hpp");
    const valo::ProgramID blurProg = valo.createProgram(
        "examples/shaders/bloom.vert.hpp", "examples/shaders/bloomblur.frag.hpp");
    const valo::ProgramID compositeProg = valo.createProgram(
        "examples/shaders/bloom.vert.hpp", "examples/shaders/bloomcomposite.frag.hpp");

    int   which        = 0;
    bool  bloom        = true;
    float threshold    = 0.32f;
    float intensity    = 1.0f;
    // one blur-buffer texel; see the note on kBloomW for why this stays at 1
    const float radius = 1.f;
    bool  useNormalMap = true;
    float zoom         = 1.f;
    Mat4f orientation;

    using clk = std::chrono::steady_clock;
    auto     fpsStart  = clk::now();
    auto     last      = clk::now();
    uint32_t fpsFrames = 0;
    int      fps       = 0;

    while (window.pollEvents()) {
        const auto frameStart = clk::now();
        const float dt = std::min(std::chrono::duration<float>(frameStart - last).count(), 0.1f);
        last = frameStart;

        /* --- input --- */
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::Tab))    which = 1 - which;
        if (window.wasKeyPressed(valo::Key::N))      useNormalMap = !useNormalMap;
        if (window.wasKeyPressed(valo::Key::B))      bloom = !bloom;
        // held, scaled by dt. Deliberately slow: the useful range on this scene is
        // only about 0.30-0.40 wide, so a fast rate makes it untunable
        if (window.isKeyDown(valo::Key::LeftBracket))
            threshold = std::clamp(threshold - 0.05f * dt, 0.f, 0.95f);
        if (window.isKeyDown(valo::Key::RightBracket))
            threshold = std::clamp(threshold + 0.05f * dt, 0.f, 0.95f);
        if (window.wasKeyPressed(valo::Key::V))      valo.setVsync(!valo.getVsync());

        if (window.wasMousePressed(valo::MouseButton::Left))  window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();

        if (window.isMouseGrabbed()) {
            // pre-multiplied, so each drag rotates about the SCREEN axes rather
            // than the model's own - which is what dragging an object feels like
            const Vec2f d = window.mouseDelta();
            orientation = Mat4f::rotationX(d.y * 0.005f)
                        * Mat4f::rotationY(d.x * 0.005f)
                        * orientation;
        }

        zoom = std::clamp(zoom - window.scrollDelta() * 0.05f, 0.3f, 3.f);

        if (window.wasKeyPressed(valo::Key::Num1)) valo.setRenderScale(1.00f);
        if (window.wasKeyPressed(valo::Key::Num2)) valo.setRenderScale(0.75f);
        if (window.wasKeyPressed(valo::Key::Num3)) valo.setRenderScale(0.50f);
        if (window.wasKeyPressed(valo::Key::Num4)) valo.setRenderScale(0.25f);

        /* --- title --- */
        ++fpsFrames;
        const auto now = clk::now();
        if (std::chrono::duration<float>(now - fpsStart).count() >= 1.f) {
            fps       = int(fpsFrames);
            fpsFrames = 0;
            fpsStart  = now;
        }

        const Model& model = models[which];
        window.setTitle(
            "VALO - glTF   " + model.name +
            "   " + std::to_string(model.items.size()) + " draw(s)" +
            (useNormalMap ? "" : "   [no normal map]") +
            (bloom ? "   bloom thresh " + std::to_string(int(threshold * 1000.f) / 10.f).substr(0, 4) : "   [no bloom]") +
            "   " + std::to_string(fps) + " fps"
        );

        /* --- camera ---

            Fit rather than trust the file: scale so the largest extent is about
            2 units and move the centre to the origin. glTF files disagree wildly
            on scale, so without this one model fills the screen and the next is
            a speck.
        */
        const float fit    = 2.f / std::max(model.extent, 1e-6f);
        const Vec3f camPos = { 0.f, 0.f, 4.2f * zoom };

        const Mat4f view = Mat4f::lookAt(camPos, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
        const float aspect = window.getAspect();
        const Mat4f proj = Mat4f::perspective(kFovY, aspect, 0.1f, 100.f);

        // read right to left: centre it, scale it, then apply the mouse rotation
        const Mat4f base = orientation
                         * Mat4f::scale({fit, fit, fit})
                         * Mat4f::translation(-model.center);

        /* --- draw --- */
        valo.beginFrame();

        sky.setUniforms(SkyUniforms{aspect, std::tan(kFovY * 0.5f)});
        valo.submit(sky, sceneLayer);

        for (const DrawItem& item : model.items) {
            Uniforms u = item.uniforms;              // material, filled at load
            u.model        = base * item.world;      // node placement, then framing
            u.mvp          = proj * view * u.model;
            u.camPos       = camPos;
            u.useNormalMap = useNormalMap;

            valo::RenderCommand cmd = item.cmd;
            cmd.program = prog;
            cmd.setUniforms(u);
            valo.submit(cmd, sceneLayer);
        }

        /* --- bloom passes ---

            The quad covers each target completely, so every pass shares one
            pixel-space ortho; only the source texture and texel step change.
        */
        const valo::Vec2u fb = window.getSize();
        const float fw = float(fb.x), fh = float(fb.y);
        const float bw = float(kBloomW), bh = float(kBloomH);

        const Mat4f postProj  = Mat4f::orthographic(0.f, fw, fh, 0.f, -1.f, 1.f);
        const Mat4f bloomProj = Mat4f::orthographic(0.f, bw, bh, 0.f, -1.f, 1.f);

        BloomUniforms bu{};
        bu.threshold = threshold;
        bu.intensity = intensity;
        bu.pos       = {0.f, 0.f};

        if (bloom) {
            // the bright pass and both blurs run in blur-buffer space
            bu.proj = bloomProj;
            bu.size = {bw, bh};

            bu.srcTex    = sceneTarget;
            bu.texelStep = {0.f, 0.f};
            post.program = threshProg;
            post.setUniforms(bu);
            valo.submit(post, threshLayer);

            bu.srcTex    = bloomA;
            bu.texelStep = {radius / bw, 0.f};
            post.program = blurProg;
            post.setUniforms(bu);
            valo.submit(post, blurHLayer);

            bu.srcTex    = bloomB;
            bu.texelStep = {0.f, radius / bh};
            post.setUniforms(bu);
            valo.submit(post, blurVLayer);
        }

        // the composite covers the window, not the blur buffers
        bu.proj = postProj;
        bu.size = {fw, fh};

        // composite to the backbuffer; with bloom off this is a straight copy,
        // since the scene no longer renders there directly
        bu.srcTex    = sceneTarget;
        bu.bloomTex  = bloomA;
        bu.intensity = bloom ? intensity : 0.f;
        post.program = compositeProg;
        post.setUniforms(bu);
        valo.submit(post, valo::VALO::BASE_LAYER);

        valo.endFrame();
    }

    return 0;
}
