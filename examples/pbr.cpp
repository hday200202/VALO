// PBR material viewer - a port of cool-pbr-test to VALO.
//
// Cook-Torrance metallic/roughness over a table of 62 materials, value-noise
// fbm bump mapping (sampled in object space so it sticks to the surface),
// optional vertex displacement, four view-space lights and a sky/sun
// environment used both as the background and the reflection term.
//
// The original carried its own rasterizer, background buffer, SDL window and
// event loop; VALO replaces all of that, so what's left is materials, shaders
// and mesh builders.
//
// Controls
//   left click    grab mouse and drag to rotate; right click releases
//   c             cube / sphere
//   [ ] / wheel   previous / next material
//   up down       bump strength        (hold)
//   left right    noise scale          (hold)
//   pgup pgdn     roughness variation  (hold)
//   d             procedural detail on / off
//   g             displace geometry along the height field
//   1 2 3 4       render scale 100% / 75% / 50% / 25%
//   r             reset orientation and parameters
//   escape        quit

#include <VALO.hpp>
#include <cstring>
#include <cmath>
#include <numbers>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

#include "shaders/pbr.vert.hpp"
#include "shaders/pbr.frag.hpp"
#include "shaders/sky.vert.hpp"
#include "shaders/sky.frag.hpp"

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Mat4f;
using valo::Color;

// ---------------------------------------------------------------------------
// meshes
// ---------------------------------------------------------------------------
struct Mesh { std::vector<Vertex> verts; std::vector<uint32_t> idx; };

/*
    Each face is a seg x seg grid rather than two triangles, so the displacement
    in the vertex shader has vertices to actually move. Every vertex on a face
    carries that face's flat normal, which is what keeps the cube's edges sharp -
    a shared, averaged normal would round them off.
*/
static Mesh buildCube(float size, int seg) {
    const float h = size * 0.5f;

    // each face as an origin corner plus two edge directions, so one loop builds
    // all six without six special cases
    struct Face { Vec3f origin, right, up, normal; };

    const Face faces[6] = {
        { {-h, -h,  h}, { 1, 0,  0}, {0, 1,  0}, { 0,  0,  1} },   // +z
        { { h, -h, -h}, {-1, 0,  0}, {0, 1,  0}, { 0,  0, -1} },   // -z
        { {-h, -h, -h}, { 0, 0,  1}, {0, 1,  0}, {-1,  0,  0} },   // -x
        { { h, -h,  h}, { 0, 0, -1}, {0, 1,  0}, { 1,  0,  0} },   // +x
        { {-h,  h,  h}, { 1, 0,  0}, {0, 0, -1}, { 0,  1,  0} },   // +y
        { {-h, -h, -h}, { 1, 0,  0}, {0, 0,  1}, { 0, -1,  0} },   // -y
    };

    Mesh m;
    const int stride = seg + 1;      // vertices per row, not quads

    for (const Face& f : faces) {
        const uint32_t base = uint32_t(m.verts.size());

        for (int j = 0; j <= seg; ++j) {
            for (int i = 0; i <= seg; ++i) {
                const float u = float(i) / seg;
                const float v = float(j) / seg;

                m.verts.push_back({
                    f.origin + f.right * (u * size) + f.up * (v * size),
                    f.normal,
                    {u, v},
                });
            }
        }

        for (int j = 0; j < seg; ++j) {
            for (int i = 0; i < seg; ++i) {
                const uint32_t v00 = base + j * stride + i;
                const uint32_t v10 = v00 + 1;
                const uint32_t v01 = v00 + stride;
                const uint32_t v11 = v01 + 1;

                m.idx.insert(m.idx.end(), {v00, v10, v11,  v00, v11, v01});
            }
        }
    }

    return m;
}

/*
    A UV sphere: stacks run pole to pole (phi), slices run around (theta). For a
    unit sphere the position and the normal are the same vector, which is why
    `unit` is used for both.

    The seam column is duplicated (j goes to <= slices) because the vertex at
    u = 0 and the one at u = 1 are in the same place but need different UVs.
*/
static Mesh buildSphere(float radius, int stacks, int slices) {
    Mesh m;

    for (int i = 0; i <= stacks; ++i) {
        const float v   = float(i) / stacks;
        const float phi = v * PI;                  // 0 at +y, pi at -y

        for (int j = 0; j <= slices; ++j) {
            const float u     = float(j) / slices;
            const float theta = u * 2.f * PI;

            const Vec3f unit = {
                std::sin(phi) * std::cos(theta),
                std::cos(phi),
                std::sin(phi) * std::sin(theta),
            };

            m.verts.push_back({unit * radius, unit, {u, v}});
        }
    }

    const uint32_t stride = slices + 1;

    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t tl = i * stride + j;
            const uint32_t tr = tl + 1;
            const uint32_t bl = tl + stride;
            const uint32_t br = bl + 1;

            m.idx.insert(m.idx.end(), {tl, br, bl,  tl, tr, br});
        }
    }

    return m;
}

