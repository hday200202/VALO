/*
    A rotating triangle through the Vulkan backend, driven entirely by the
    public VALO interface.

    Source ext/vulkan/env.sh first, or the loader finds neither the vendored
    ICD nor the validation layers.

      --frames N   run N frames then exit (default: until the window closes)

    Keys:
      F            cycle cullmode (None / Back / Front)
      V            toggle vsync
      Escape       quit
*/

#include <VALO.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

int main(int argc, char** argv) {
#ifndef VALO_HAS_VULKAN
    std::fprintf(stderr, "built without Vulkan - run ./build.sh to vendor the SDK\n");
    return 1;
#else
    int maxFrames = 0;      // 0 = run until the window closes

    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);

    valo::Window window(
        "VALO - Vulkan triangle",
        {800, 600},
        valo::WinFlags::Resize | valo::WinFlags::Vulkan
    );

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync   = true;
    cfg.backend = valo::Backend::Vulkan;

    if (!valo.init(window, cfg)) {
        std::fprintf(stderr, "[vk-triangle] VALO::init failed\n");
        return 1;
    }

    const valo::ProgramID program = valo.createProgram(
        "examples/shaders/triangle.vert",
        "examples/shaders/triangle.frag"
    );
    if (program == valo::DEFAULT_PROGRAM) {
        std::fprintf(stderr, "[vk-triangle] createProgram failed - is glslc on PATH?\n");
        return 1;
    }

    // mirrors the shader's push_constant block byte for byte
    struct Push { valo::Mat4f mvp; };
    static_assert(sizeof(Push) == 64, "push block must match the shader");

    valo::RenderCommand cmd;
    cmd.program     = program;
    cmd.indexCount  = 3;                        // no buffers, so this is the vertex count
    cmd.cullmode    = valo::CullMode::Back;

    const char* cullNames[] = { "None", "Back", "Front" };

    valo::Clock clock;
    clock.start();

    using clk = std::chrono::steady_clock;
    auto     fpsWindowStart = clk::now();
    uint32_t fpsFrames      = 0;
    uint64_t frames         = 0;

    while (window.pollEvents()) {
        ++fpsFrames;
        const float fpsElapsed = std::chrono::duration<float>(clk::now() - fpsWindowStart).count();
        if (fpsElapsed >= 1.f) {
            window.setTitle(
                "VALO - Vulkan triangle\t\tFPS: "
                + std::to_string(int(fpsFrames / fpsElapsed + 0.5f))
                + "\t\tVSync: " + (valo.getVsync() ? "on" : "off")
                + "\t\tCull: " + cullNames[int(cmd.cullmode)]
            );
            fpsWindowStart = clk::now();
            fpsFrames      = 0;
        }

        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::V)) valo.setVsync(!valo.getVsync());
        if (window.wasKeyPressed(valo::Key::F))
            cmd.cullmode = valo::CullMode((int(cmd.cullmode) + 1) % 3);

        // cycling hue makes a stalled frame loop obvious at a glance - a
        // frozen colour means the loop is blocked, not that nothing is drawing
        const float t = float(clock.elapsed().asSeconds());
        valo.setLayerClearColor(valo::VALO::BASE_LAYER, {
            0.5f + 0.5f * std::sin(t),
            0.5f + 0.5f * std::sin(t + 2.094f),   // +120 degrees
            0.5f + 0.5f * std::sin(t + 4.189f),   // +240 degrees
            1.f
        });

        // spinning in the screen plane: with no projection a Y rotation only
        // squashes the triangle. The aspect term stops NDC's square from
        // stretching it as the window resizes.
        const float aspect = window.getAspect();

        Push push;
        push.mvp = valo::Mat4f::scale({aspect > 0.f ? 1.f / aspect : 1.f, 1.f, 1.f})
                 * valo::Mat4f::rotationZ(t);

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
