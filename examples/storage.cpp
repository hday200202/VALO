/*
    Storage buffer test.

    A fullscreen quad split into four bands, each one assertion the fragment
    shader makes about data it can only have got from a storage buffer. Green
    passes, red fails - no readback path is needed, and the same picture has to
    appear on both backends.

      top        slot 0 holds 1000 floats and size() sees all of them
      second     summing the first 100 matches what the CPU computed
      third      slot 2 is reachable with slots 0 and 1 unbound
      bottom     a 32-byte struct element round-trips intact

    U updates the storage buffer to a new scale every frame, which exercises
    retire-and-replace rather than a one-shot upload - watch VMA on close.

      --vulkan   run on the Vulkan backend
      --frames N run N frames then exit
*/

#include <VALO.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

#include "shaders/storage.vert.hpp"
#include "shaders/storage.frag.hpp"

int main(int argc, char** argv) {
    bool useVulkan = false;
    int  maxFrames = 0;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--vulkan")) useVulkan = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
    }

    valo::Window window("VALO - storage buffers", {800, 600},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = true;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;

    const valo::ProgramID program = valo.createProgram(
        "examples/shaders/storage.vert.hpp",
        "examples/shaders/storage.frag.hpp"
    );
    if (program == valo::DEFAULT_PROGRAM) {
        std::fprintf(stderr, "[storage] createProgram failed\n");
        return 1;
    }

    // a quad, so every pixel runs the fragment shader
    StorageVertex quad[4] = {{{-1,-1}}, {{1,-1}}, {{1,1}}, {{-1,1}}};
    uint32_t      idx[6]  = {0, 1, 2, 0, 2, 3};

    constexpr uint32_t COUNT = 1000;
    float scale = 0.001f;

    std::vector<float> numbers(COUNT);
    for (uint32_t i = 0; i < COUNT; ++i) numbers[i] = float(i) * scale;

    // deliberately not a multiple of 16, so size() has to divide rather than
    // assume a round element count
    std::vector<Particle> particles(4);
    for (uint32_t i = 0; i < 4; ++i)
        particles[i] = { {float(i), float(i) + 1.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f} };

    const valo::StorageBufferID numberBuffer =
        valo.createStorageBuffer(numbers.data(), numbers.size() * sizeof(float));

    const valo::StorageBufferID particleBuffer =
        valo.createStorageBuffer(particles.data(), particles.size() * sizeof(Particle));

    if (numberBuffer == valo::EMPTY_BUFFER || particleBuffer == valo::EMPTY_BUFFER) {
        std::fprintf(stderr, "[storage] createStorageBuffer failed\n");
        return 1;
    }

    valo::RenderCommand cmd;
    cmd.verts     = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    cmd.indices   = valo.createIndexBuffer(idx, 6);
    cmd.program   = program;
    cmd.cullmode  = valo::CullMode::None;
    cmd.depthTest = false;
    cmd.depthWrite = false;

    // slot 1 is left empty on purpose: the Vulkan push-descriptor loop indexes
    // its write array by a running count while dstBinding is the slot, and a
    // gap is the only thing that tells the two apart
    cmd.bindStorageBuffer(0, numberBuffer);
    cmd.bindStorageBuffer(2, particleBuffer);

    bool     updating = false;
    uint64_t frames   = 0;

    std::printf("green = pass, red = fail. U toggles per-frame updates.\n");

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::U)) updating = !updating;

        if (updating) {
            scale = scale > 0.004f ? 0.001f : scale + 0.0005f;
            for (uint32_t i = 0; i < COUNT; ++i) numbers[i] = float(i) * scale;
            valo.updateStorageBuffer(numberBuffer, numbers.data(), numbers.size() * sizeof(float));
        }

        cmd.setUniforms(StorageUniforms{ COUNT, scale });

        valo.beginFrame();
        valo.submit(cmd);
        valo.endFrame();

        ++frames;
        if (maxFrames > 0 && frames >= uint64_t(maxFrames)) break;
    }

    valo.shutdown();
    std::printf("\n%llu frames\n", (unsigned long long)frames);
    return 0;
}
