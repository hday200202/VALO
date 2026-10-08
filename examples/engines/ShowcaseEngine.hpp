#pragma once

/*
    ShowcaseEngine - the minimal engine with the shading turned all the way up.

    Same shape as MinimalEngine (a Model you position, an Engine that owns the
    window and camera), but every surface goes through a Cook-Torrance metallic
    /roughness BRDF lit by a procedural sky and four movable lights, and the
    frame is assembled in five passes so the floor can mirror the model:

        1  sky      fullscreen quad, ray per pixel, no depth
        2  mirror   the model again through y = 0, front faces, full depth
        3  floor    alpha = 1 - fresnel, so pass 2 shows through at grazing angles
        4  model    the real thing, opaque then blended materials
        5  orbs     emissive spheres so the lights are visible in frame

    Everything is evaluated per pixel on the CPU, so it is a rasteriser wearing
    a ray tracer's clothes: the reflection is a mirrored draw rather than a
    traced ray, and the "IBL" is the same closed-form sky the background uses,
    sampled along the reflection vector and blurred by roughness.
*/

#include <string>
#include <deque>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>
#include "../include/VALO.hpp"
#include "BVH.hpp"
#include "../shaders/trace.vert.hpp"
#include "../shaders/trace.frag.hpp"
#include "../shaders/scobject.vert.hpp"
#include "../shaders/scobject.frag.hpp"
#include "../shaders/scsky.vert.hpp"
#include "../shaders/scsky.frag.hpp"
#include "../shaders/scfloor.vert.hpp"
#include "../shaders/scfloor.frag.hpp"
#include "../shaders/scorb.vert.hpp"
#include "../shaders/scorb.frag.hpp"

namespace sc {

using namespace valo;


// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
class Model {
public:
    Model() = default;

    void setVALO(VALO* valo)                    { m_valo = valo; }
    void setSceneBuffer(UniformBufferID scene)  { m_sceneBuffer = scene; }
    void setTransform(const Mat4f& transform)   { m_transform = transform; }
    void setPosition(const Vec3f& position)     { m_transform = Mat4f::translation(position); }
    bool isLoaded() const                       { return m_loaded; }

    /*
        glTF files agree on nothing about placement or units: of the models to
        hand one is 0.06 units end to end and another is 4, and most are centred
        on the origin rather than standing on it - which puts half the geometry
        under the floor, where the mirror pass reflects it back up through
        itself and it reads as a second copy of the model.

        So every model is fitted on load: scaled uniformly until its longest
        axis is targetSize, centred on x/z, and dropped until its lowest point
        rests on groundY. Calling setTransform() or setPosition() afterwards
        replaces this outright.
    */
    void fitToGround(const float targetSize, const float groundY) {
        if (!m_loaded) return;

        const Vec3f size = m_boundsMax - m_boundsMin;
        const float longest = std::max({size.x, size.y, size.z});
        if (!(longest > 1e-9f)) return;                      // also catches a NaN axis

        const float scale  = targetSize / longest;
        const Vec3f centre = (m_boundsMin + m_boundsMax) * 0.5f;

        m_transform = Mat4f::translation({-centre.x * scale,
                                          groundY - m_boundsMin.y * scale,
                                          -centre.z * scale})
                    * Mat4f::scale({scale, scale, scale});
    }

    Vec3f boundsMin() const { return m_boundsMin; }
    Vec3f boundsMax() const { return m_boundsMax; }

    // bounds with the model's own transform applied, so the contact shadow
    // tracks setPosition() instead of staying where the model was loaded
    void worldBounds(Vec3f& lo, Vec3f& hi) const {
        lo = { 1e30f,  1e30f,  1e30f};
        hi = {-1e30f, -1e30f, -1e30f};
        for (int c = 0; c < 8; ++c) {
            const Vec3f corner = {(c & 1) ? m_boundsMax.x : m_boundsMin.x,
                                  (c & 2) ? m_boundsMax.y : m_boundsMin.y,
                                  (c & 4) ? m_boundsMax.z : m_boundsMin.z};
            const Vec3f w = (m_transform * Vec4f{corner, 1.f}).xyz;
            lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
            hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
        }
    }