// ---------------------------------------------------------------------------
// The shader's Material has no name field - a const char* has no GLSL
// type - so the labels live here, parallel to MATERIALS.
static const char* MATERIAL_NAMES[] = {
    "Gold", "Polished brass", "Weathered brass", "Bronze",
    "Copper", "Silver", "Platinum", "Aluminium",
    "Chrome", "Nickel", "Titanium", "Anodised titanium",
    "Cobalt", "Zinc", "Lead", "Damascus steel",
    "Brushed steel", "Gunmetal", "Cast iron", "Oxidised iron",
    "Rusted steel", "Verdigris copper", "Red plastic", "White plastic",
    "Black plastic", "Blue car paint", "Red car paint", "Matte vinyl",
    "Rubber", "Silicone", "Porcelain", "Glazed ceramic",
    "Unglazed ceramic", "Terracotta", "Obsidian", "Ice",
    "Pearl", "Wax", "Marble", "Granite",
    "Slate", "Sandstone", "Rough concrete", "Asphalt",
    "Chalk", "Jade", "Lapis lazuli", "Malachite",
    "Turquoise", "Amber", "Varnished wood", "Raw oak",
    "Ebony", "Cork", "Leather", "Bone",
    "Velvet", "Denim", "Canvas", "Charcoal",
    "Snow", "Moss",
};

