// Window input and platform queries.
//
// Every event is printed as it arrives: keys with their layout symbol and
// modifiers, typed text, buttons, wheel (precise or notched), focus. Four
// regions set the cursor shape on hover, and the frame is redrawn from the
// resize callback so it keeps up during a drag-resize.
//
// Controls
//   tab           toggle text input
//   ctrl+c        copy a test string to the clipboard
//   ctrl+v        print the clipboard
//   escape        quit
//
// Flags
//   --vulkan      run on the Vulkan backend
//   --moves       print mouse motion too

#include <VALO.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include "shaders/roundrect.vert.hpp"
#include "shaders/roundrect.frag.hpp"

using valo::Vec2f;
using valo::Vec4f;
using valo::Mat4f;

static std::string utf8(const char32_t c) {
    std::string out;
    if (c < 0x80)         out += char(c);
    else if (c < 0x800)   { out += char(0xC0 | (c >> 6));  out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else                  { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    return out;
}

static std::string mods(const valo::KeyMod m) {
    std::string out;
    if (any(m & valo::KeyMod::Ctrl))  out += "ctrl+";
    if (any(m & valo::KeyMod::Shift)) out += "shift+";
    if (any(m & valo::KeyMod::Alt))   out += "alt+";
    if (any(m & valo::KeyMod::Gui))   out += "gui+";
    return out;
}

int main(int argc, char** argv) {
    bool useVulkan = false, moves = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--vulkan")) useVulkan = true;
        if (!std::strcmp(argv[i], "--moves"))  moves     = true;
    }

    valo::Window window("VALO - input", {960, 640},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    valo::VALO valo; valo::RenderConfig cfg; cfg.vsync = true;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;
    if (!valo.init(window, cfg)) return 1;
    valo.setLayerClearColor(valo::VALO::BASE_LAYER, {0.12f, 0.12f, 0.14f, 1.f});

    const char* themes[] = {"unknown", "light", "dark"};
    const valo::Vec2u monitor = window.getMonitorSize();
    std::printf("display scale %.2f, pixel density %.2f, %.0f Hz, monitor %ux%u, theme %s\n",
        window.getDisplayScale(), window.getPixelDensity(), window.getRefreshRate(),
        monitor.x, monitor.y, themes[int(valo::Window::getSystemTheme())]);
    std::printf("native handle %p\nbase path %s\npref path %s\n",
        window.nativeHandle(), valo::Window::basePath().c_str(),
        valo::Window::prefPath("VALO", "input").c_str());

    Vertex quad[4] = {{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}; uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    valo::RenderCommand rect;
    rect.verts     = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    rect.indices   = valo.createIndexBuffer(idx, 6);
    rect.program   = valo.createProgram("examples/shaders/roundrect.vert.hpp", "examples/shaders/roundrect.frag.hpp");
    rect.cullmode  = valo::CullMode::None;
    rect.depthTest = false; rect.depthWrite = false;
    rect.blendmode = valo::BlendMode::Alpha;

    struct Region { valo::CursorShape shape; Vec4f color; const char* name; };
    const Region regions[4] = {
        {valo::CursorShape::Hand,      {0.30f, 0.45f, 0.75f, 1.f}, "hand"},
        {valo::CursorShape::Text,      {0.35f, 0.65f, 0.40f, 1.f}, "text"},
        {valo::CursorShape::ResizeEW,  {0.75f, 0.45f, 0.30f, 1.f}, "resize-ew"},
        {valo::CursorShape::Crosshair, {0.60f, 0.35f, 0.65f, 1.f}, "crosshair"},
    };

    auto regionAt = [&](const Vec2f p, const valo::Vec2u sz) -> int {
        const float w = float(sz.x) / 4.f;
        if (p.y < 40.f || p.y > float(sz.y) - 40.f) return -1;
        const int i = int(p.x / w);
        return (i >= 0 && i < 4) ? i : -1;
    };

    int hovered = -2;
    uint32_t redraws = 0;

    auto drawFrame = [&]() {
        const valo::Vec2u sz = window.getSize();
        const Mat4f proj = Mat4f::orthographic(0.f, float(sz.x), float(sz.y), 0.f, -1.f, 1.f);
        const float w = float(sz.x) / 4.f;

        valo.beginFrame();
        for (int i = 0; i < 4; ++i) {
            Vec4f c = regions[i].color;
            if (i == hovered) c = {c.x * 1.3f, c.y * 1.3f, c.z * 1.3f, 1.f};
            rect.setUniforms(Uniforms{proj, {w * float(i) + 8.f, 40.f}, {w - 16.f, float(sz.y) - 80.f}, c, 18.f});
            valo.submit(rect);
        }
        valo.endFrame();
    };

    window.setResizeCallback([&]() {
        ++redraws;
        const valo::Vec2u sz = window.getSize();
        std::printf("resize callback #%u  %ux%u\n", redraws, sz.x, sz.y);
        drawFrame();
    });

    window.startTextInput();
    std::printf("text input %s\n", window.isTextInputActive() ? "on" : "off");

    uint32_t copies = 0;
    while (window.pollEvents()) {
        for (const valo::WindowEvent& e : window.events()) {
            switch (e.type) {
                case valo::WindowEventType::KeyDown:
                case valo::WindowEventType::KeyUp:
                    std::printf("%s key %u symbol '%s' (U+%04X) %s%s at %.0f,%.0f  t=%llu\n",
                        e.type == valo::WindowEventType::KeyDown ? "down" : "up  ",
                        unsigned(e.key), e.symbol ? utf8(e.symbol).c_str() : "", unsigned(e.symbol),
                        mods(e.mods).c_str(), e.repeat ? " repeat" : "",
                        e.mouse.x, e.mouse.y, (unsigned long long)e.timestamp);
                    break;
                case valo::WindowEventType::Text:
                    std::printf("text \"%s\"\n", e.text);
                    break;
                case valo::WindowEventType::MouseDown:
                case valo::WindowEventType::MouseUp:
                    std::printf("mouse %s button %u x%u at %.0f,%.0f %s\n",
                        e.type == valo::WindowEventType::MouseDown ? "down" : "up",
                        unsigned(e.button), unsigned(e.clicks), e.mouse.x, e.mouse.y, mods(e.mods).c_str());
                    break;
                case valo::WindowEventType::MouseMove:
                    if (moves) std::printf("move %.0f,%.0f\n", e.mouse.x, e.mouse.y);
                    break;
                case valo::WindowEventType::Wheel:
                    std::printf("wheel %.3f,%.3f %s %sat %.0f,%.0f\n",
                        e.wheel.x, e.wheel.y, e.precise ? "precise" : "notched", mods(e.mods).c_str(),
                        e.mouse.x, e.mouse.y);
                    break;
                case valo::WindowEventType::FocusGained:    std::printf("focus gained\n"); break;
                case valo::WindowEventType::FocusLost:      std::printf("focus lost\n"); break;
                case valo::WindowEventType::CloseRequested: std::printf("close requested\n"); break;
            }

            if (e.type != valo::WindowEventType::KeyDown || e.repeat) continue;

            if (e.key == valo::Key::Escape) window.close();

            if (e.key == valo::Key::Tab) {
                if (window.isTextInputActive()) window.stopTextInput();
                else                            window.startTextInput();
                std::printf("text input %s\n", window.isTextInputActive() ? "on" : "off");
            }

            if (any(e.mods & valo::KeyMod::Ctrl) && e.symbol == U'c') {
                const std::string text = "VALO clipboard " + std::to_string(++copies) + " \xC3\xA9\xC3\xBC \xE6\x97\xA5\xE6\x9C\xAC";
                window.setClipboardText(text);
                std::printf("copied \"%s\"\n", text.c_str());
            }

            if (any(e.mods & valo::KeyMod::Ctrl) && e.symbol == U'v')
                std::printf("pasted \"%s\"\n", window.getClipboardText().c_str());
        }

        const int now = regionAt(window.mousePosition(), window.getSize());
        if (now != hovered) {
            hovered = now;
            window.setCursor(now >= 0 ? regions[now].shape : valo::CursorShape::Arrow);
            std::printf("cursor %s\n", now >= 0 ? regions[now].name : "arrow");
        }

        drawFrame();
        std::fflush(stdout);
    }

    return 0;
}
