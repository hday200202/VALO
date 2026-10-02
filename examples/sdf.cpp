// Raymarched SDF primitives - a port of Inigo Quilez's "Raymarching - Primitives"
// (https://www.shadertoy.com/view/Xds3zN, MIT) to VALO's C++ shaders.
//
// The whole scene is one fullscreen triangle; every pixel raymarches in the
// fragment shader. This is a stress test for the fragment path, so it runs at
// a reduced render scale by default. Keys 1-4 change the scale, Esc quits.
// Left-click grabs the mouse for FPS look; WASD move, Q/E down/up, Shift = fast,
// right-click releases.

#include <VALO.hpp>
#include <cstring>
#include <cmath>
#include <numbers>
#include <chrono>
#include <string>
#include <algorithm>

#include "shaders/sdf.vert.hpp"
#include "shaders/sdf.frag.hpp"

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Color;


int main(int argc, char** argv) {
    // one scene, either backend: the software path resolves the shader
    // paths to registered C++ callbacks, the Vulkan path translates them
    bool useVulkan = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--vulkan")) useVulkan = true;

    valo::Window window("VALO - SDF primitives", {1280, 720},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = false;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;
    valo.setRenderScale(0.5f);

    // one big triangle covering the screen (CCW), no index tricks needed
    Vertex tri[3] = {{{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};
    uint32_t idx[3] = {0, 1, 2};
    valo::VtxLayout layout({valo::VtxAttrib::Vec2f});
    valo::RenderCommand cmd;
    cmd.verts     = valo.createVertexBuffer(tri, 3, layout);
    cmd.indices   = valo.createIndexBuffer(idx, 3);
    cmd.program   = valo.createProgram("shaders/sdf.vert.hpp", "shaders/sdf.frag.hpp");
    cmd.depthTest = false;
    cmd.cullmode  = valo::CullMode::None;

    // FPS camera: start where the original's orbit would be at t=0, looking at the scene
    Vec3f camPos = {4.75f, 1.45f, -0.75f + 4.5f * std::sin(3.2f)};
    float yaw    = std::atan2(0.25f - camPos.x, -0.75f - camPos.z) ;   // face the target
    float pitch  = -0.35f;
    const float mouseSens = 0.0025f;

    using clk = std::chrono::steady_clock;
    auto fpsStart = clk::now(); uint32_t fpsFrames = 0;
    uint64_t lastTicks = 0; auto tStart = clk::now();

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::Num1)) valo.setRenderScale(1.00f);
        if (window.wasKeyPressed(valo::Key::Num2)) valo.setRenderScale(0.50f);
        if (window.wasKeyPressed(valo::Key::Num3)) valo.setRenderScale(0.25f);
        if (window.wasKeyPressed(valo::Key::Num4)) valo.setRenderScale(0.125f);
        if (window.wasMousePressed(valo::MouseButton::Left))  window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();
        if (window.wasKeyPressed(valo::Key::V)) valo.setVsync(!valo.getVsync());

        const float dt = [&] {
            uint64_t now = uint64_t(std::chrono::duration<double, std::milli>(clk::now() - tStart).count());
            float d = lastTicks ? (now - lastTicks) / 1000.f : 0.f; lastTicks = now; return std::min(d, 0.1f);
        }();

        if (window.isMouseGrabbed()) {
            const Vec2f d = window.mouseDelta();
            yaw   += d.x * mouseSens;
            pitch  = clampf(pitch - d.y * mouseSens, -1.5f, 1.5f);
        }

        // camera basis from yaw/pitch (yaw 0 = looking down -z)
        const Vec3f cw = normalize(Vec3f{std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)});
        const Vec3f cu = normalize(cw.cross(Vec3f{0.f, 1.f, 0.f}));
        const Vec3f cv = cu.cross(cw);

        const float speed = (window.isKeyDown(valo::Key::LShift) ? 6.f : 2.f) * dt;
        if (window.isKeyDown(valo::Key::W)) camPos += cw * speed;
        if (window.isKeyDown(valo::Key::S)) camPos -= cw * speed;
        if (window.isKeyDown(valo::Key::D)) camPos += cu * speed;
        if (window.isKeyDown(valo::Key::A)) camPos -= cu * speed;
        if (window.isKeyDown(valo::Key::E)) camPos.y += speed;
        if (window.isKeyDown(valo::Key::Q)) camPos.y -= speed;

        ++fpsFrames;
        float el = std::chrono::duration<float>(clk::now() - fpsStart).count();
        if (el >= 1.f) {
            window.setTitle("VALO - SDF primitives   " + std::to_string(int(fpsFrames / el + 0.5f)) + " fps   scale " + std::to_string(valo.getRenderScale()).substr(0, 4));
            fpsStart = clk::now(); fpsFrames = 0;
        }

        valo.beginFrame();
        cmd.setUniforms(Uniforms{camPos, cu, cv, cw, window.getAspect()});
        valo.submit(cmd);
        valo.endFrame();
    }
    return 0;
}
