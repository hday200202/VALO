// Point-topology demo: a spiral galaxy of 8000 stars, fully animated in the
// vertex shader from a `time` uniform (the vertex buffer is uploaded once and
// never touched again). Shows valo_PointSize, additive blending, and depthWrite=false.
#include <VALO.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace valo;

// Each star is static data: orbit radius, base angle, height, and a seed.
struct Star      { float radius; float angle; float height; float seed; };
struct GUniforms { Mat4f viewProj; Mat4f model; float time; };
struct GVaryings { Vec3f color; float glow; };

static constexpr float kGalaxyRadius = 3.5f;

void galaxyVS(const Star& in, const GUniforms& u, GVaryings& out) {
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
    valo_Position = clip;
}
void galaxyFS(const GVaryings& in, const GUniforms&) {
    // low alpha so additive blending needs several overlapping stars to saturate
    valo_FragColor = Color(in.color * in.glow, 0.35f);
}

static float frand(uint32_t& seed) {                         // cheap LCG, 0..1
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) * (1.f / 16777216.f);
}

int main() {
    Window window("VALO - particles", {1280, 960});

    VALO valo;
    RenderConfig cfg;
    cfg.vsync       = true;
    cfg.renderScale = 1.f;
    if (!valo.init(window, cfg)) return 1;

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
    galaxy.depthWrite = false;                              // stars don't occlude each other
    galaxy.blendmode  = BlendMode::Additive;                // overlapping stars add up instead of overwriting

    // simple fly camera: W/S forward/back, A/D strafe, Q/E down/up, ESC quit
    Vec3f camPos = {0.f, 2.5f, 7.f};
    const Vec3f camFwd = Vec3f{0.f, -0.35f, -1.f}.normalized();
    const Vec3f camRight = camFwd.cross({0.f, 1.f, 0.f}).normalized();
    uint64_t lastTicks = SDL_GetTicks();

    while (window.pollEvents()) {
        const uint64_t now = SDL_GetTicks();
        const float dt = (now - lastTicks) / 1000.f;
        lastTicks = now;
        if (window.wasKeyPressed(Key::Escape)) window.close();

        const float speed = 3.f * dt;
        if (window.isKeyDown(Key::W)) camPos += camFwd   * speed;
        if (window.isKeyDown(Key::S)) camPos -= camFwd   * speed;
        if (window.isKeyDown(Key::D)) camPos += camRight * speed;
        if (window.isKeyDown(Key::A)) camPos -= camRight * speed;
        if (window.isKeyDown(Key::E)) camPos.y += speed;
        if (window.isKeyDown(Key::Q)) camPos.y -= speed;
        const Mat4f view = Mat4f::lookAt(camPos, camPos + camFwd, {0.f, 1.f, 0.f});

        const Vec2u size   = window.getSize();
        const float aspect = float(size.x) / float(size.y);
        const Mat4f proj   = Mat4f::perspective(std::numbers::pi_v<float> / 3.f, aspect, 0.1f, 100.f);
        const float t      = SDL_GetTicks() / 1000.f;

        valo.beginFrame();

        // tilted disk, slowly precessing; everything else happens in the shader
        Mat4f galaxyModel = Mat4f::rotationY(t * 0.05f) * Mat4f::rotationX(0.55f);
        galaxy.setUniforms(GUniforms{proj * view, galaxyModel, t});
        valo.submit(galaxy);

        valo.endFrame();
    }

    return 0;
}
