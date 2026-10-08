// Render to texture.
//
// Two layers: the first draws rounded rects into an offscreen 512x512 target,
// the second draws a single quad to the backbuffer that samples that target.
// If render targets work, the window shows the offscreen scene inside a
// moving panel rather than the rects drawn directly.
//
// --show-target takes the other route through the same target: nothing is drawn
// to the backbuffer at all, and endFrame() is handed the target so the backend
// presents it directly, stretched to the window.
//
// --track creates the target with createTarget(), which sizes it to the window
// and marks it window-tracking, so the backend has to rebuild it on every
// resize. The window is resizable in that mode - drag it and the offscreen
// scene has to stay sharp and correctly scaled.
//
// --update swaps the offscreen target for a CPU-owned texture that is rewritten
// every frame: a moving block via updateTexture(x, y, w, h, ...) and a full
// refill every two seconds via updateTexture(data). Same sampling path, so it
// exercises the upload routines rather than the attachment ones.
//
//   --vulkan        use the Vulkan backend instead of software
//   --show-target   present the target itself instead of sampling it
//   --track         window-sized target that follows resizes
//   --update        sample a CPU-updated texture instead of a render target
//   s               toggle show-target at runtime
//   escape          quit

#include <VALO.hpp>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

#include "shaders/roundrect.vert.hpp"
#include "shaders/roundrect.frag.hpp"
#include "shaders/blit.vert.hpp"
#include "shaders/blit.frag.hpp"

using valo::Vec2f;
using valo::Vec4f;
using valo::Mat4f;

constexpr uint32_t TARGET_SIZE = 512;

int main(int argc, char** argv) {
    bool useVulkan = false, showTarget = false, track = false, update = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--vulkan"))      useVulkan  = true;
        if (!std::strcmp(argv[i], "--show-target")) showTarget = true;
        if (!std::strcmp(argv[i], "--track"))       track      = true;
        if (!std::strcmp(argv[i], "--update"))      update     = true;
    }

    valo::Window window("VALO - render target", {960, 720},
        (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None)
      | (track     ? valo::WinFlags::Resize : valo::WinFlags::None));

    valo::VALO valo; valo::RenderConfig cfg; cfg.vsync = true;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;

    valo.setLayerClearColor(valo::VALO::BASE_LAYER, {0.08f, 0.09f, 0.11f, 1.f});

    // no size => sized to the window and flagged window-tracking
    const valo::RenderTargetID target = track
        ? valo.createTarget()
        : valo.createTarget(TARGET_SIZE, TARGET_SIZE);
    if (target == valo::EMPTY_TEXTURE) { std::fprintf(stderr, "createTarget failed\n"); return 1; }

    // drawn before the base layer so the target is complete by the time it is sampled
    const valo::LayerID offscreen = valo.addLayer(target, {0.15f, 0.05f, 0.25f, 1.f});
    valo.setLayerOrder({offscreen, valo::VALO::BASE_LAYER});

    // --update mode: a CPU-owned texture rewritten through the update calls
    constexpr uint32_t TEX_SIZE = 256;
    std::vector<uint32_t> pixels(TEX_SIZE * TEX_SIZE);
    auto fillChecker = [&](uint32_t tint) {
        for (uint32_t py = 0; py < TEX_SIZE; ++py)
            for (uint32_t px = 0; px < TEX_SIZE; ++px)
                pixels[py * TEX_SIZE + px] =
                    ((px / 32 + py / 32) & 1) ? 0xFF202020u : tint;
    };
    fillChecker(0xFF3080C0u);

    const valo::TextureID cpuTex = update
        ? valo.createTexture(TEX_SIZE, TEX_SIZE, pixels.data(), false, false)
        : valo::EMPTY_TEXTURE;

    Vertex quad[4] = {{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}};
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    const auto verts = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    const auto indices = valo.createIndexBuffer(idx, 6);

    valo::RenderCommand rect;
    rect.verts = verts; rect.indices = indices;
    rect.program = valo.createProgram("examples/shaders/roundrect.vert.hpp",
                                      "examples/shaders/roundrect.frag.hpp");
    rect.cullmode = valo::CullMode::None;
    rect.depthTest = false; rect.depthWrite = false;
    rect.blendmode = valo::BlendMode::Alpha;

    valo::RenderCommand blit;
    blit.verts = verts; blit.indices = indices;
    blit.program = valo.createProgram("examples/shaders/blit.vert.hpp",
                                      "examples/shaders/blit.frag.hpp");
    blit.cullmode = valo::CullMode::None;
    blit.depthTest = false; blit.depthWrite = false;

    float t = 0.f;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::S)) showTarget = !showTarget;
        t += 1.f / 60.f;

        const valo::Vec2u sz = window.getSize();
        const Mat4f projB = Mat4f::orthographic(0.f, float(sz.x), float(sz.y), 0.f, -1.f, 1.f);

        // a tracking target is window-sized, so its projection follows the window
        const float tw = track ? float(sz.x) : float(TARGET_SIZE);
        const float th = track ? float(sz.y) : float(TARGET_SIZE);
        const Mat4f projT = Mat4f::orthographic(0.f, tw, th, 0.f, -1.f, 1.f);

        if (update) {
            // a full refill every two seconds, a moving block every frame
            if ((int(t * 60.f) % 120) == 0) {
                fillChecker(int(t) % 4 < 2 ? 0xFF3080C0u : 0xFF20A060u);
                valo.updateTexture(cpuTex, pixels.data());
            }

            constexpr uint32_t BLOCK = 48;
            std::vector<uint32_t> block(BLOCK * BLOCK, 0xFF00D0FFu);
            const uint32_t bx = uint32_t((std::sin(t) * 0.5f + 0.5f) * float(TEX_SIZE - BLOCK));
            const uint32_t by = uint32_t((std::cos(t) * 0.5f + 0.5f) * float(TEX_SIZE - BLOCK));
            valo.updateTexture(cpuTex, bx, by, BLOCK, BLOCK, block.data());
        }

        valo.beginFrame();

        // into the target: three rects orbiting the centre
        for (int i = 0; i < (update ? 0 : 3); ++i) {
            const float a = t + float(i) * 2.094f;
            const Vec2f c = {tw * 0.5f + std::cos(a) * tw * 0.23f,
                             th * 0.5f + std::sin(a) * th * 0.23f};
            const Vec4f col = {i == 0 ? 1.f : 0.2f, i == 1 ? 1.f : 0.3f, i == 2 ? 1.f : 0.4f, 1.f};
            const float side = std::min(tw, th) * 0.27f;
            rect.setUniforms(Uniforms{projT, c - Vec2f{side * 0.5f, side * 0.5f},
                                      {side, side}, col, side * 0.26f});
            valo.submit(rect, offscreen);
        }

        // to the backbuffer: one quad sampling the target. Skipped when the
        // target is presented directly - there is nothing to compose it into.
        if (!showTarget) {
            const float pad = 60.f + std::sin(t) * 30.f;
            blit.setUniforms(BlitUniforms{projB, {pad, pad},
                {float(sz.x) - pad * 2.f, float(sz.y) - pad * 2.f},
                update ? cpuTex : target});
            valo.submit(blit, valo::VALO::BASE_LAYER);
        }

        valo.endFrame(showTarget ? target : valo::Renderer::BACKBUFFER);
    }

    valo.removeLayer(offscreen);
    if (update) valo.destroyTexture(cpuTex);
    valo.destroyTarget(target);
    valo.shutdown();
    return 0;
}
