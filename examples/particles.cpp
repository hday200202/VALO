// Point-topology demo: a spiral galaxy of 8000 stars, fully animated in the
// vertex shader from a `time` uniform (the vertex buffer is uploaded once and
// never touched again). Shows valo_PointSize, additive blending, and depthWrite=false.
#include <VALO.hpp>
#include <cstring>
#include <cmath>
#include <numbers>
#include <vector>

#include "shaders/galaxy.vert.hpp"
#include "shaders/galaxy.frag.hpp"

using namespace valo;

static float frand(uint32_t& seed) {                         // cheap LCG, 0..1
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) * (1.f / 16777216.f);
}

int main(int argc, char** argv) {
    // one scene, either backend: the software path resolves the shader
    // paths to registered C++ callbacks, the Vulkan path translates them
    bool useVulkan = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--vulkan")) useVulkan = true;

    Window window("VALO - particles", {1280, 960},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    VALO valo;
    RenderConfig cfg;
    cfg.vsync       = true;
    cfg.renderScale = 1.f;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;

    constexpr uint32_t kStars = 8000;
    constexpr int      kArms  = 3;
    std::vector<Star>     stars(kStars);
    std::vector<uint32_t> sidx(kStars);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < kStars; ++i) {
        const float r    = std::sqrt(frand(seed)) * kGalaxyRadius;       // uniform over the disk
        const int   arm  = int(frand(seed) * kArms);
        const float armA = arm * 6.2831853f / kArms + r * 1.6f;           // logarithmic-ish twist
        const float scat = (frand(seed) - 0.5f) * (0.35f + 0.9f * (1.f - r / kGalaxyRadius));
        stars[i] = {r, armA + scat, (frand(seed) - 0.5f) * 2.f, frand(seed)};
        sidx[i]  = i;
    }

    VtxLayout      slayout({VtxAttrib::Float, VtxAttrib::Float, VtxAttrib::Float, VtxAttrib::Float});
    VertexBufferID svb   = valo.createVertexBuffer(stars.data(), kStars, slayout);
    IndexBufferID  sib   = valo.createIndexBuffer(sidx.data(), kStars);
    ProgramID      sprog = valo.createProgram("examples/shaders/galaxy.vert.hpp", "examples/shaders/galaxy.frag.hpp");

    RenderCommand galaxy;
    galaxy.verts      = svb;
    galaxy.indices    = sib;
    galaxy.program    = sprog;
    galaxy.topology   = Topology::Points;
    galaxy.depthWrite = false;                              // stars don't occlude each other
    galaxy.blendmode  = BlendMode::Additive;                // overlapping stars add up instead of overwriting

    // simple fly camera: W/S forward/back, A/D strafe, Q/E down/up, ESC quit
    Vec3f camPos = {0.f, 2.5f, 7.f};
    const Vec3f camFwd = Vec3f{0.f, -0.35f, -1.f}.normalized();
    const Vec3f camRight = camFwd.cross({0.f, 1.f, 0.f}).normalized();
    Clock timer;
    timer.start();

    while (window.pollEvents()) {
        const float dt = float(valo.getDeltaTime());
        if (window.wasKeyPressed(Key::Escape)) window.close();

        const float speed = 3.f * dt;
        if (window.isKeyDown(Key::W)) camPos += camFwd   * speed;
        if (window.isKeyDown(Key::S)) camPos -= camFwd   * speed;
        if (window.isKeyDown(Key::D)) camPos += camRight * speed;
        if (window.isKeyDown(Key::A)) camPos -= camRight * speed;
        if (window.isKeyDown(Key::E)) camPos.y += speed;
        if (window.isKeyDown(Key::Q)) camPos.y -= speed;
        const Mat4f view = Mat4f::lookAt(camPos, camPos + camFwd, {0.f, 1.f, 0.f});

        const Vec2u size   = window.getSize();
        const float aspect = float(size.x) / float(size.y);
        const Mat4f proj   = Mat4f::perspective(std::numbers::pi_v<float> / 3.f, aspect, 0.1f, 100.f);
        const float t      = float(timer.elapsed().asSeconds());

        valo.beginFrame();

        // tilted disk, slowly precessing; everything else happens in the shader
        Mat4f galaxyModel = Mat4f::rotationY(t * 0.05f) * Mat4f::rotationX(0.55f);
        galaxy.setUniforms(GUniforms{proj * view, galaxyModel, t});
        valo.submit(galaxy);

        valo.endFrame();
    }

    return 0;
}
