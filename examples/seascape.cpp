// "Seascape" by Alexander Alekseev aka TDM (https://www.shadertoy.com/view/Ms2SD1,
// CC BY-NC-SA 3.0), ported to VALO's C++ shaders. One fullscreen triangle; the
// fragment shader height-traces an animated ocean and shades it against the sky.
//
// Controls
//   1 - 4         render scale 1, 0.5, 0.25, 0.125
//   space         pause
//   escape        quit
//
// Flags
//   --vulkan      run on the Vulkan backend
//   --validation  Vulkan validation layers
//   --time T      start T seconds in
//   --paused      start paused

#include <VALO.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include "shaders/seascape.vert.hpp"
#include "shaders/seascape.frag.hpp"

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

    using clk = std::chrono::steady_clock;
    auto  fpsStart  = clk::now();
    int   fpsFrames = 0;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::Space))  paused = !paused;
        if (window.wasKeyPressed(valo::Key::Num1))   valo.setRenderScale(1.f);
        if (window.wasKeyPressed(valo::Key::Num2))   valo.setRenderScale(0.5f);
        if (window.wasKeyPressed(valo::Key::Num3))   valo.setRenderScale(0.25f);
        if (window.wasKeyPressed(valo::Key::Num4))   valo.setRenderScale(0.125f);

        if (!paused) time += float(valo.getDeltaTime());

        ++fpsFrames;
        const float elapsed = std::chrono::duration<float>(clk::now() - fpsStart).count();
        if (elapsed >= 1.f) {
            window.setTitle("VALO - seascape   " + std::to_string(int(fpsFrames / elapsed + 0.5f)) + " fps   scale "
                          + std::to_string(valo.getRenderScale()).substr(0, 5));
            fpsStart  = clk::now();
            fpsFrames = 0;
        }

        cmd.setUniforms(sea::SeaUniforms{time, window.getAspect(), float(window.getSize().x), 0.f});

        valo.beginFrame();
        valo.submit(cmd);
        valo.endFrame();
    }

    valo.shutdown();
    return 0;
}