    bool loadGLTF(const std::string& path) {
        if (!m_valo) return false;

        const GLTF gltf = parseGLTF(path);
        if (gltf.vertices.empty() || gltf.indices.empty()) return false;

        const std::vector<Vertex> verts = buildTangents(gltf);
        m_vertices = m_valo->createVertexBuffer(verts.data(), verts.size(), Vertex::layout());
        m_indices  = m_valo->createIndexBuffer(gltf.indices.data(), gltf.indices.size());

        // the raster path hands these to the GPU and forgets them; the tracer
        // needs them on the CPU to build a BVH, so a copy stays behind
        m_cpuVerts   = verts;
        m_cpuIndices = gltf.indices;

        /*
            glTF splits normals at hard edges, which is right for a rasteriser
            but leaves a traced hit interpolating between two unrelated normals
            - the model reads as faceted. Averaging per unique position gives
            the tracer a continuous field to interpolate across.

            Quantised to 1e-4 before hashing, because two vertices at the same
            corner rarely agree to the last bit.
        */
        {
            struct Key {
                int x, y, z;
                bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
            };
            struct Hash {
                size_t operator()(const Key& k) const {
                    return size_t(k.x) * 73856093u ^ size_t(k.y) * 19349663u ^ size_t(k.z) * 83492791u;
                }
            };

            auto key = [](const Vec3f& p) {
                return Key{int(std::lround(p.x * 1e4f)),
                           int(std::lround(p.y * 1e4f)),
                           int(std::lround(p.z * 1e4f))};
            };

            std::unordered_map<Key, std::vector<Vec3f>, Hash> acc;
            for (const Vertex& v : m_cpuVerts) acc[key(v.pos)].push_back(v.normal);

            /*
                Averaging everything at a position is wrong for this geometry:
                a thin shell puts the inside and outside surface on the same
                vertex with opposing normals, and summing them cancels out -
                measured on DamagedHelmet, the worst case ended up pointing
                0.73 the other way.

                So only normals already within SMOOTH_ANGLE of this one are
                averaged in. A hard edge keeps its split and a curved surface
                still welds.
            */
            constexpr float SMOOTH_ANGLE = 0.5f;        // cos 60 degrees

            m_smoothNormals.resize(m_cpuVerts.size());
            for (size_t i = 0; i < m_cpuVerts.size(); ++i) {
                const Vec3f own = m_cpuVerts[i].normal;

                Vec3f sum = {0.f, 0.f, 0.f};
                for (const Vec3f& n : acc[key(m_cpuVerts[i].pos)])
                    if (n.dot(own) > SMOOTH_ANGLE) sum += n;

                m_smoothNormals[i] = sum.length() > 1e-6f ? sum.normalized() : own;
            }
        }
        m_program  = m_valo->createProgram("examples/shaders/scobject.vert.hpp", "examples/shaders/scobject.frag.hpp");

        for (const GLTFMaterial& src : gltf.materials) {
            m_materials.push_back(Material{
                .baseColorTex           = loadTexture(gltf, src.baseColorTex),
                .metallicRoughnessTex   = loadTexture(gltf, src.metallicRoughnessTex),
                .normalTex              = loadTexture(gltf, src.normalTex),
                .occlusionTex           = loadTexture(gltf, src.occlusionTex),
                .emissiveTex            = loadTexture(gltf, src.emissiveTex),
                .baseColor              = src.baseColor,
                .emissive               = src.emissive,
                .metallic               = src.metallic,
                .roughness              = src.roughness,
            });
        }

        /*
            The node graph is flattened once here instead of every frame. Each
            primitive also gets a mesh-space centroid, which is what the blended
            pass sorts on - glass needs to arrive back to front or it composites
            in the wrong order.
        */
        for (const GLTFNode& node : gltf.nodes) {
            if (node.mesh < 0 || size_t(node.mesh) >= gltf.meshes.size()) continue;

            for (const GLTFPrimitive& prim : gltf.meshes[node.mesh].primitives) {
                Vec3f centroid = {0.f, 0.f, 0.f};
                const uint32_t last = std::min<uint32_t>(prim.firstIndex + prim.indexCount,
                                                         uint32_t(gltf.indices.size()));
                uint32_t counted = 0;
                for (uint32_t i = prim.firstIndex; i < last; ++i) {
                    const Vec3f& p = gltf.vertices[gltf.indices[i]].pos;
                    centroid += p;
                    ++counted;
                    trackBounds((node.world * Vec4f{p, 1.f}).xyz);
                }
                if (counted) centroid = centroid * (1.f / float(counted));

                const bool blended = materialFor(prim.material).baseColor.w < 0.995f;
                m_items.push_back(Item{node.world, centroid, prim.firstIndex, prim.indexCount,
                                       prim.material, blended});
            }
        }

        return m_loaded = true;
    }

