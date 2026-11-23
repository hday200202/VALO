// "Snail" by Inigo Quilez (Shadertoy, CC BY-NC-SA 3.0), ported to VALO's C++
// shaders. One fullscreen triangle; the fragment shader raymarches the snail,
// shell, leaf and water drops.
//
// Shadertoy's channel images aren't available, so the textures are generated:
// tileable fbm noise for iChannel1/2, white noise for iChannel3, and the
// background's 200-tap blur baked once into its own texture.
//
// Controls
//   1 - 4         render scale 1, 0.5, 0.25, 0.125
//   a             antialiasing off / 2x2 supersampling (default on for Vulkan)
//   space         pause
//   escape        quit
//
// Flags
//   --vulkan      run on the Vulkan backend
//   --validation  Vulkan validation layers
//   --time T      start T seconds in
//   --paused      start paused

#include <VALO.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "shaders/snail.vert.hpp"
#include "shaders/snail.frag.hpp"

namespace {

constexpr int NOISE_SIZE = 256;
constexpr int GRAIN_SIZE = 64;
constexpr int SKY_SIZE   = 256;
constexpr int SKY_LOD    = 16;      // the original samples the background at mip 4

float hashLattice(const int x, const int y, const int seed) {
    uint32_t n = uint32_t(x) * 1597334677u ^ uint32_t(y) * 3812015801u ^ uint32_t(seed) * 2654435769u;
    n = n * 0x27d4eb2du;
    n = n ^ (n >> 15u);
    return float(n >> 8u) * (1.f / 16777216.f);
}

// value noise wrapped at period, so the texture tiles
float valueNoise(const float x, const float y, const int period, const int seed) {
    const int   ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - float(ix),      fy = y - float(iy);
    const float ux = fx * fx * (3.f - 2.f * fx);
    const float uy = fy * fy * (3.f - 2.f * fy);

    auto at = [&](const int cx, const int cy) {
        return hashLattice(((cx % period) + period) % period, ((cy % period) + period) % period, seed);
    };

    const float a = at(ix, iy),     b = at(ix + 1, iy);
    const float c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

float fbm(const float s, const float t, const int seed) {
    float sum = 0.f, amp = 0.5f, norm = 0.f;
    int   period = 8;
    for (int o = 0; o < 5; ++o) {
        sum    += amp * valueNoise(s * float(period), t * float(period), period, seed + o * 17);
        norm   += amp;
        amp    *= 0.5f;
        period *= 2;
    }
    return std::clamp((sum / norm - 0.5f) * 1.8f + 0.5f, 0.f, 1.f);
}

std::vector<valo::Color> makeNoise() {
    std::vector<valo::Color> img(NOISE_SIZE * NOISE_SIZE);
    for (int y = 0; y < NOISE_SIZE; ++y)
        for (int x = 0; x < NOISE_SIZE; ++x) {
            const float s = (float(x) + 0.5f) / NOISE_SIZE, t = (float(y) + 0.5f) / NOISE_SIZE;
            img[y * NOISE_SIZE + x] = {fbm(s, t, 1), fbm(s, t, 101), fbm(s, t, 201), fbm(s, t, 301)};
        }
    return img;
}

std::vector<valo::Color> makeGrain() {
    std::vector<valo::Color> img(GRAIN_SIZE * GRAIN_SIZE);
    for (int y = 0; y < GRAIN_SIZE; ++y)
        for (int x = 0; x < GRAIN_SIZE; ++x)
            img[y * GRAIN_SIZE + x] = {hashLattice(x, y, 7), hashLattice(x, y, 8), hashLattice(x, y, 9), hashLattice(x, y, 10)};
    return img;
}

/*
    background() blurs iChannel2 at mip 4 with 200 smooth-max taps around
    uv * 0.25. That depends on uv alone, so it is done here once: box-filter the
    noise down to SKY_LOD texels, then bake the blur at every texel of the sky.
*/
std::vector<valo::Color> makeSky(const std::vector<valo::Color>& noise) {
    const int cell = NOISE_SIZE / SKY_LOD;
    std::vector<valo::Vec3f> low(SKY_LOD * SKY_LOD);
    for (int y = 0; y < SKY_LOD; ++y)
        for (int x = 0; x < SKY_LOD; ++x) {
            valo::Vec3f sum;
            for (int j = 0; j < cell; ++j)
                for (int i = 0; i < cell; ++i)
                    sum += noise[(y * cell + j) * NOISE_SIZE + x * cell + i].xyz;
            low[y * SKY_LOD + x] = sum / float(cell * cell);
        }

    auto sampleLow = [&](const float s, const float t) {
        const float fx = s * SKY_LOD - 0.5f, fy = t * SKY_LOD - 0.5f;
        const int   ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float ax = fx - float(ix),      ay = fy - float(iy);
        auto at = [&](const int cx, const int cy) {
            return low[(((cy % SKY_LOD) + SKY_LOD) % SKY_LOD) * SKY_LOD + ((cx % SKY_LOD) + SKY_LOD) % SKY_LOD];
        };
        const valo::Vec3f top = at(ix, iy)     + (at(ix + 1, iy)     - at(ix, iy))     * ax;
        const valo::Vec3f bot = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * ax;
        return top + (bot - top) * ay;
    };

    auto smax = [](const float a, const float b, const float k) {
        const float h = std::max(k - std::abs(a - b), 0.f);
        return std::max(a, b) + h * h * 0.25f / k;
    };

    std::vector<valo::Color> img(SKY_SIZE * SKY_SIZE);
    for (int y = 0; y < SKY_SIZE; ++y)
        for (int x = 0; x < SKY_SIZE; ++x) {
            const float s = (float(x) + 0.5f) / SKY_SIZE, t = (float(y) + 0.5f) / SKY_SIZE;
            valo::Vec3f col;
            for (int i = 0; i < 200; ++i) {
                const float h  = float(i) / 200.f;
                const float an = 31.f * 6.2831f * h;
                const valo::Vec3f tmp = sampleLow(s + 0.0075f * h * std::cos(an), t + 0.0075f * h * std::sin(an));
                col = {smax(col.x, tmp.y, 0.5f), smax(col.y, tmp.x, 0.5f), smax(col.z, tmp.z, 0.5f)};
            }
            img[y * SKY_SIZE + x] = {std::min(col.x, 1.f), std::min(col.y, 1.f), std::min(col.z, 1.f), 1.f};
        }
    return img;
}

valo::TextureID upload(valo::VALO& valo, const std::vector<valo::Color>& img, const int size) {
    std::vector<uint32_t> px(img.size());
    for (size_t i = 0; i < img.size(); ++i) px[i] = valo::colorToABGR32(img[i]);
    return valo.createTexture(size, size, px.data(), false, false);
}

} // namespace

int main(int argc, char** argv) {
    bool  useVulkan = false, validation = false;
    float time      = 0.f;
    bool  paused    = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--vulkan"))     useVulkan  = true;
        if (!std::strcmp(argv[i], "--validation")) validation = true;
        if (!std::strcmp(argv[i], "--time") && i + 1 < argc) time = float(std::atof(argv[++i]));
        if (!std::strcmp(argv[i], "--paused")) paused = true;
    }

