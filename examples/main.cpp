#include <VALO.hpp>
#include <cstring>
#include <numbers>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <string>
#include "shaders/cubes.vert.hpp"
#include "shaders/cubes.frag.hpp"

int main(int argc, char** argv) {
    // one scene, either backend: the software path resolves the shader
    // paths to registered C++ callbacks, the Vulkan path translates them
    bool useVulkan = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--vulkan")) useVulkan = true;

    valo::Window window("VALO", {1280, 720},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = true;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;
    valo.setRenderScale(1.f);
    valo.setLayerClearColor(valo::VALO::BASE_LAYER, {.8f, .9f, 1.f, 1.f});

    bool ortho = false;
    bool texToggle = false;

    Vertex verts[24] = {
        {{-1,-1, 1},{0,1}}, {{ 1,-1, 1},{1,1}}, {{ 1, 1, 1},{1,0}}, {{-1, 1, 1},{0,0}},  // +z
        {{ 1,-1,-1},{0,1}}, {{-1,-1,-1},{1,1}}, {{-1, 1,-1},{1,0}}, {{ 1, 1,-1},{0,0}},  // -z
        {{ 1,-1, 1},{0,1}}, {{ 1,-1,-1},{1,1}}, {{ 1, 1,-1},{1,0}}, {{ 1, 1, 1},{0,0}},  // +x
        {{-1,-1,-1},{0,1}}, {{-1,-1, 1},{1,1}}, {{-1, 1, 1},{1,0}}, {{-1, 1,-1},{0,0}},  // -x
        {{-1, 1, 1},{0,1}}, {{ 1, 1, 1},{1,1}}, {{ 1, 1,-1},{1,0}}, {{-1, 1,-1},{0,0}},  // +y
        {{-1,-1,-1},{0,1}}, {{ 1,-1,-1},{1,1}}, {{ 1,-1, 1},{1,0}}, {{-1,-1, 1},{0,0}},  // -y
    };

    uint32_t indices[36];
    for (uint32_t f = 0; f < 6; ++f) {
        uint32_t* i = indices + f * 6;
        i[0] = f*4; i[1] = f*4+1; i[2] = f*4+2;
        i[3] = f*4; i[4] = f*4+2; i[5] = f*4+3;
    }

    valo::TextureID      rust   = valo.createTexture("examples/textures/rust.jpg");
    valo::TextureID      dirt   = valo.createTexture("examples/textures/dirt.jpg");
    valo::VtxLayout      layout({valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec2f});
    valo::VertexBufferID vb     = valo.createVertexBuffer(verts, 24, layout);
    valo::IndexBufferID  ib     = valo.createIndexBuffer(indices, 36);
    valo::ProgramID      prog   = valo.createProgram("examples/shaders/cubes.vert.hpp", "examples/shaders/cubes.frag.hpp");

    valo::RenderCommand cube;
    cube.verts   = vb;
    cube.indices = ib;
    cube.program = prog;
    // cube.cullmode = valo::CullMode::None;
    // cube.fillmode = valo::FillMode::Wireframe;
    // cube.indexCount = 18;
    // cube.firstIndex = 9;

    cube.instanceCount = GRID * GRID * GRID;

    // orbit camera around the grid's center: yaw/pitch from the mouse, distance from scroll
    const float gridExtent = GRID * SPACING;
    float yaw = 0.6f, pitch = 0.4f, zoom = 1.f;

    using clock = std::chrono::steady_clock;
    auto     fpsWindowStart = clock::now();
    uint32_t fpsFrames      = 0;

    while (window.pollEvents()) {
        ++fpsFrames;
        const float fpsElapsed = std::chrono::duration<float>(clock::now() - fpsWindowStart).count();
        if (fpsElapsed >= 1.f) {
            window.setTitle("VALO\t\tFPS: " + std::to_string(int(fpsFrames / fpsElapsed + 0.5f)) + "\t\tVSync: " + (valo.getVsync() ? "on" : "off") );
            fpsWindowStart = clock::now();
            fpsFrames      = 0;
        }
        if (window.wasMousePressed(valo::MouseButton::Left)) 
            window.isMouseGrabbed() ? window.releaseMouse() : window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();
        if (window.wasKeyPressed(valo::Key::Num1)) valo.setRenderScale(1.00f);
        if (window.wasKeyPressed(valo::Key::Num2)) valo.setRenderScale(0.75f);
        if (window.wasKeyPressed(valo::Key::Num3)) valo.setRenderScale(0.50f);
        if (window.wasKeyPressed(valo::Key::Num4)) valo.setRenderScale(0.25f);
        if (window.wasKeyPressed(valo::Key::T)) texToggle = !texToggle;
        if (window.wasKeyPressed(valo::Key::V)) valo.setVsync(!valo.getVsync());
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::O)) ortho = !ortho;
        if (window.isMouseGrabbed()) {
            const valo::Vec2f d = window.mouseDelta();
            window.setMousePositionLocal(window.getCenterLocal());
            yaw   += d.x * 0.005f;
            pitch  = std::clamp(pitch + d.y * 0.005f, -1.5f, 1.5f);
        }

        zoom = std::clamp(zoom - window.scrollDelta() * 0.05f, 0.05f, 5.f);

        // camera sits on a sphere around the origin; the grid is centered there
        const float dist = gridExtent * 1.2f * zoom;
        const valo::Vec3f eye = {
            dist * std::cos(pitch) * std::sin(yaw),
            dist * std::sin(pitch),
            dist * std::cos(pitch) * std::cos(yaw),
        };
        const valo::Mat4f view = valo::Mat4f::lookAt(eye, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});

        const float aspect  = window.getAspect();
        const float farClip = gridExtent * 4.f * 5.f;   // covers the farthest zoom
        valo::Mat4f proj;
        if (!ortho) {
            proj = valo::Mat4f::perspective(std::numbers::pi_v<float> / 3.f, aspect, 0.1f, farClip);
        } else {
            const float h = dist * 0.6f;
            proj = valo::Mat4f::orthographic(-h * aspect, h * aspect, -h, h, 0.1f, farClip);
        }

        valo.beginFrame();
        cube.setUniforms(Uniforms{proj * view, texToggle ? rust : dirt});
        valo.submit(cube);
        valo.endFrame();
    }

    return 0;
}
