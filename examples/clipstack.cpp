// Clip stack demo, UI style.
//
// Everything is 2D in pixel space (orthographic, y down, no depth). A window
// panel contains a scrolling list; each list row is itself a rounded panel
// with a text-like bar that's wider than the row. Three nested clips:
//
//   pushClip(window)            - rounded rect, the outer frame
//     pushClip(list viewport)   - the scroll area inside the window
//       pushClip(row)           - each row, so its content can't spill out
//         row content
//       popClip()
//     popClip()
//   popClip()
//
// Rounded rects are a quad + an SDF fragment shader that `discard`s outside
// the shape - the same program is used for the clip shapes and for the visible
// panels, only colorWrite differs (VALO sets that for clips).
//
// Controls
//   wheel         scroll the list
//   c             toggle clipping (see what it's doing)
//   escape        quit

#include <VALO.hpp>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>
#include <algorithm>

#include "shaders/roundrect.vert.hpp"
#include "shaders/roundrect.frag.hpp"

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Mat4f;
using valo::Color;

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // one scene, either backend: the software path resolves the shader
    // paths to registered C++ callbacks, the Vulkan path translates them
    bool useVulkan = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--vulkan")) useVulkan = true;

    valo::Window window("VALO - clip stack", {1280, 960},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));
    valo::VALO valo; valo::RenderConfig cfg; cfg.vsync = true;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;
    valo.setLayerClearColor(valo::VALO::BASE_LAYER, {0.12f, 0.12f, 0.14f, 1.f});

    Vertex quad[4] = {{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}; uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    valo::RenderCommand rect;
    rect.verts     = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    rect.indices   = valo.createIndexBuffer(idx, 6);
    rect.program   = valo.createProgram("shaders/roundrect.vert.hpp", "shaders/roundrect.frag.hpp");
    rect.cullmode  = valo::CullMode::None;
    rect.depthTest = false; rect.depthWrite = false;
    rect.blendmode = valo::BlendMode::Alpha;

    // draw helper: place the shared quad at pos/size with a color and corner radius
    auto draw = [&](Vec2f pos, Vec2f size, Vec4f color, float radius, const Mat4f& proj) {
        rect.setUniforms(Uniforms{proj, pos, size, color, radius});
        valo.submit(rect);
    };
    auto clip = [&](Vec2f pos, Vec2f size, float radius, const Mat4f& proj) {
        rect.setUniforms(Uniforms{proj, pos, size, {0, 0, 0, 0}, radius});
        valo.pushClip(rect);
    };

    float scroll = 0.f; bool clipping = true;
    using clk = std::chrono::steady_clock; auto fpsStart = clk::now(); uint32_t fpsFrames = 0; int fps = 0;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::C)) clipping = !clipping;
        if (window.wasKeyPressed(valo::Key::V)) valo.setVsync(!valo.getVsync());
        scroll = std::clamp(scroll - window.scrollDelta() * 24.f, 0.f, 900.f);

        ++fpsFrames; const auto now = clk::now();
        if (std::chrono::duration<float>(now - fpsStart).count() >= 1.f) { fps = int(fpsFrames); fpsFrames = 0; fpsStart = now; }
        window.setTitle(std::string("VALO - clip stack   clipping ") + (clipping ? "on" : "off") + "   " + std::to_string(fps) + " fps");

        const valo::Vec2u sz = window.getSize();
        // pixel space, y down: (0,0) top-left, (w,h) bottom-right
        const Mat4f proj = Mat4f::orthographic(0.f, float(sz.x), float(sz.y), 0.f, -1.f, 1.f);

        const Vec2f winPos  = {float(sz.x) * 0.5f - 300.f, 120.f};
        const Vec2f winSize = {600.f, float(sz.y) - 240.f};
        const Vec2f listPos = winPos + Vec2f{20.f, 70.f};
        const Vec2f listSize = winSize - Vec2f{40.f, 90.f};

        valo.beginFrame();

        // --- window frame (the outer clip) ---
        draw(winPos, winSize, {0.20f, 0.21f, 0.25f, 1.f}, 24.f, proj);
        if (clipping) clip(winPos, winSize, 24.f, proj);

        // title bar: deliberately wider than the window - the outer clip trims it
        draw(winPos - Vec2f{60.f, 0.f}, {winSize.x + 120.f, 50.f}, {0.30f, 0.32f, 0.40f, 1.f}, 0.f, proj);

        // --- list viewport (nested clip) ---
        draw(listPos, listSize, {0.15f, 0.16f, 0.19f, 1.f}, 12.f, proj);
        if (clipping) clip(listPos, listSize, 12.f, proj);

        for (int i = 0; i < 20; ++i) {
            const float rowH = 64.f, gap = 10.f;
            const Vec2f rowPos  = listPos + Vec2f{12.f, 12.f + i * (rowH + gap) - scroll};
            const Vec2f rowSize = {listSize.x - 24.f, rowH};
            if (rowPos.y + rowH < listPos.y || rowPos.y > listPos.y + listSize.y) continue;   // trivially off-screen

            const float hue = i / 20.f;
            const Vec4f rowCol = {0.25f + 0.25f * std::sin(hue * 6.28f), 0.35f, 0.45f + 0.2f * std::cos(hue * 6.28f), 1.f};
            draw(rowPos, rowSize, rowCol, 16.f, proj);
            if (clipping) clip(rowPos, rowSize, 16.f, proj);

            // row content: a "text" bar and a badge that both overflow the row's rounded corners
            draw(rowPos + Vec2f{-30.f, 22.f}, {rowSize.x * 0.7f + 60.f, 20.f}, {0.9f, 0.9f, 0.9f, 0.85f}, 10.f, proj);
            draw(rowPos + Vec2f{rowSize.x - 40.f, -12.f}, {70.f, 40.f}, {0.95f, 0.45f, 0.30f, 1.f}, 20.f, proj);

            if (clipping) valo.popClip();
        }

        if (clipping) valo.popClip();   // list
        if (clipping) valo.popClip();   // window

        // something outside every clip, to prove the stack is fully unwound
        draw({30.f, float(sz.y) - 70.f}, {220.f, 40.f}, {0.4f, 0.75f, 0.45f, 1.f}, 20.f, proj);

        valo.endFrame();
    }
    return 0;
}
