#include <VALO.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace valo;

// ---- what a dev writes: their own vertex / uniform / varying types ----
struct Vertex   { Vec3f pos; Vec2f uv; };
struct Uniforms { Mat4f mvp; TextureID albedo; };
struct Varyings { Vec2f uv; };

Vec4f vs(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.uv = in.uv;
    return u.mvp * Vec4f(in.pos, 1.f);
}

Color fs(const Varyings& in, const Uniforms& u) {
    return texture(u.albedo, in.uv);
}

// 8x8 checkerboard with a red cell at uv (0,0) and a green one at uv (1,1),
// so a flipped or rotated UV mapping is obvious at a glance.
static std::vector<uint32_t> makeChecker(uint32_t size, uint32_t cells) {
    std::vector<uint32_t> px(size * size);
    const uint32_t cell = size / cells;
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            bool dark = ((x / cell) + (y / cell)) & 1;
            px[y * size + x] = colorToABGR32(dark ? Color(0.15f, 0.15f, 0.18f, 1.f)
                                                  : Color(0.9f,  0.9f,  0.85f, 1.f));
        }
    for (uint32_t i = 0; i < cell; ++i)
        for (uint32_t j = 0; j < cell; ++j) {
            px[j * size + i]                           = colorToABGR32({1.f, 0.f, 0.f, 1.f}); // top-left
            px[(size - 1 - j) * size + (size - 1 - i)] = colorToABGR32({0.f, 1.f, 0.f, 1.f}); // bottom-right
        }
    return px;
}

// ---- particles: a spiral galaxy, fully animated in the vertex shader ----
// Each star is static data: orbit radius, base angle, height, and a seed.
// The vertex shader computes its position from `time`, so the vertex buffer
// is uploaded once and never touched again.
struct Star      { float radius; float angle; float height; float seed; };
struct GUniforms { Mat4f viewProj; Mat4f model; float time; };
struct GVaryings { Vec3f color; float glow; };

static constexpr float kGalaxyRadius = 3.5f;

Vec4f galaxyVS(const Star& in, const GUniforms& u, GVaryings& out) {
    const float r = in.radius;
    // inner stars orbit faster (roughly Keplerian) -> arms wind up over time
    const float a = in.angle + u.time * 0.9f / std::sqrt(r + 0.25f);
    // thin disk, puffier toward the core
    const float y = in.height * (0.35f - 0.25f * (r / kGalaxyRadius));

    Vec3f pos  = {r * std::cos(a), y, r * std::sin(a)};
    Vec4f clip = u.viewProj * u.model * Vec4f(pos, 1.f);

    // color by radius: white-yellow core -> orange -> blue-violet rim
    const float f = r / kGalaxyRadius;
    Vec3f core = {1.0f, 0.95f, 0.8f};
    Vec3f mid  = {1.0f, 0.55f, 0.25f};
    Vec3f rim  = {0.35f, 0.45f, 1.0f};
    out.color  = f < 0.35f ? lerp(core, mid, f / 0.35f) : lerp(mid, rim, (f - 0.35f) / 0.65f);

    // twinkle: each star has its own phase
    out.glow = 0.55f + 0.45f * std::sin(u.time * 2.5f + in.seed * 6.2831853f);

    // point size shrinks with distance (clip.w == view depth), bigger near the core
    valo_PointSize = std::clamp((5.f + 4.f * (1.f - f)) / clip.w * 2.5f, 1.f, 14.f);
    return clip;
}
Color galaxyFS(const GVaryings& in, const GUniforms&) {
    return Color(in.color * in.glow, 1.f);
}

static float frand(uint32_t& seed) {                         // cheap LCG, 0..1
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) * (1.f / 16777216.f);
}

