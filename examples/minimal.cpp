#include <VALO.hpp>
#include <iostream>

using namespace valo;

// Vertex
struct Vertex {
    Vertex(
        const Vec3f& pos, 
        const Color& color, 
        const Vec3f& normal, 
        const Vec2f& uv
    ) : pos(pos), color(color), normal(normal), uv(uv) {}

    Vec3f pos; 
    Color color; 
    Vec3f normal; 
    Vec2f uv;

    static const VtxLayout layout() {
        return {{
            VtxAttrib::Vec3f, 
            VtxAttrib::Vec4f, 
            VtxAttrib::Vec3f, 
            VtxAttrib::Vec2f
        }};
    }
};

struct Uniforms {};
struct Varyings { Vec2f uv; };

void vertShader(const Vertex& in, const Uniforms& u, Varyings& out) {
    // transform verts
    out.uv = in.uv;
    valo_Position = {in.pos, 1.f};
}

void fragShader(const Varyings& in, const Uniforms& u) {
    // output color based on where the pixel is located
    valo_FragColor = {in.uv.x, in.uv.y, 0.5, 1.f};
}

int main() {
    Window window("Minimal", {800, 600});
    VALO valo;

    valo.init(window);

    Vertex fullscreen[4] = {
        {{-1,  1,  0}, {1, 1, 1, 1}, {1, 1, 1}, {0, 0}},
        {{ 1,  1,  0}, {1, 1, 1, 1}, {1, 1, 1}, {1, 0}},
        {{-1, -1,  0}, {1, 1, 1, 1}, {1, 1, 1}, {0, 1}},
        {{ 1, -1,  0}, {1, 1, 1, 1}, {1, 1, 1}, {1, 1}}
    };

    uint32_t fullscreenIndices[6] = {
        0, 1, 2,
        1, 3, 2 
    };

    auto verts = valo.createVertexBuffer(fullscreen, 4, Vertex::layout());
    auto indcs = valo.createIndexBuffer(fullscreenIndices, 6);
    auto prgrm = valo.createProgram(vertShader, fragShader);

    RenderCommand cmd;
    cmd.verts = verts;
    cmd.indices = indcs;
    cmd.program = prgrm;
    cmd.winding = VtxWinding::CW;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(Key::C)) std::cout << "C is down" << std::endl;

        valo.beginFrame();
        // submit RenderCommands
        valo.submit(cmd);
        valo.endFrame();
    }
}