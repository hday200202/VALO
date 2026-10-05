/*
    The smallest complete VALO program: a spinning triangle with a colour at
    each corner. Start here.

      --vulkan   run on the Vulkan backend
*/

#include <VALO.hpp>

#include <cstring>

#include "shaders/hello.vert.hpp"
#include "shaders/hello.frag.hpp"

int main(int argc, char** argv) {
    const bool useVulkan = argc > 1 && !std::strcmp(argv[1], "--vulkan");

    // 1. a window, and VALO drawing into it
    valo::Window window(
        (useVulkan ? "Hello VALO - Vulkan" : "Hello VALO - Software"), 
        {800, 600},
        valo::WinFlags::Resize | 
        (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None)
    );

    valo::RenderConfig cfg;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;

    valo::VALO valo;
    if (!valo.init(window, cfg)) return 1;

    // 2. the shader pair, written in C++ in shaders/hello.*.hpp
    const valo::ProgramID program = valo.createProgram(
        "examples/shaders/hello.vert.hpp",
        "examples/shaders/hello.frag.hpp"
    );

    // 3. three corners, each with a position and a colour
    HelloVertex verts[3] = {
        {{ 0.0f,  0.6f}, {1.f, 0.f, 0.f}},
        {{-0.6f, -0.4f}, {0.f, 1.f, 0.f}},
        {{ 0.6f, -0.4f}, {0.f, 0.f, 1.f}},
    };
    uint32_t indices[3] = {0, 1, 2};

    // 4. a draw: which geometry, drawn with which program
    valo::RenderCommand cmd;

    cmd.verts = valo.createVertexBuffer(
        verts, 3, 
        valo::VtxLayout({
            valo::VtxAttrib::Vec2f, 
            valo::VtxAttrib::Vec3f
        })
    );

    cmd.indices  = valo.createIndexBuffer(indices, 3);
    cmd.program  = program;
    cmd.cullmode = valo::CullMode::None;

    // 5. the loop: update the uniforms, then submit the draw between
    //    beginFrame and endFrame
    float angle = 0.f;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();

        angle += float(valo.getDeltaTime());

        const valo::Vec2u size = window.getSize();
        cmd.setUniforms(HelloUniforms{ angle, float(size.x) / float(size.y) });

        valo.beginFrame();
        valo.submit(cmd);
        valo.endFrame();
    }

    valo.shutdown();
    return 0;
}