int main() {
    // dev-owned window so we can query its size for the aspect ratio
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = SDL_CreateWindow("VALO - cube", 320 * 4, 240 * 4, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) return 1;

    VALO valo;
    RenderConfig cfg;
    cfg.sdlWindow       = window;
    cfg.vsync           = false;
    cfg.renderScale     = 1.f;
    if (!valo.init(cfg)) return 1;

    // unit cube, 4 verts per face so each face gets a full 0..1 uv square.
    // uv (0,0) = image top-left. Faces are CCW from outside.
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
        const uint32_t b = f * 4;
        uint32_t* i = indices + f * 6;
        i[0] = b; i[1] = b + 1; i[2] = b + 2;
        i[3] = b; i[4] = b + 2; i[5] = b + 3;
    }

    std::vector<uint32_t> checker = makeChecker(256, 8);
    TextureID      tex  = valo.createTexture(256, 256, checker.data());

    VtxLayout      layout({VtxAttrib::Vec3f, VtxAttrib::Vec2f});
    VertexBufferID vb   = valo.createVertexBuffer(verts, 24, layout);
    IndexBufferID  ib   = valo.createIndexBuffer(indices, 36);
    ProgramID      prog = valo.createProgram(vs, fs);

    RenderCommand cube;
    cube.verts   = vb;
    cube.indices = ib;
    cube.program = prog;
    cube.cullmode = CullMode::Back;
    // cube.fillmode = FillMode::Wireframe;
    // cube.topology = Topology::Lines;

    // --- galaxy particle system ---
    constexpr uint32_t kStars = 8000;
    constexpr int      kArms  = 3;
    std::vector<Star>     stars(kStars);
    std::vector<uint32_t> sidx(kStars);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < kStars; ++i) {
        const float r    = std::sqrt(frand(seed)) * kGalaxyRadius;       // uniform over the disk
        const int   arm  = int(frand(seed) * kArms);
        const float armA = arm * 6.2831853f / kArms + r * 1.6f;           // logarithmic-ish twist
        const float scat = (frand(seed) - 0.5f) * (0.35f + 0.9f * (1.f - r / kGalaxyRadius));
        stars[i] = {r, armA + scat, (frand(seed) - 0.5f) * 2.f, frand(seed)};
        sidx[i]  = i;
    }

    VtxLayout      slayout({VtxAttrib::Float, VtxAttrib::Float, VtxAttrib::Float, VtxAttrib::Float});
    VertexBufferID svb   = valo.createVertexBuffer(stars.data(), kStars, slayout);
    IndexBufferID  sib   = valo.createIndexBuffer(sidx.data(), kStars);
    ProgramID      sprog = valo.createProgram(galaxyVS, galaxyFS);

    RenderCommand galaxy;
    galaxy.verts      = svb;
    galaxy.indices    = sib;
    galaxy.program    = sprog;
    galaxy.topology   = Topology::Points;
    galaxy.depthWrite = false;                              // stars test against the cubes but not each other

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

        static bool prevFM = false;
        bool fm = keys[SDL_SCANCODE_L];
        if (fm && !prevFM) {
            if (cube.fillmode == FillMode::Wireframe)
                cube.fillmode = FillMode::Solid;
            else cube.fillmode = FillMode::Wireframe;
        }
        prevFM = fm;
        const Mat4f view = Mat4f::lookAt(camPos, camPos + camFwd, {0.f, 1.f, 0.f});

        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const float aspect = float(w) / float(h);
        const Mat4f proj   = Mat4f::perspective(std::numbers::pi_v<float> / 3.f, aspect, 0.1f, 100.f);
        const float t      = SDL_GetTicks() / 1000.f;

        valo.beginFrame();

        // spinning cube at the origin
        Mat4f model = Mat4f::rotationY(t) * Mat4f::rotationX(t * 0.6f);
        cube.setUniforms(Uniforms{proj * view * model, tex});
        valo.submit(cube);

        // second cube: smaller, orbiting behind/around the first.
        // depth test + perspective decide who's in front as it passes.
        Mat4f model2 = 
            Mat4f::translation({2.5f * std::cos(t * 0.8f), 0.f, 2.5f * std::sin(t * 0.8f)}) * 
            Mat4f::rotationZ(-t) * 
            Mat4f::scale({0.5f, 0.5f, 0.5f});
        cube.setUniforms(Uniforms{proj * view * model2, tex});
        valo.submit(cube);

        // galaxy: tilted disk above the cubes, everything else happens in the shader
        Mat4f galaxyModel = Mat4f::translation({0.f, 2.6f, 0.f}) * Mat4f::rotationX(0.55f);
        galaxy.setUniforms(GUniforms{proj * view, galaxyModel, t});
        valo.submit(galaxy);

        valo.endFrame();
    }

    valo.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