    valo::Window window("VALO - snail", {1280, 720},
        valo::WinFlags::Resize | valo::WinFlags::HiDPI
      | (useVulkan ? valo::WinFlags::Vulkan : valo::WinFlags::None));

    valo::RenderConfig cfg;
    cfg.vsync      = true;
    cfg.validation = validation;
    if (useVulkan) cfg.backend = valo::Backend::Vulkan;

    valo::VALO valo;
    if (!valo.init(window, cfg)) return 1;
    valo.setRenderScale(useVulkan ? 1.f : 0.25f);

    const auto noise = makeNoise();
    const valo::TextureID noiseTex = upload(valo, noise, NOISE_SIZE);
    const valo::TextureID grainTex = upload(valo, makeGrain(), GRAIN_SIZE);
    const valo::TextureID skyTex   = upload(valo, makeSky(noise), SKY_SIZE);

    snail::SnailVertex tri[3] = {{{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};
    uint32_t idx[3] = {0, 1, 2};

    valo::RenderCommand cmd;
    cmd.verts     = valo.createVertexBuffer(tri, 3, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    cmd.indices   = valo.createIndexBuffer(idx, 3);
    cmd.program   = valo.createProgram("examples/shaders/snail.vert.hpp", "examples/shaders/snail.frag.hpp");
    cmd.depthTest = false;
    cmd.cullmode  = valo::CullMode::None;

    using clk = std::chrono::steady_clock;
    auto  fpsStart  = clk::now();
    auto  lastFrame = clk::now();
    int   fpsFrames = 0;
    int   aa        = useVulkan ? 2 : 1;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::Space))  paused = !paused;
        if (window.wasKeyPressed(valo::Key::Num1))   valo.setRenderScale(1.f);
        if (window.wasKeyPressed(valo::Key::Num2))   valo.setRenderScale(0.5f);
        if (window.wasKeyPressed(valo::Key::Num3))   valo.setRenderScale(0.25f);
        if (window.wasKeyPressed(valo::Key::Num4))   valo.setRenderScale(0.125f);

        if (window.wasKeyPressed(valo::Key::A))      aa = aa == 1 ? 2 : 1;

        // wall clock rather than getDeltaTime(), which caps a frame at 0.1 s
        const auto now = clk::now();
        if (!paused) time += std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;

        ++fpsFrames;
        const float elapsed = std::chrono::duration<float>(clk::now() - fpsStart).count();
        if (elapsed >= 1.f) {
            window.setTitle("VALO - snail   " + std::to_string(int(fpsFrames / elapsed + 0.5f)) + " fps   scale "
                          + std::to_string(valo.getRenderScale()).substr(0, 5) + "   aa " + std::to_string(aa * aa) + "x");
            fpsStart  = clk::now();
            fpsFrames = 0;
        }

        // the resolution actually rendered, so the subpixel offsets are a pixel wide
        const valo::Vec2u size  = window.getSize();
        const float       scale = window.getPixelDensity() * valo.getRenderScale();
        cmd.setUniforms(snail::SnailUniforms{time, float(size.x) * scale, float(size.y) * scale, aa, noiseTex, grainTex, skyTex});

        valo.beginFrame();
        valo.submit(cmd);
        valo.endFrame();
    }

    valo.shutdown();
    return 0;
}