int main(int argc, char** argv) {
    // one scene, either backend: the software path resolves the shader
    // paths to registered C++ callbacks, the Vulkan path translates them
    bool useVulkan = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--vulkan")) useVulkan = true;

    valo::Window window("VALO - PBR", {1280, 960},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));
    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = true;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;

    valo::VtxLayout layout({valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec2f});
    const Mesh cube   = buildCube(2.f, CUBE_SEGMENTS);
    const Mesh sphere = buildSphere(1.25f, SPHERE_STACKS, SPHERE_SLICES);
    const valo::VertexBufferID cubeVb = valo.createVertexBuffer(cube.verts.data(), cube.verts.size(), layout);
    const valo::IndexBufferID  cubeIb = valo.createIndexBuffer(cube.idx.data(), cube.idx.size());
    const valo::VertexBufferID sphVb  = valo.createVertexBuffer(sphere.verts.data(), sphere.verts.size(), layout);
    const valo::IndexBufferID  sphIb  = valo.createIndexBuffer(sphere.idx.data(), sphere.idx.size());

    SceneUniforms scene{};
    const valo::UniformBufferID sceneBuffer = valo.createUniformBuffer(&scene, sizeof(scene));

    valo::RenderCommand obj;
    obj.program = valo.createProgram("examples/shaders/pbr.vert.hpp", "examples/shaders/pbr.frag.hpp");
    obj.bindUniformBuffer(SCENE_SLOT, sceneBuffer);

    SkyVertex quad[4] = {{{-1.f, -1.f}}, {{1.f, -1.f}}, {{1.f, 1.f}}, {{-1.f, 1.f}}};
    uint32_t quadIdx[6] = {0, 1, 2, 0, 2, 3};
    valo::RenderCommand sky;
    sky.verts      = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    sky.indices    = valo.createIndexBuffer(quadIdx, 6);
    sky.program    = valo.createProgram("examples/shaders/sky.vert.hpp", "examples/shaders/sky.frag.hpp");
    sky.depthTest  = false;
    sky.depthWrite = false;
    sky.cullmode   = valo::CullMode::None;

    // fixed camera; the object rotates
    const Vec3f camPos = {0.f, 0.f, CAMERA_DIST};
    const Mat4f view = Mat4f::lookAt(camPos, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    const Mat4f viewInv = view.inverse();

    Mat4f orientation;
    int   materialIndex = 0;

    bool  useSphere = false;
    bool  detail    = true;
    bool  displace  = false;

    float bump       = DEFAULT_BUMP;
    float noiseScale = DEFAULT_NOISE;
    float variation  = DEFAULT_VARIATION;

    using clk = std::chrono::steady_clock;
    auto     last      = clk::now();
    auto     fpsStart  = last;
    uint32_t fpsFrames = 0;
    int      fps       = 0;

    while (window.pollEvents()) {
        // clamped so one slow frame cannot make a held key jump the value
        const auto  now = clk::now();
        const float dt  = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        /* --- input: view --- */
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasMousePressed(valo::MouseButton::Left))  window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();
        if (window.isMouseGrabbed()) {
            // pre-multiplied, so each drag rotates about the SCREEN axes rather
            // than the object's own - which is what dragging a thing feels like
            const Vec2f d = window.mouseDelta();
            orientation = Mat4f::rotationX(d.y * MOUSE_SPEED)
                        * Mat4f::rotationY(d.x * MOUSE_SPEED)
                        * orientation;
        }

        /* --- input: what to draw --- */
        if (window.wasKeyPressed(valo::Key::C)) useSphere = !useSphere;
        if (window.wasKeyPressed(valo::Key::D)) detail    = !detail;
        if (window.wasKeyPressed(valo::Key::G)) displace  = !displace;
        if (window.wasKeyPressed(valo::Key::V)) valo.setVsync(!valo.getVsync());

        // wheel or brackets step through the material table; adding
        // MATERIAL_COUNT before the modulo keeps -1 from going negative
        int step = int(window.scrollDelta().y > 0.f) - int(window.scrollDelta().y < 0.f);
        if (window.wasKeyPressed(valo::Key::RightBracket)) step =  1;
        if (window.wasKeyPressed(valo::Key::LeftBracket))  step = -1;
        materialIndex = (materialIndex + MATERIAL_COUNT + step) % MATERIAL_COUNT;

        /* --- input: held keys, scaled by dt so they tune at the same rate
               whatever the frame rate --- */
        if (window.isKeyDown(valo::Key::Up))       bump       = clampf(bump + 1.0f * dt, 0.f, 4.f);
        if (window.isKeyDown(valo::Key::Down))     bump       = clampf(bump - 1.0f * dt, 0.f, 4.f);
        if (window.isKeyDown(valo::Key::Right))    noiseScale = clampf(noiseScale + 6.f * dt, 0.5f, 48.f);
        if (window.isKeyDown(valo::Key::Left))     noiseScale = clampf(noiseScale - 6.f * dt, 0.5f, 48.f);
        if (window.isKeyDown(valo::Key::PageUp))   variation  = clampf(variation + 0.35f * dt, 0.f, 1.f);
        if (window.isKeyDown(valo::Key::PageDown)) variation  = clampf(variation - 0.35f * dt, 0.f, 1.f);
        if (window.wasKeyPressed(valo::Key::Num1)) valo.setRenderScale(1.00f);
        if (window.wasKeyPressed(valo::Key::Num2)) valo.setRenderScale(0.75f);
        if (window.wasKeyPressed(valo::Key::Num3)) valo.setRenderScale(0.50f);
        if (window.wasKeyPressed(valo::Key::Num4)) valo.setRenderScale(0.25f);

        if (window.wasKeyPressed(valo::Key::R)) {
            orientation = Mat4f();
            detail      = true;
            displace    = false;
            bump        = DEFAULT_BUMP;
            noiseScale  = DEFAULT_NOISE;
            variation   = DEFAULT_VARIATION;
        }

        /* --- title --- */
        ++fpsFrames;
        if (std::chrono::duration<float>(now - fpsStart).count() >= 1.f) {
            fps       = int(fpsFrames);
            fpsFrames = 0;
            fpsStart  = now;
        }

        const Material& mat = MATERIALS[materialIndex];
        window.setTitle(
            std::string("VALO - PBR   ") + MATERIAL_NAMES[materialIndex] +
            "   bump "  + std::to_string(bump).substr(0, 4) +
            "  scale "  + std::to_string(noiseScale).substr(0, 4) +
            "  var "    + std::to_string(variation).substr(0, 4) +
            (detail   ? "" : "  [detail off]") +
            (displace ? "  [displace]" : "") +
            "   " + std::to_string(fps) + " fps"
        );

        /* --- camera and per-frame uniforms --- */
        const float aspect = window.getAspect();
        const Mat4f proj   = Mat4f::perspective(FOV_Y, aspect, 0.1f, 100.f);

        // the lights are authored in view space so the rig stays put as the
        // object turns; viewInv puts them back into the world space the shader
        // does its distance maths in
        scene.camPos = camPos;
        for (int i = 0; i < 4; ++i) {
            scene.lightPos[i] = (viewInv * Vec4f(LIGHTS[i].viewPos, 1.f)).xyz;
        }
        valo.updateUniformBuffer(sceneBuffer, &scene, sizeof(scene));

        /* --- per-draw uniforms ---

            The object rotates and the camera does not, so `orientation` is the
            whole model matrix. It is a pure rotation, which is why the shader
            can reuse it to transform normals - a matrix with scale in it would
            need the inverse transpose instead.
        */
        Uniforms u;
        u.mvp   = proj * view * orientation;
        u.model = orientation;

        u.albedo    = mat.albedo;
        u.metallic  = mat.metallic;
        u.roughness = mat.roughness;

        u.bump       = bump;
        u.noiseScale = noiseScale;
        u.variation  = variation;
        u.detail     = detail;
        u.displace   = displace;

        /* --- draw --- */
        obj.verts   = useSphere ? sphVb : cubeVb;
        obj.indices = useSphere ? sphIb : cubeIb;
        obj.setUniforms(u);

        sky.setUniforms(SkyUniforms{aspect, std::tan(FOV_Y * 0.5f)});

        valo.beginFrame();
        valo.submit(sky);       // first: depth test off, so it never occludes
        valo.submit(obj);
        valo.endFrame();
    }

    return 0;
}
