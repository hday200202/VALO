/*
    An instanced cube grid through the Vulkan backend, driven entirely by the
    public VALO interface. The Vulkan counterpart of main.cpp - same geometry,
    same camera, GLSL instead of C++ shader callbacks, and no texture yet.

    Source ext/vulkan/env.sh first.

      --frames N   run N frames then exit
      --grid N     cubes per axis (default 8 -> 512 instances)

    Keys:
      Left click   grab/release the mouse to orbit
      Scroll       zoom
      W            toggle wireframe
      V            toggle vsync
      Escape       quit
*/

#include <VALO.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>

int main(int argc, char** argv) {
#ifndef VALO_HAS_VULKAN
    std::fprintf(stderr, "built without Vulkan - run ./build.sh to vendor the SDK\n");
    return 1;
#else
    int maxFrames = 0;
    int grid      = 8;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--grid") && i + 1 < argc) grid = std::max(1, std::atoi(argv[++i]));
    }

    const float SPACING = 4.f;
    const float CUBE    = 1.f;

    valo::Window window(
        "VALO - Vulkan cubes",
        {1280, 720},
        valo::WinFlags::Resize | valo::WinFlags::Vulkan
    );

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync   = true;
    cfg.backend = valo::Backend::Vulkan;

    if (!valo.init(window, cfg)) {
        std::fprintf(stderr, "[vk-cubes] VALO::init failed\n");
        return 1;
    }

    const valo::ProgramID program = valo.createProgram(
        "examples/shaders/cube.vert",
        "examples/shaders/cube.frag"
    );
    if (program == valo::DEFAULT_PROGRAM) {
        std::fprintf(stderr, "[vk-cubes] createProgram failed - is glslc on PATH?\n");
        return 1;
    }

    struct Vertex { valo::Vec3f pos; valo::Vec2f uv; };

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

    valo::VtxLayout      layout({valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec2f});
    valo::VertexBufferID vb = valo.createVertexBuffer(verts, 24, layout);
    valo::IndexBufferID  ib = valo.createIndexBuffer(indices, 36);

    if (vb == valo::EMPTY_BUFFER || ib == valo::EMPTY_BUFFER) {
        std::fprintf(stderr, "[vk-cubes] buffer creation failed\n");
        return 1;
    }

    struct Push { valo::Mat4f viewProj; valo::Vec4f params; };
    static_assert(sizeof(Push) == 80, "push block must match the shader");

    valo::RenderCommand cmd;
    cmd.verts         = vb;
    cmd.indices       = ib;
    cmd.program       = program;
    cmd.instanceCount = uint32_t(grid) * uint32_t(grid) * uint32_t(grid);
    cmd.cullmode      = valo::CullMode::Back;

    valo.setLayerClearColor(valo::VALO::BASE_LAYER, {0.05f, 0.06f, 0.09f, 1.f});

    const float gridExtent = grid * SPACING;
    float yaw = 0.6f, pitch = 0.4f, zoom = 1.f;
    bool  wireframe = false;

    using clk = std::chrono::steady_clock;
    auto     fpsWindowStart = clk::now();
    uint32_t fpsFrames      = 0;
    uint64_t frames         = 0;

    while (window.pollEvents()) {
        ++fpsFrames;
        const float fpsElapsed = std::chrono::duration<float>(clk::now() - fpsWindowStart).count();
        if (fpsElapsed >= 1.f) {
            window.setTitle(
                "VALO - Vulkan cubes\t\tFPS: "
                + std::to_string(int(fpsFrames / fpsElapsed + 0.5f))
                + "\t\tCubes: " + std::to_string(cmd.instanceCount)
                + "\t\tVSync: " + (valo.getVsync() ? "on" : "off")
            );
            fpsWindowStart = clk::now();
            fpsFrames      = 0;
        }

        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::V)) valo.setVsync(!valo.getVsync());
        if (window.wasKeyPressed(valo::Key::W)) {
            wireframe = !wireframe;
            cmd.fillmode = wireframe ? valo::FillMode::Wireframe : valo::FillMode::Solid;
        }
        if (window.wasMousePressed(valo::MouseButton::Left))
            window.isMouseGrabbed() ? window.releaseMouse() : window.grabMouse();

        if (window.isMouseGrabbed()) {
            const valo::Vec2f d = window.mouseDelta();
            window.setMousePositionLocal(window.getCenterLocal());
            yaw   -= d.x * 0.005f;
            pitch  = std::clamp(pitch + d.y * 0.005f, -1.5f, 1.5f);
        }

        zoom = std::clamp(zoom - window.scrollDelta() * 0.05f, 0.05f, 5.f);

        const float dist = gridExtent * 1.2f * zoom;
        const valo::Vec3f eye = {
            dist * std::cos(pitch) * std::sin(yaw),
            dist * std::sin(pitch),
            dist * std::cos(pitch) * std::cos(yaw),
        };

        const valo::Mat4f view = valo::Mat4f::lookAt(eye, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
        const valo::Mat4f proj = valo::Mat4f::perspective(
            std::numbers::pi_v<float> / 3.f, window.getAspect(), 0.1f, gridExtent * 20.f);

        Push push;
        push.viewProj = proj * view;
        push.params   = {float(grid), SPACING, CUBE, 0.f};

        valo.beginFrame();
        cmd.setUniforms(push);
        valo.submit(cmd);
        valo.endFrame();

        ++frames;
        if (maxFrames > 0 && frames >= uint64_t(maxFrames)) break;
    }

    valo.shutdown();

    std::printf("\n%llu frames\n", (unsigned long long)frames);
    std::printf("%s\n", frames > 0 ? "RESULT: ok" : "RESULT: no frames ran");
    return frames > 0 ? 0 : 1;
#endif
}