    // pass = the world matrix the whole model is drawn through (identity for the
    // real draw, the mirror for the reflection)
    void draw(const Mat4f& viewProj, const Mat4f& pass, const Vec3f& camPos,
              const float handedness, const float dim, const float emissiveBoost,
              const bool blendedPass, const CullMode cull) {
        if (!m_valo || !m_loaded) return;

        const Mat4f root = pass * m_transform;

        if (blendedPass) {
            m_sorted.clear();
            for (size_t i = 0; i < m_items.size(); ++i) {
                if (!m_items[i].blended) continue;
                const Vec3f world = (root * m_items[i].world * Vec4f{m_items[i].centroid, 1.f}).xyz;
                m_sorted.push_back({(world - camPos).length(), i});
            }
            std::sort(m_sorted.begin(), m_sorted.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            for (const auto& [_, index] : m_sorted)
                submit(m_items[index], viewProj, root,
                       handedness, dim, emissiveBoost, true, cull);
            return;
        }

        for (const Item& item : m_items)
            if (!item.blended)
                submit(item, viewProj, root,
                       handedness, dim, emissiveBoost, false, cull);
    }

    /*
        Every primitive, baked through its node transform and the model's own
        fit, so the tracer sees one flat world-space soup. Done once when the
        BVH is built rather than per frame - nothing here animates.
    */
    void appendTriangles(std::vector<TraceTri>& out, std::vector<TraceMaterial>& mats) const {
        if (!m_loaded) return;

        const uint32_t materialBase = uint32_t(mats.size());
        for (const Material& m : m_materials) {
            mats.push_back(TraceMaterial{
                .baseColor = m.baseColor,
                .emissive  = m.emissive,
                .metallic  = m.metallic,
                .roughness = m.roughness,

                .baseColorTex         = m.baseColorTex,
                .metallicRoughnessTex = m.metallicRoughnessTex,
                .normalTex            = m.normalTex,
                .occlusionTex         = m.occlusionTex,
                .emissiveTex          = m.emissiveTex,
            });
        }

        // a primitive with no material still needs one to index
        mats.push_back(TraceMaterial{
            .baseColor = {0.8f, 0.8f, 0.8f, 1.f},
            .emissive  = {0.f, 0.f, 0.f},
            .metallic  = 0.f,
            .roughness = 0.6f,

            .baseColorTex         = EMPTY_TEXTURE,
            .metallicRoughnessTex = EMPTY_TEXTURE,
            .normalTex            = EMPTY_TEXTURE,
            .occlusionTex         = EMPTY_TEXTURE,
            .emissiveTex          = EMPTY_TEXTURE,
        });
        const uint32_t fallback = uint32_t(mats.size()) - 1u;

        for (const Item& item : m_items) {
            const Mat4f world  = m_transform * item.world;
            const Mat4f normal = world;          // no non-uniform scale in the fit

            const uint32_t mat = item.material >= 0
                ? materialBase + uint32_t(item.material)
                : fallback;

            for (uint32_t i = 0; i + 2 < item.indexCount; i += 3) {
                const uint32_t i0 = m_cpuIndices[item.firstIndex + i];
                const uint32_t i1 = m_cpuIndices[item.firstIndex + i + 1];
                const uint32_t i2 = m_cpuIndices[item.firstIndex + i + 2];

                auto place  = [&](const Vec3f& v) { return (world  * Vec4f(v, 1.f)).xyz; };
                auto rotate = [&](const Vec3f& v) { return (normal * Vec4f(v, 0.f)).xyz.normalized(); };

                out.push_back(TraceTri{
                    .p0 = place(m_cpuVerts[i0].pos), .material = mat,
                    .p1 = place(m_cpuVerts[i1].pos),
                    .p2 = place(m_cpuVerts[i2].pos),
                    .n0 = rotate(m_smoothNormals[i0]),
                    .n1 = rotate(m_smoothNormals[i1]),
                    .n2 = rotate(m_smoothNormals[i2]),

                    .t0 = Vec4f(rotate(m_cpuVerts[i0].tangent.xyz), m_cpuVerts[i0].tangent.w),
                    .t1 = Vec4f(rotate(m_cpuVerts[i1].tangent.xyz), m_cpuVerts[i1].tangent.w),
                    .t2 = Vec4f(rotate(m_cpuVerts[i2].tangent.xyz), m_cpuVerts[i2].tangent.w),

                    .uv0 = m_cpuVerts[i0].uv,
                    .uv1 = m_cpuVerts[i1].uv,
                    .uv2 = m_cpuVerts[i2].uv,
                });
            }
        }
    }

private:
    struct Item {
        Mat4f    world;
        Vec3f    centroid;
        uint32_t firstIndex, indexCount;
        int      material;
        bool     blended;
    };

    void submit(const Item& item, const Mat4f& viewProj, const Mat4f& root, const float handedness,
                const float dim, const float emissiveBoost, const bool blended, const CullMode cull) {
        const Mat4f model = root * item.world;

        Uniforms u{};
        u.mvp           = viewProj * model;
        u.model         = model;
        u.material      = materialFor(item.material);
        u.handedness    = handedness;
        u.dim           = dim;
        u.emissiveBoost = emissiveBoost;

        RenderCommand cmd;
        cmd.verts      = m_vertices;
        cmd.indices    = m_indices;
        cmd.program    = m_program;
        cmd.firstIndex = item.firstIndex;
        cmd.indexCount = item.indexCount;
        cmd.cullmode   = cull;
        cmd.blendmode  = blended ? BlendMode::Alpha : BlendMode::Opaque;
        cmd.depthWrite = !blended;
        cmd.setUniforms(u);
        cmd.bindUniformBuffer(SCENE_SLOT, m_sceneBuffer);
        m_valo->submit(cmd);
    }

    /*
        glTF only ships tangents when the exporter felt like it, and this model
        does not, so they are derived from the UV parameterisation: per triangle
        the tangent is the direction in which u increases, accumulated onto each
        vertex and orthonormalised against the normal at the end.
    */
    // the parser already supplies tangents - the file's own, or computed from
    // the UVs when it has none - so this only reorders into this shader's Vertex
    static std::vector<Vertex> buildTangents(const GLTF& gltf) {
        std::vector<Vertex> out(gltf.vertices.size());
        for (size_t i = 0; i < out.size(); ++i) {
            const GLTFVertex& v = gltf.vertices[i];
            out[i] = Vertex{v.pos, v.normal, v.tangent, v.uv};
        }
        return out;
    }

    /*
        Linear on purpose: scobject.frag decodes its colour maps with
        srgbToLinear itself. A program that leaves the decode to the sampler
        loads colour slots with srgb = true instead - loadModel() does.
    */
    TextureID loadTexture(const GLTF& gltf, const int image) {
        return loadGLTFTexture(*m_valo, gltf, image, false);
    }

    const Material& materialFor(const int index) const {
        if (index < 0 || size_t(index) >= m_materials.size()) return m_fallback;
        return m_materials[index];
    }

    void trackBounds(const Vec3f& p) {
        m_boundsMin = {std::min(m_boundsMin.x, p.x), std::min(m_boundsMin.y, p.y), std::min(m_boundsMin.z, p.z)};
        m_boundsMax = {std::max(m_boundsMax.x, p.x), std::max(m_boundsMax.y, p.y), std::max(m_boundsMax.z, p.z)};
    }

    VertexBufferID m_vertices = EMPTY_BUFFER;
    IndexBufferID  m_indices  = EMPTY_BUFFER;
    ProgramID      m_program  = DEFAULT_PROGRAM;

    std::vector<Material> m_materials;
    std::vector<Item>     m_items;
    std::vector<std::pair<float, size_t>> m_sorted;   // kept alive so sorting does not allocate per frame

    Material m_fallback;
    Mat4f    m_transform;

    Vec3f m_boundsMin = { 1e30f,  1e30f,  1e30f};
    Vec3f m_boundsMax = {-1e30f, -1e30f, -1e30f};

    // the raster path uploads and forgets; the tracer needs these on the CPU
    std::vector<Vertex>   m_cpuVerts;
    std::vector<uint32_t> m_cpuIndices;
    std::vector<Vec3f>    m_smoothNormals;

    VALO* m_valo = nullptr;
    UniformBufferID m_sceneBuffer = EMPTY_BUFFER;
    bool  m_loaded = false;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
class Engine {
public:
    Engine() = default;

    bool init(
        const std::string& title = "VALO - PBR Showcase",
        const Vec2u& size = {1280, 720},
        const Backend backend = Backend::Software,
        const bool validation = false
    ) {
        /*
            Every pixel of this runs on the CPU, and the frame is fragment bound
            - measured on a 16 core box at ~1MP it is 9 fps at full resolution
            and 16 at 65%, for a picture that is hard to tell apart once it is
            moving. Keys 1-4 override it; 1 is the beauty shot.
        */
        RenderConfig cfg;
        cfg.vsync       = true;
        cfg.renderScale = 0.65f;
        cfg.backend     = backend;
        cfg.validation  = validation;   // Vulkan only, and costs ~4.6x the frame rate

        // SDL has to be told at creation; a surface cannot be attached later
        WinFlags flags = WinFlags::Resize | WinFlags::HiDPI;
        if (backend == Backend::Vulkan) flags = flags | WinFlags::Vulkan;

        if (!m_window.create(title, size, flags) || !m_valo.init(m_window, cfg)) return false;
        m_running = true;

        // created empty; render() refills it every frame before the first submit
        const SceneUniforms scene{};
        m_sceneBuffer = m_valo.createUniformBuffer(&scene, sizeof(scene));

        buildSky();
        buildFloor();
        buildOrb();
        resetCamera();
        return true;
    }

    // targetSize is the length of the fitted model's longest axis, so every
    // model arrives at roughly the same size however its file was authored
    Model& addModel(const std::string& pathToGltf, const float targetSize = 4.f) {
        m_models.emplace_back();
        Model& model = m_models.back();
        model.setVALO(&m_valo);
        model.setSceneBuffer(m_sceneBuffer);
        model.loadGLTF(pathToGltf);

        if (model.isLoaded()) {
            model.fitToGround(targetSize, FLOOR_Y);
            frameModels();
        }
        return model;
    }

    // the camera framing, the light rig and the contact shadow all key off the
    // fitted size, so they follow whatever the model turned out to be
    void frameModels() {
        Vec3f lo, hi;
        if (!worldBounds(lo, hi)) return;

        m_focus       = (lo + hi) * 0.5f;
        m_focus.y     = lo.y + (hi.y - lo.y) * 0.42f;
        m_orbitRadius = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * 1.55f;
        resetCamera();
    }

    /*
        Flattens every model into one world-space triangle soup, builds a BVH
        over it and uploads both. Called once after the models load - nothing in
        this scene animates, so rebuilding per frame would buy nothing.
    */
    void buildTraceScene() {
        std::vector<TraceTri>      tris;
        std::vector<TraceMaterial> mats;

        for (const Model& m : m_models) m.appendTriangles(tris, mats);

        if (tris.empty()) {
            std::fprintf(stderr, "showcase: nothing to trace\n");
            return;
        }

        // the build reorders tris, so the BVH has to be uploaded from the
        // same array afterwards rather than from a copy taken before
        m_bvh.build(tris);

        m_traceNodes = m_valo.createStorageBuffer(
            m_bvh.nodes().data(), m_bvh.nodes().size() * sizeof(BVHNode));
        m_traceTris = m_valo.createStorageBuffer(
            tris.data(), tris.size() * sizeof(TraceTri));
        m_traceMats = m_valo.createStorageBuffer(
            mats.data(), mats.size() * sizeof(TraceMaterial));

        // built from the reordered array, so index i still means the same
        // triangle in both buffers
        std::vector<TriPos> pos(tris.size());
        for (size_t i = 0; i < tris.size(); ++i)
            pos[i] = { tris[i].p0, tris[i].p1 - tris[i].p0, tris[i].p2 - tris[i].p0,
                       std::clamp(mats[tris[i].material].baseColor.w, 0.f, 1.f) };

        m_tracePos = m_valo.createStorageBuffer(pos.data(), pos.size() * sizeof(TriPos));

        m_traceProgram = m_valo.createProgram(
            "examples/shaders/trace.vert.hpp", "examples/shaders/trace.frag.hpp");

        TraceVertex quad[4] = {{{-1,-1}}, {{1,-1}}, {{1,1}}, {{-1,1}}};
        uint32_t    idx[6]  = {0, 1, 2, 0, 2, 3};

        m_traceVerts   = m_valo.createVertexBuffer(quad, 4, VtxLayout({VtxAttrib::Vec2f}));
        m_traceIndices = m_valo.createIndexBuffer(idx, 6);

        std::printf("traced scene: %zu triangles, %zu BVH nodes, %zu materials\n",
                    tris.size(), m_bvh.nodes().size(), mats.size());
    }

    void update() {
        m_running = m_window.pollEvents();
        if (!m_running) return;

        const float dt = float(m_valo.getDeltaTime());
        m_time += dt;

        if (m_window.wasKeyPressed(Key::Escape)) { m_window.close(); m_running = false; return; }
        if (m_window.wasKeyPressed(Key::R))      { m_reflections = !m_reflections; }
        if (m_window.wasKeyPressed(Key::L))      { m_lightsMove  = !m_lightsMove; }
        if (m_window.wasKeyPressed(Key::F))      { m_showFloor   = !m_showFloor; }
        if (m_window.wasKeyPressed(Key::O))      { m_showOrbs    = !m_showOrbs; }
        if (m_window.wasKeyPressed(Key::Num1))   { m_valo.setRenderScale(1.00f); }
        if (m_window.wasKeyPressed(Key::Num2))   { m_valo.setRenderScale(0.75f); }
        if (m_window.wasKeyPressed(Key::Num3))   { m_valo.setRenderScale(0.50f); }
        if (m_window.wasKeyPressed(Key::Num4))   { m_valo.setRenderScale(0.25f); }
        if (m_window.wasKeyPressed(Key::Num5))   { m_valo.setRenderScale(2.00f); }
        if (m_window.wasKeyPressed(Key::Num6))   { m_valo.setRenderScale(3.00f); }
        if (m_window.wasKeyPressed(Key::Num4))   { m_valo.setRenderScale(4.00f); }
        if (m_window.wasKeyPressed(Key::Tab))    { resetCamera(); m_autoOrbit = true; }
        if (m_window.wasKeyPressed(Key::V))      { m_valo.setVsync(!m_valo.getVsync()); }

        m_exposure = clampf(m_exposure + m_window.scrollDelta() * 0.06f, 0.15f, 6.f);
        if (m_lightsMove) m_lightPhase += dt * 0.35f;
        animateLights();

        const float moveSpeed = (m_window.isKeyDown(Key::LCtrl) ? 9.f : 3.f) * dt;
        Vec3f movement = {0.f, 0.f, 0.f};
        if (m_window.isKeyDown(Key::W))      movement.z =  moveSpeed;
        if (m_window.isKeyDown(Key::S))      movement.z = -moveSpeed;
        if (m_window.isKeyDown(Key::A))      movement.x = -moveSpeed;
        if (m_window.isKeyDown(Key::D))      movement.x =  moveSpeed;
        if (m_window.isKeyDown(Key::LShift)) movement.y = -moveSpeed;
        if (m_window.isKeyDown(Key::Space))  movement.y =  moveSpeed;

        bool justGrabbed = false;
        if (m_window.wasMousePressed(MouseButton::Left)) {
            if (m_window.isMouseGrabbed()) m_window.releaseMouse();
            else { m_window.grabMouse(); justGrabbed = true; m_autoOrbit = false; }
        }
        if (m_window.wasMousePressed(MouseButton::Right)) m_window.releaseMouse();

        if (m_window.isMouseGrabbed()) {
            // the delta is read after pollEvents, and dropped on the frame the
            // grab starts: that frame's motion is the trip to the click, not a look
            m_camera.update(justGrabbed ? Vec2f{0.f, 0.f} : m_window.mouseDelta(), movement);
        } else if (m_autoOrbit) {
            orbitCamera(dt);
        } else {
            m_camera.update({0.f, 0.f}, movement);
        }

        updateTitle(dt);
    }

    void render() {
        if (!m_running) return;
        if (m_raytrace && m_traceProgram != DEFAULT_PROGRAM) { renderTraced(); return; }

        const Mat4f view = m_camera.view();
        const Mat4f proj = Mat4f::perspective(m_camera.fov, m_window.getAspect(),
                                              m_camera.nearPlane, m_camera.farPlane);
        const Mat4f viewProj = proj * view;
        const bool  aboveFloor = m_camera.pos.y > FLOOR_Y + 0.02f;

        // one upload for the whole frame; every command below binds it
        SceneUniforms scene{};
        for (int i = 0; i < LIGHT_COUNT; ++i) scene.lights[i] = m_lights[i];
        scene.camPos   = m_camera.pos;
        scene.exposure = m_exposure;
        m_valo.updateUniformBuffer(m_sceneBuffer, &scene, sizeof(scene));

        m_valo.beginFrame();

        drawSky();

        // 2 - the model again, mirrored through the floor. Mirroring reverses
        //     the winding, so the cull flips with it.
        if (m_reflections && m_showFloor && aboveFloor) {
            const Mat4f mirror = Mat4f::translation({0.f, 2.f * FLOOR_Y, 0.f}) * Mat4f::scale({1.f, -1.f, 1.f});
            for (Model& m : m_models) {
                m.draw(viewProj, mirror, m_camera.pos, -1.f, 0.78f, m_emissiveBoost, false, CullMode::Front);
                m.draw(viewProj, mirror, m_camera.pos, -1.f, 0.78f, m_emissiveBoost, true,  CullMode::Front);
            }
        }

        if (m_showFloor) drawFloor(viewProj);

        for (Model& m : m_models)
            m.draw(viewProj, Mat4f::identity(), m_camera.pos, 1.f, 1.f, m_emissiveBoost, false, CullMode::Back);
        for (Model& m : m_models)
            m.draw(viewProj, Mat4f::identity(), m_camera.pos, 1.f, 1.f, m_emissiveBoost, true,  CullMode::Back);

        if (m_showOrbs) drawOrbs(viewProj);

        m_valo.endFrame();
    }

    /*
        One fullscreen quad; every pixel builds its own ray. Nothing is
        rasterised, so the sky, floor, reflection and shading all come out of
        the fragment shader against the BVH.
    */
    void renderTraced() {
        TraceUniforms u{};

        u.camPos   = m_camera.pos;
        u.camFwd   = m_camera.forward;
        u.camRight = m_camera.right;
        u.camUp    = m_camera.up;
        u.tanHalf  = std::tan(m_camera.fov * 0.5f);
        u.aspect   = m_window.getAspect();
        u.exposure = m_exposure;
        u.floorY   = FLOOR_Y;

        u.lightCount  = uint32_t(LIGHT_COUNT);
        u.showFloor   = m_showFloor   ? 1u : 0u;
        u.reflections = m_reflections ? 1u : 0u;

        for (int i = 0; i < LIGHT_COUNT; ++i) {
            u.lights[i].pos       = m_lights[i].pos;
            u.lights[i].colour    = m_lights[i].colour;
            u.lights[i].intensity = m_lights[i].intensity;
        }

        RenderCommand cmd;
        cmd.verts      = m_traceVerts;
        cmd.indices    = m_traceIndices;
        cmd.program    = m_traceProgram;
        cmd.cullmode   = CullMode::None;
        cmd.depthTest  = false;
        cmd.depthWrite = false;
        cmd.setUniforms(u);

        cmd.bindStorageBuffer(0, m_traceNodes);
        cmd.bindStorageBuffer(1, m_traceTris);
        cmd.bindStorageBuffer(2, m_traceMats);
        cmd.bindStorageBuffer(3, m_tracePos);

        m_valo.beginFrame();
        m_valo.submit(cmd);
        m_valo.endFrame();
    }

    bool isRunning() const { return m_running; }
    VALO&   getVALO()   { return m_valo; }
    Window& getWindow() { return m_window; }
    Camera& getCamera() { return m_camera; }

private:
    // -----------------------------------------------------------------------
    // the union of every loaded model's fitted bounds; false if nothing loaded
    bool worldBounds(Vec3f& lo, Vec3f& hi) const {
        bool any = false;
        lo = { 1e30f,  1e30f,  1e30f};
        hi = {-1e30f, -1e30f, -1e30f};

        for (const Model& m : m_models) {
            if (!m.isLoaded()) continue;
            Vec3f mlo, mhi;
            m.worldBounds(mlo, mhi);
            lo = {std::min(lo.x, mlo.x), std::min(lo.y, mlo.y), std::min(lo.z, mlo.z)};
            hi = {std::max(hi.x, mhi.x), std::max(hi.y, mhi.y), std::max(hi.z, mhi.z)};
            any = true;
        }
        return any;
    }

    void resetCamera() {
        m_orbitAngle  = 0.72f;
        m_camera.pos  = m_focus + Vec3f{std::sin(m_orbitAngle) * m_orbitRadius,
                                        m_orbitRadius * 0.34f,
                                        std::cos(m_orbitAngle) * m_orbitRadius};
        m_camera.lookAtPoint(m_focus);
    }

    void orbitCamera(const float dt) {
        m_orbitAngle += dt * 0.12f;
        m_camera.pos = m_focus + Vec3f{std::sin(m_orbitAngle) * m_orbitRadius,
                                       m_orbitRadius * (0.30f + 0.06f * std::sin(m_orbitAngle * 0.7f)),
                                       std::cos(m_orbitAngle) * m_orbitRadius};
        m_camera.lookAtPoint(m_focus);
    }

    /*
        A three point rig plus one accent: a warm key that swings across the
        front, a cool fill opposite it, a low warm bounce that picks out the
        sills, and a magenta rim behind the model to separate it from the sky.
    */
    void animateLights() {
        const float a = m_lightPhase;
        const float r = std::max(m_orbitRadius, 3.f);

        m_lights[0] = {{std::sin(a) * r * 0.85f, r * 0.75f, std::cos(a) * r * 0.85f},
                       {1.00f, 0.93f, 0.82f}, 46.f * r};
        m_lights[1] = {{std::sin(a + PI) * r * 0.9f, r * 0.42f, std::cos(a + PI) * r * 0.9f},
                       {0.42f, 0.62f, 1.00f}, 26.f * r};
        m_lights[2] = {{std::sin(a * 0.6f + 1.9f) * r * 0.7f, r * 0.13f, std::cos(a * 0.6f + 1.9f) * r * 0.7f},
                       {1.00f, 0.44f, 0.16f}, 15.f * r};
        m_lights[3] = {{std::sin(-a * 0.8f + 3.6f) * r * 0.8f, r * 0.55f, std::cos(-a * 0.8f + 3.6f) * r * 0.8f},
                       {0.85f, 0.25f, 0.95f}, 20.f * r};

        for (Light& l : m_lights) l.pos += Vec3f{m_focus.x, 0.f, m_focus.z};
    }

    // -----------------------------------------------------------------------
    void buildSky() {
        const SkyVertex quad[4] = {{{-1.f, -1.f}}, {{1.f, -1.f}}, {{1.f, 1.f}}, {{-1.f, 1.f}}};
        const uint32_t  idx[6]  = {0, 1, 2, 0, 2, 3};

        m_sky.verts      = m_valo.createVertexBuffer(quad, 4, VtxLayout({VtxAttrib::Vec2f}));
        m_sky.indices    = m_valo.createIndexBuffer(idx, 6);
        m_sky.program    = m_valo.createProgram("examples/shaders/scsky.vert.hpp", "examples/shaders/scsky.frag.hpp");
        m_sky.depthTest  = false;
        m_sky.depthWrite = false;
        m_sky.cullmode   = CullMode::None;
        m_sky.bindUniformBuffer(SCENE_SLOT, m_sceneBuffer);
    }

    void buildFloor() {
        const float e = 130.f;   // comfortably beyond fadeEnd; the shader discards past it anyway
        const FloorVertex quad[4] = {{{-e, FLOOR_Y, -e}}, {{e, FLOOR_Y, -e}}, {{e, FLOOR_Y, e}}, {{-e, FLOOR_Y, e}}};
        const uint32_t    idx[6]  = {0, 2, 1, 0, 3, 2};

        m_floor.verts      = m_valo.createVertexBuffer(quad, 4, VtxLayout({VtxAttrib::Vec3f}));
        m_floor.indices    = m_valo.createIndexBuffer(idx, 6);
        m_floor.program    = m_valo.createProgram("examples/shaders/scfloor.vert.hpp", "examples/shaders/scfloor.frag.hpp");
        m_floor.cullmode   = CullMode::None;
        m_floor.blendmode  = BlendMode::Alpha;
        m_floor.depthWrite = true;
        m_floor.bindUniformBuffer(SCENE_SLOT, m_sceneBuffer);
    }

    void buildOrb() {
        constexpr int stacks = 14, slices = 20;
        std::vector<OrbVertex> verts;
        std::vector<uint32_t>  idx;

        for (int i = 0; i <= stacks; ++i) {
            const float phi = float(i) / stacks * PI;
            for (int j = 0; j <= slices; ++j) {
                const float theta = float(j) / slices * 2.f * PI;
                const Vec3f unit = {std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
                verts.push_back({unit, unit});
            }
        }
        const uint32_t stride = slices + 1;
        for (int i = 0; i < stacks; ++i) for (int j = 0; j < slices; ++j) {
            const uint32_t tl = uint32_t(i) * stride + uint32_t(j), tr = tl + 1, bl = tl + stride, br = bl + 1;
            idx.insert(idx.end(), {tl, br, bl, tl, tr, br});
        }

        m_orb.verts      = m_valo.createVertexBuffer(verts.data(), verts.size(),
                                                     VtxLayout({VtxAttrib::Vec3f, VtxAttrib::Vec3f}));
        m_orb.indices    = m_valo.createIndexBuffer(idx.data(), idx.size());
        m_orb.program    = m_valo.createProgram("examples/shaders/scorb.vert.hpp", "examples/shaders/scorb.frag.hpp");
        m_orb.blendmode  = BlendMode::Additive;
        m_orb.depthWrite = false;
        m_orb.cullmode   = CullMode::None;
        m_orb.bindUniformBuffer(SCENE_SLOT, m_sceneBuffer);
    }

    // -----------------------------------------------------------------------
    void drawSky() {
        SkyUniforms u{};
        u.right      = m_camera.right;
        u.up         = m_camera.up;
        u.forward    = m_camera.forward;
        u.tanHalfFov = std::tan(m_camera.fov * 0.5f);
        u.aspect     = m_window.getAspect();

        m_sky.setUniforms(u);
        m_valo.submit(m_sky);
    }

    void drawFloor(const Mat4f& viewProj) {
        FloorUniforms u{};
        u.viewProj     = viewProj;
        Vec3f lo, hi;
        if (!worldBounds(lo, hi)) { lo = {-1.f, 0.f, -1.f}; hi = {1.f, 1.f, 1.f}; }
        u.shadowCentre = {(lo.x + hi.x) * 0.5f, FLOOR_Y, (lo.z + hi.z) * 0.5f};
        u.shadowRadius = {std::max((hi.x - lo.x) * 0.62f, 0.2f),
                          std::max((hi.z - lo.z) * 0.62f, 0.2f)};
        u.fadeStart    = 18.f;
        u.fadeEnd      = 62.f;
        u.gridSpacing  = 1.f;

        m_floor.setUniforms(u);
        m_valo.submit(m_floor);
    }

    void drawOrbs(const Mat4f& viewProj) {
        for (const Light& l : m_lights) {
            if (l.intensity <= 0.f) continue;

            // core then halo: the halo is a bigger, dimmer shell added on top,
            // which is as close to a bloom as a single pass gets
            constexpr float scale = 0.055f;
            for (int pass = 0; pass < 2; ++pass) {
                OrbUniforms u{};
                u.viewProj   = viewProj;
                u.centre     = l.pos;
                u.colour     = l.colour;
                u.radius     = pass == 0 ? scale : scale * 3.4f;
                u.brightness = pass == 0 ? 34.f  : 2.2f;

                m_orb.setUniforms(u);
                m_valo.submit(m_orb);
            }
        }
    }

    /*
        Measured off a steady_clock rather than getDeltaTime(), which clamps at
        0.1s so a heavy frame cannot run the animation away - feed that to a
        counter and it reads a flat 10 fps for anything slower than 10 fps.
    */
    void updateTitle(const float) {
        const auto now = std::chrono::steady_clock::now();
        m_fpsAccum += std::chrono::duration<float>(now - m_fpsMark).count();
        m_fpsMark = now;
        ++m_fpsFrames;
        if (m_fpsAccum < 0.5f) return;

        const int fps = int(float(m_fpsFrames) / m_fpsAccum + 0.5f);
        m_window.setTitle("VALO - PBR Showcase   |   " + std::to_string(fps) + " fps   |   "
                        + "exposure " + std::to_string(m_exposure).substr(0, 4)
                        + (m_reflections ? "   |   reflections on" : "   |   reflections off"));
        m_fpsAccum = 0.f;
        m_fpsFrames = 0;
    }

    // Window first: members destruct in reverse, and the Vulkan surface
    // cannot be destroyed after the SDL window it came from.
    Window m_window;
    VALO   m_valo;
    Camera m_camera;

    std::deque<Model> m_models;
    RenderCommand m_sky, m_floor, m_orb;
    Light m_lights[LIGHT_COUNT];
    UniformBufferID m_sceneBuffer = EMPTY_BUFFER;

    Vec3f m_focus       = {0.f, 0.8f, 0.f};
    float m_orbitRadius = 7.f;
    float m_orbitAngle  = 0.72f;

    float m_time = 0.f, m_lightPhase = 0.6f;
    float m_exposure = 1.f, m_emissiveBoost = 2.6f;
    float m_fpsAccum = 0.f;
    uint32_t m_fpsFrames = 0;
    std::chrono::steady_clock::time_point m_fpsMark = std::chrono::steady_clock::now();

    bool m_running     = false;

    BVH             m_bvh;
    bool            m_raytrace      = true;
    ProgramID       m_traceProgram  = DEFAULT_PROGRAM;
    StorageBufferID m_traceNodes    = EMPTY_BUFFER;
    StorageBufferID m_traceTris     = EMPTY_BUFFER;
    StorageBufferID m_traceMats     = EMPTY_BUFFER;
    StorageBufferID m_tracePos      = EMPTY_BUFFER;
    VertexBufferID  m_traceVerts    = EMPTY_BUFFER;
    IndexBufferID   m_traceIndices  = EMPTY_BUFFER;
    bool m_reflections = true;
    bool m_showFloor   = true;
    bool m_showOrbs    = true;
    bool m_lightsMove  = true;
    bool m_autoOrbit   = true;
};

}
