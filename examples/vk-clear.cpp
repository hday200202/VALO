/*
    Phase 1 verification: a window that clears to a colour through the Vulkan
    backend. Nothing is drawn - the clear is the attachment's loadOp.

    Source ext/vulkan/env.sh first, or the loader finds neither the vendored
    ICD nor the validation layers.

      --frames N   run N frames then exit (default: until the window closes)
      --quiet      no per-second reporting

    What this is actually testing, beyond "a colour appears":

      - frame index wrap. Semaphore and fence reuse bugs only surface once the
        frame counter passes framesInFlight, so a single frame proves nothing.
      - sustained running. A fence-ordering mistake hangs rather than printing
        anything, so the frame counter stalling is the signal.
      - resize. OUT_OF_DATE / SUBOPTIMAL have to be absorbed silently.
      - clean teardown, with no leaked-object report from validation.
*/

#include <VALO.hpp>

#include <cstdio>
#include <cstring>
#include <cmath>

int main(int argc, char** argv) {
#ifndef VALO_HAS_VULKAN
    std::fprintf(stderr, "built without Vulkan - run ./build.sh to vendor the SDK\n");
    return 1;
#else
    int  maxFrames = 0;       // 0 = run until the window closes
    bool quiet     = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--quiet"))             quiet = true;
    }

    valo::Window window(
        "VALO - Vulkan clear",
        {800, 600},
        valo::WinFlags::Resize | valo::WinFlags::Vulkan
    );

    /*
        Through Renderer with Backend::Vulkan rather than VulkanRenderer
        directly: that exercises the whole dispatch table, which is the
        point now that the backends are symmetric.
    */
    valo::RenderConfig cfg;
    cfg.window      = &window;
    cfg.sdlWindow   = window.sdlHandle();
    cfg.backend     = valo::Backend::Vulkan;

    valo::Renderer renderer;
    if (!renderer.init(cfg)) {
        std::fprintf(stderr, "[vk-clear] Renderer::init failed\n");
        return 1;
    }

    valo::Clock clock;
    clock.start();

    valo::Clock report;
    report.start();

    uint64_t frames = 0;
    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();

        // cycling hue makes a stalled frame loop obvious at a glance - a
        // frozen colour means the loop is blocked, not that nothing is drawing
        const float t = float(clock.elapsed().asSeconds());
        const valo::Color clearColor = {
            0.5f + 0.5f * std::sin(t),
            0.5f + 0.5f * std::sin(t + 2.094f),   // +120 degrees
            0.5f + 0.5f * std::sin(t + 4.189f),   // +240 degrees
            1.f
        };

        renderer.clear(clearColor, valo::Renderer::BACKBUFFER, 0.f);
        renderer.show(valo::Renderer::BACKBUFFER);
        ++frames;

        if (!quiet && report.elapsed().asSeconds() >= 1.0) {
            std::printf("  %llu frames, %.0f fps\n",
                        (unsigned long long)frames,
                        double(frames) / clock.elapsed().asSeconds());
            report.reset();
        }

        if (maxFrames > 0 && frames >= uint64_t(maxFrames)) break;
    }

    renderer.shutdown();

    // nothing counts failures any more - clear and show report their own
    // errors to stderr, and a hang or a validation complaint is the signal
    std::printf("\n%llu frames\n", (unsigned long long)frames);
    std::printf("%s\n", frames > 0 ? "RESULT: ok" : "RESULT: no frames ran");
    return frames > 0 ? 0 : 1;
#endif
}
