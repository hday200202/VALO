// "Seascape" by Alexander Alekseev aka TDM (https://www.shadertoy.com/view/Ms2SD1,
// CC BY-NC-SA 3.0), ported to VALO's C++ shaders. One fullscreen triangle; the
// fragment shader height-traces an animated ocean and shades it against the sky.
//
// Controls
//   left click    grab the mouse to look around, right click releases
//   w a s d       move, q / e down / up, shift faster
//   scroll        wave height
//   1 - 4         render scale 1, 0.5, 0.25, 0.125
//   space         pause the waves
//   escape        quit
//
// Flags
//   --vulkan      run on the Vulkan backend
//   --validation  Vulkan validation layers
//   --time T      start T seconds in
//   --paused      start paused

#include <VALO.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "shaders/seascape.vert.hpp"
#include "shaders/seascape.frag.hpp"

using valo::Vec3f;

int main(int argc, char** argv) {
    bool  useVulkan = false, validation = false;
    float time      = 0.f;
    bool  paused    = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--vulkan"))     useVulkan  = true;
        if (!std::strcmp(argv[i], "--validation")) validation = true;
        if (!std::strcmp(argv[i], "--time") && i + 1 < argc) time = float(std::atof(argv[++i]));
        if (!std::strcmp(argv[i], "--paused")) paused = true;
    }

    valo::Window window("VALO - seascape", {1280, 720},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    valo::RenderConfig cfg;
    cfg.vsync      = true;
    cfg.validation = validation;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;

    valo::VALO valo;
    if (!valo.init(window, cfg)) return 1;
    valo.setRenderScale(0.5f);

    sea::SeaVertex tri[3] = {{{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};
    uint32_t idx[3] = {0, 1, 2};

    valo::RenderCommand cmd;
    cmd.verts     = valo.createVertexBuffer(tri, 3, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    cmd.indices   = valo.createIndexBuffer(idx, 3);
    cmd.program   = valo.createProgram("examples/shaders/seascape.vert.hpp", "examples/shaders/seascape.frag.hpp");
    cmd.depthTest = false;
    cmd.cullmode  = valo::CullMode::None;

    // where the original's flight starts, looking out to sea (yaw 0 = down -z)
    Vec3f camPos = {0.f, 3.5f, 0.f};
    float yaw    = 0.f;
    float pitch  = -0.3f;
    const float MOUSE_SENS = 0.0025f;
    const float WALK_SPEED = 5.f;
    const float RUN_SPEED  = 20.f;

    float seaHeight = 0.6f;
    const float HEIGHT_STEP = 1.1f;     // per wheel notch, multiplicative
    const float HEIGHT_MIN  = 0.05f;
    const float HEIGHT_MAX  = 3.f;

    using clk = std::chrono::steady_clock;
    auto  fpsStart  = clk::now();
    auto  lastFrame = clk::now();
    int   fpsFrames = 0;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::Space))  paused = !paused;
        if (window.wasKeyPressed(valo::Key::Num1))   valo.setRenderScale(1.f);
        if (window.wasKeyPressed(valo::Key::Num2))   valo.setRenderScale(0.5f);
        if (window.wasKeyPressed(valo::Key::Num3))   valo.setRenderScale(0.25f);
        if (window.wasKeyPressed(valo::Key::Num4))   valo.setRenderScale(0.125f);
        if (window.wasMousePressed(valo::MouseButton::Left))  window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();

        const float scroll = window.scrollDelta().y;
        if (scroll != 0.f)
            seaHeight = std::clamp(seaHeight * std::pow(HEIGHT_STEP, scroll), HEIGHT_MIN, HEIGHT_MAX);

        const auto  now = clk::now();
        const float dt  = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        if (!paused) time += dt;

        if (window.isMouseGrabbed()) {
            const valo::Vec2f d = window.mouseDelta();
            yaw   += d.x * MOUSE_SENS;
            pitch  = std::clamp(pitch - d.y * MOUSE_SENS, -1.5f, 1.5f);
        }

        const Vec3f cw = Vec3f(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)).normalized();
        const Vec3f cu = cw.cross(Vec3f(0.f, 1.f, 0.f)).normalized();
        const Vec3f cv = cu.cross(cw);

        // capped so a stalled frame doesn't fling the camera
        const float step = (window.isKeyDown(valo::Key::LShift) ? RUN_SPEED : WALK_SPEED) * std::min(dt, 0.1f);
        if (window.isKeyDown(valo::Key::W)) camPos += cw * step;
        if (window.isKeyDown(valo::Key::S)) camPos -= cw * step;
        if (window.isKeyDown(valo::Key::D)) camPos += cu * step;
        if (window.isKeyDown(valo::Key::A)) camPos -= cu * step;
        if (window.isKeyDown(valo::Key::E)) camPos.y += step;
        if (window.isKeyDown(valo::Key::Q)) camPos.y -= step;

        ++fpsFrames;
        const float elapsed = std::chrono::duration<float>(clk::now() - fpsStart).count();
        if (elapsed >= 1.f) {
            window.setTitle("VALO - seascape   " + std::to_string(int(fpsFrames / elapsed + 0.5f)) + " fps   scale "
                          + std::to_string(valo.getRenderScale()).substr(0, 5) + "   waves " + std::to_string(seaHeight).substr(0, 4));
            fpsStart  = clk::now();
            fpsFrames = 0;
        }

        cmd.setUniforms(sea::SeaUniforms{camPos, time, cu, window.getAspect(), cv, float(window.getSize().x), cw, seaHeight});

        valo.beginFrame();
        valo.submit(cmd);
        valo.endFrame();
    }

    valo.shutdown();
    return 0;
}
