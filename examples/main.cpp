#include <VALO.hpp>
#include <cmath>
#include <numbers>

using namespace valo;

// ---- what a dev writes: their own vertex / uniform / varying types ----
struct Vertex   { Vec3f pos; Vec3f color; };
struct Uniforms { Mat4f mvp; };
struct Varyings { Vec3f color; };

Vec4f vs(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.color = in.color;
    return u.mvp * Vec4f(in.pos, 1.f);
}

Color fs(const Varyings& in, const Uniforms&) {
    return Color(in.color, 1.f);
}

int main() {
    // dev-owned window so we can query its size for the aspect ratio
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = SDL_CreateWindow("VALO - cube", 320 * 2, 240 * 2, SDL_WINDOW_RESIZABLE);
    if (!window) return 1;

    VALO valo;
    RenderConfig cfg;
    cfg.sdlWindow = window;
    cfg.vsync     = true;
    if (!valo.init(cfg)) return 1;

    // unit cube, one color per corner
    Vertex verts[8] = {
        {{-1, -1, -1}, {0, 0, 0}},  // 0
        {{ 1, -1, -1}, {1, 0, 0}},  // 1
        {{ 1,  1, -1}, {1, 1, 0}},  // 2
        {{-1,  1, -1}, {0, 1, 0}},  // 3
        {{-1, -1,  1}, {0, 0, 1}},  // 4
        {{ 1, -1,  1}, {1, 0, 1}},  // 5
        {{ 1,  1,  1}, {1, 1, 1}},  // 6
        {{-1,  1,  1}, {0, 1, 1}},  // 7
    };

    // counter-clockwise as seen from outside = front face (Vulkan/OpenGL default)
    uint32_t indices[36] = {
        4, 5, 6,  4, 6, 7,   // front  (+z)
        1, 0, 3,  1, 3, 2,   // back   (-z)
        5, 1, 2,  5, 2, 6,   // right  (+x)
        0, 4, 7,  0, 7, 3,   // left   (-x)
        7, 6, 2,  7, 2, 3,   // top    (+y)
        0, 1, 5,  0, 5, 4,   // bottom (-y)
    };

    VtxLayout      layout({VtxAttrib::Vec3f, VtxAttrib::Vec3f});
    VertexBufferID vb   = valo.createVertexBuffer(verts, 8, layout);
    IndexBufferID  ib   = valo.createIndexBuffer(indices, 36);
    ProgramID      prog = valo.createProgram(vs, fs);

    RenderCommand cube;
    cube.verts   = vb;
    cube.indices = ib;
    cube.program = prog;
    cube.cullmode = CullMode::Back;

    // simple fly camera: W/S forward/back, A/D strafe, Q/E down/up, ESC quit
    Vec3f camPos = {0.f, 1.5f, 6.f};
    const Vec3f camFwd = Vec3f{0.f, -0.25f, -1.f}.normalized();
    const Vec3f camRight = camFwd.cross({0.f, 1.f, 0.f}).normalized();
    uint64_t lastTicks = SDL_GetTicks();

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: valo.resize(); break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) running = false;
                    break;
                default: break;
            }
        }

        // held keys -> camera movement (frame-rate independent)
        const uint64_t now = SDL_GetTicks();
        const float dt = (now - lastTicks) / 1000.f;
        lastTicks = now;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const float speed = 3.f * dt;
        if (keys[SDL_SCANCODE_W]) camPos += camFwd   * speed;
        if (keys[SDL_SCANCODE_S]) camPos -= camFwd   * speed;
        if (keys[SDL_SCANCODE_D]) camPos += camRight * speed;
        if (keys[SDL_SCANCODE_A]) camPos -= camRight * speed;
        if (keys[SDL_SCANCODE_E]) camPos.y += speed;
        if (keys[SDL_SCANCODE_Q]) camPos.y -= speed;
        const Mat4f view = Mat4f::lookAt(camPos, camPos + camFwd, {0.f, 1.f, 0.f});

        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const float aspect = float(w) / float(h);
        const Mat4f proj   = Mat4f::perspective(std::numbers::pi_v<float> / 3.f, aspect, 0.1f, 100.f);
        const float t      = SDL_GetTicks() / 1000.f;

        valo.beginFrame();

        // spinning cube at the origin
        Mat4f model = Mat4f::rotationY(t) * Mat4f::rotationX(t * 0.6f);
        cube.setUniforms(Uniforms{proj * view * model});
        valo.submit(cube);

        // second cube: smaller, orbiting behind/around the first.
        // depth test + perspective decide who's in front as it passes.
        Mat4f model2 = 
            Mat4f::translation({2.5f * std::cos(t * 0.8f), 0.f, 2.5f * std::sin(t * 0.8f)}) * 
            Mat4f::rotationZ(-t) * 
            Mat4f::scale({0.5f, 0.5f, 0.5f});
        cube.setUniforms(Uniforms{proj * view * model2});
        valo.submit(cube);

        valo.endFrame();
    }

    valo.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
