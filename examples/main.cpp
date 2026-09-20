#include <VALO.hpp>
#include <numbers>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <string>

struct Vertex   { valo::Vec3f pos; valo::Vec2f uv; };
struct Uniforms { valo::Mat4f mvp; valo::TextureID tex; };
struct Varyings { valo::Vec2f uv; };

valo::Vec4f vs(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.uv = in.uv * 0.01f;
    return u.mvp * valo::Vec4f(in.pos, 1.f);
}

valo::Color fs(const Varyings& in, const Uniforms& u) {
    return texture(
        u.tex, in.uv, 
        valo::Sampler(
            valo::Wrap::Repeat, 
            valo::Wrap::Repeat, 
            valo::Filter::Linear
        )
    );
}

int main() {
    valo::Window window("VALO", {1920, 1080});

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = true;
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
    valo::ProgramID      prog   = valo.createProgram(vs, fs);

    valo::RenderCommand cube;
    cube.verts   = vb;
    cube.indices = ib;
    cube.program = prog;
    // cube.cullmode = valo::CullMode::None;
    // cube.fillmode = valo::FillMode::Wireframe;
    // cube.indexCount = 18;
    // cube.firstIndex = 9;

    const valo::Mat4f view = valo::Mat4f::lookAt({0.f, 1.5f, 5.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    float yaw = 0.f, pitch = 0.f, zoom = 1.f;

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
            const bool upsideDown = std::cos(pitch) < 0.f;
            window.setMousePositionLocal(window.getCenterLocal());
            yaw   += d.x * 0.01f * (upsideDown ? -1.f : 1.f);
            pitch += d.y * 0.01f;
        }

        zoom = std::clamp(zoom - window.scrollDelta() * 0.1f, 0.3f, 3.f);

        const valo::Vec2u size = window.getSize();
        if (!ortho) {
            const valo::Mat4f proj = valo::Mat4f::perspective(std::numbers::pi_v<float> / 3.f, float(size.x) / float(size.y), 0.1f, 100.f);
            const valo::Mat4f model = valo::Mat4f::rotationX(pitch) * valo::Mat4f::rotationY(yaw) * valo::Mat4f::scale({zoom, zoom, zoom});

            valo.beginFrame();
            cube.setUniforms(Uniforms{proj * view * model, texToggle ? rust : dirt});
            valo.submit(cube);
            valo.endFrame();
        }
        else {
            const float aspect = window.getAspect();
            const float h = 2.5f;
            const valo::Mat4f proj = valo::Mat4f::orthographic(-h * aspect, h * aspect, -h, h, 0.1f, 100.f);
            const valo::Mat4f model = valo::Mat4f::rotationX(pitch) * valo::Mat4f::rotationY(yaw) * valo::Mat4f::scale({zoom, zoom, zoom});
            
            valo.beginFrame();
            cube.setUniforms(Uniforms{proj * view * model, texToggle ? rust : dirt});
            valo.submit(cube);
            valo.endFrame();
        }
    }

    return 0;
}
