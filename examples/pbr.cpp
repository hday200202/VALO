// PBR material viewer - a port of cool-pbr-test to VALO.
//
// Cook-Torrance metallic/roughness over a table of 62 materials, value-noise
// fbm bump mapping (sampled in object space so it sticks to the surface),
// optional vertex displacement, four view-space lights and a sky/sun
// environment used both as the background and the reflection term.
//
// The original carried its own rasterizer, background buffer, SDL window and
// event loop; VALO replaces all of that, so what's left is materials, shaders
// and mesh builders.
//
// Controls
//   left click    grab mouse and drag to rotate; right click releases
//   c             cube / sphere
//   [ ] / wheel   previous / next material
//   up down       bump strength        (hold)
//   left right    noise scale          (hold)
//   pgup pgdn     roughness variation  (hold)
//   d             procedural detail on / off
//   g             displace geometry along the height field
//   1 2 3 4       render scale 100% / 75% / 50% / 25%
//   r             reset orientation and parameters
//   escape        quit

#include <VALO.hpp>
#include <cmath>
#include <numbers>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Mat4f;
using valo::Color;

constexpr float kPi         = std::numbers::pi_v<float>;
constexpr float kCameraDist = 4.2f;
constexpr float kFovY       = 0.9f;
constexpr float kMouseSpeed = 0.005f;

constexpr float kDefaultBump      = 0.60f;
constexpr float kDefaultNoise     = 6.00f;
constexpr float kDefaultVariation = 0.30f;
constexpr float kDisplaceScale    = 0.16f;

constexpr int kSphereStacks = 96, kSphereSlices = 128, kCubeSegments = 40;

static inline float saturate(float x) { return std::min(std::max(x, 0.f), 1.f); }
static inline float clampf(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }

// ---------------------------------------------------------------------------
// materials: albedo is linear; for metals it is F0, not a diffuse colour
// ---------------------------------------------------------------------------
struct Material { const char* name; Vec3f albedo; float metallic, roughness; };

constexpr Material kMaterials[] = {
    { "Gold",             { 1.000f, 0.766f, 0.336f }, 1.0f, 0.22f },
    { "Polished brass",   { 0.910f, 0.778f, 0.423f }, 1.0f, 0.15f },
    { "Weathered brass",  { 0.780f, 0.600f, 0.280f }, 1.0f, 0.62f },
    { "Bronze",           { 0.714f, 0.428f, 0.181f }, 1.0f, 0.40f },
    { "Copper",           { 0.955f, 0.638f, 0.538f }, 1.0f, 0.28f },
    { "Silver",           { 0.972f, 0.960f, 0.915f }, 1.0f, 0.12f },
    { "Platinum",         { 0.672f, 0.637f, 0.585f }, 1.0f, 0.20f },
    { "Aluminium",        { 0.913f, 0.922f, 0.924f }, 1.0f, 0.18f },
    { "Chrome",           { 0.550f, 0.556f, 0.554f }, 1.0f, 0.05f },
    { "Nickel",           { 0.660f, 0.609f, 0.526f }, 1.0f, 0.30f },
    { "Titanium",         { 0.542f, 0.497f, 0.449f }, 1.0f, 0.35f },
    { "Anodised titanium",{ 0.300f, 0.420f, 0.720f }, 1.0f, 0.25f },
    { "Cobalt",           { 0.662f, 0.655f, 0.634f }, 1.0f, 0.40f },
    { "Zinc",             { 0.664f, 0.824f, 0.850f }, 1.0f, 0.45f },
    { "Lead",             { 0.632f, 0.626f, 0.641f }, 1.0f, 0.55f },
    { "Damascus steel",   { 0.480f, 0.490f, 0.500f }, 1.0f, 0.38f },
    { "Brushed steel",    { 0.560f, 0.570f, 0.580f }, 1.0f, 0.55f },
    { "Gunmetal",         { 0.220f, 0.230f, 0.250f }, 1.0f, 0.45f },
    { "Cast iron",        { 0.290f, 0.290f, 0.300f }, 1.0f, 0.70f },
    { "Oxidised iron",    { 0.300f, 0.250f, 0.220f }, 0.9f, 0.85f },
    { "Rusted steel",     { 0.280f, 0.150f, 0.090f }, 0.7f, 0.92f },
    { "Verdigris copper", { 0.180f, 0.400f, 0.330f }, 0.6f, 0.80f },
    { "Red plastic",      { 0.640f, 0.030f, 0.030f }, 0.0f, 0.28f },
    { "White plastic",    { 0.850f, 0.850f, 0.850f }, 0.0f, 0.35f },
    { "Black plastic",    { 0.020f, 0.020f, 0.020f }, 0.0f, 0.30f },
    { "Blue car paint",   { 0.020f, 0.055f, 0.260f }, 0.0f, 0.12f },
    { "Red car paint",    { 0.330f, 0.012f, 0.020f }, 0.0f, 0.10f },
    { "Matte vinyl",      { 0.150f, 0.160f, 0.170f }, 0.0f, 0.72f },
    { "Rubber",           { 0.022f, 0.022f, 0.024f }, 0.0f, 0.92f },
    { "Silicone",         { 0.560f, 0.540f, 0.560f }, 0.0f, 0.58f },
    { "Porcelain",        { 0.880f, 0.870f, 0.840f }, 0.0f, 0.08f },
    { "Glazed ceramic",   { 0.620f, 0.180f, 0.120f }, 0.0f, 0.12f },
    { "Unglazed ceramic", { 0.700f, 0.620f, 0.540f }, 0.0f, 0.68f },
    { "Terracotta",       { 0.520f, 0.250f, 0.150f }, 0.0f, 0.82f },
    { "Obsidian",         { 0.020f, 0.020f, 0.025f }, 0.0f, 0.07f },
    { "Ice",              { 0.750f, 0.830f, 0.880f }, 0.0f, 0.10f },
    { "Pearl",            { 0.880f, 0.860f, 0.820f }, 0.0f, 0.16f },
    { "Wax",              { 0.850f, 0.820f, 0.700f }, 0.0f, 0.42f },
    { "Marble",           { 0.780f, 0.770f, 0.740f }, 0.0f, 0.20f },
    { "Granite",          { 0.290f, 0.280f, 0.270f }, 0.0f, 0.75f },
    { "Slate",            { 0.130f, 0.140f, 0.150f }, 0.0f, 0.70f },
    { "Sandstone",        { 0.560f, 0.450f, 0.320f }, 0.0f, 0.88f },
    { "Rough concrete",   { 0.330f, 0.320f, 0.300f }, 0.0f, 0.95f },
    { "Asphalt",          { 0.055f, 0.055f, 0.060f }, 0.0f, 0.90f },
    { "Chalk",            { 0.880f, 0.880f, 0.860f }, 0.0f, 0.97f },
    { "Jade",             { 0.055f, 0.310f, 0.160f }, 0.0f, 0.14f },
    { "Lapis lazuli",     { 0.070f, 0.130f, 0.480f }, 0.0f, 0.22f },
    { "Malachite",        { 0.060f, 0.330f, 0.240f }, 0.0f, 0.26f },
    { "Turquoise",        { 0.100f, 0.520f, 0.520f }, 0.0f, 0.25f },
    { "Amber",            { 0.600f, 0.320f, 0.060f }, 0.0f, 0.18f },
    { "Varnished wood",   { 0.190f, 0.085f, 0.030f }, 0.0f, 0.35f },
    { "Raw oak",          { 0.320f, 0.210f, 0.110f }, 0.0f, 0.78f },
    { "Ebony",            { 0.045f, 0.035f, 0.030f }, 0.0f, 0.45f },
    { "Cork",             { 0.430f, 0.300f, 0.170f }, 0.0f, 0.85f },
    { "Leather",          { 0.230f, 0.130f, 0.080f }, 0.0f, 0.65f },
    { "Bone",             { 0.820f, 0.790f, 0.700f }, 0.0f, 0.55f },
    { "Velvet",           { 0.180f, 0.030f, 0.060f }, 0.0f, 0.96f },
    { "Denim",            { 0.110f, 0.180f, 0.320f }, 0.0f, 0.85f },
    { "Canvas",           { 0.560f, 0.510f, 0.420f }, 0.0f, 0.90f },
    { "Charcoal",         { 0.018f, 0.018f, 0.018f }, 0.0f, 0.88f },
    { "Snow",             { 0.900f, 0.910f, 0.950f }, 0.0f, 0.60f },
    { "Moss",             { 0.090f, 0.200f, 0.070f }, 0.0f, 0.93f },
};
constexpr int kMaterialCount = int(sizeof(kMaterials) / sizeof(kMaterials[0]));

// ---------------------------------------------------------------------------
// lights (view space: camera at origin looking down -z, object at z = -4.2)
// ---------------------------------------------------------------------------
struct Light { Vec3f viewPos; Vec3f colour; float intensity; };
constexpr Light kLights[4] = {
    { { -3.5f,  3.5f, -0.7f }, { 1.00f, 0.96f, 0.90f }, 52.0f },  // key
    { {  4.0f,  0.5f, -1.7f }, { 0.55f, 0.68f, 1.00f }, 26.0f },  // cool fill
    { { -1.5f, -3.0f, -2.2f }, { 1.00f, 0.55f, 0.32f }, 16.0f },  // warm bounce
    { {  1.0f,  1.5f, -8.2f }, { 0.85f, 0.88f, 1.00f }, 34.0f },  // rim
};

// ---------------------------------------------------------------------------
// environment: sky gradient + a sun along the key light
// ---------------------------------------------------------------------------
constexpr Vec3f kSkyTop     = { 0.040f, 0.140f, 0.500f };  // saturated deep blue
constexpr Vec3f kSkyHorizon = { 0.380f, 0.500f, 0.700f };  // lighter blue, less haze
constexpr Vec3f kSkyGround  = { 0.160f, 0.105f, 0.060f };  // warmer, sandier earth
constexpr float kSkyFalloff = 0.50f;
constexpr Vec3f kSunColour  = { 1.000f, 0.920f, 0.780f };  // a touch warmer
static const Vec3f kSunDir  = Vec3f{ -0.45f, 0.75f, 0.48f }.normalized();

static Vec3f skyColour(const Vec3f& d) {
    const float y = clampf(d.y, -1.f, 1.f);
    return y >= 0.f ? lerp(kSkyHorizon, kSkyTop,    std::pow( y, kSkyFalloff))
                    : lerp(kSkyHorizon, kSkyGround, std::pow(-y, kSkyFalloff));
}

static Vec3f environmentColour(const Vec3f& d) {
    const float toSun = saturate(d.normalized().dot(kSunDir));
    const float disc  = std::pow(toSun, 900.f) * 14.f;
    const float haze  = std::pow(toSun,  12.f) * 0.30f;
    return skyColour(d) + kSunColour * (disc + haze);
}

static const Vec3f kAmbientUp   = skyColour({0.f,  1.f, 0.f});
static const Vec3f kAmbientDown = skyColour({0.f, -1.f, 0.f});

/* ===========================================================================
    Value noise and fbm

    Value noise: hash each integer lattice point to a random number, then
    interpolate smoothly between the eight corners of the cell a point falls in.
    fbm sums several octaves at doubling frequency and halving amplitude, which
    is what turns a soft blob field into something that reads as surface detail.
=========================================================================== */

// A cheap integer hash. No randomness needed beyond "the same input always gives
// the same unpredictable-looking output", which is what makes the noise stable
// frame to frame.
static inline float hash3(int x, int y, int z) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(z) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h) * (1.f / 4294967296.f);
}
// The classic 3t^2 - 2t^3 ease. Interpolating the lattice linearly would leave
// visible creases at every cell boundary; this makes the first derivative match.
static inline float smoothstepf(float t) { return t * t * (3.f - 2.f * t); }

static float valueNoise(const Vec3f& p) {
    // which lattice cell, and how far into it
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);

    const int ix = int(fx);
    const int iy = int(fy);
    const int iz = int(fz);

    const float tx = smoothstepf(p.x - fx);
    const float ty = smoothstepf(p.y - fy);
    const float tz = smoothstepf(p.z - fz);

    // the cell's eight corners
    const float c000 = hash3(ix,     iy,     iz    );
    const float c100 = hash3(ix + 1, iy,     iz    );
    const float c010 = hash3(ix,     iy + 1, iz    );
    const float c110 = hash3(ix + 1, iy + 1, iz    );
    const float c001 = hash3(ix,     iy,     iz + 1);
    const float c101 = hash3(ix + 1, iy,     iz + 1);
    const float c011 = hash3(ix,     iy + 1, iz + 1);
    const float c111 = hash3(ix + 1, iy + 1, iz + 1);

    // trilinear: collapse along x, then y, then z
    const float x00 = c000 + (c100 - c000) * tx;
    const float x10 = c010 + (c110 - c010) * tx;
    const float x01 = c001 + (c101 - c001) * tx;
    const float x11 = c011 + (c111 - c011) * tx;

    const float y0 = x00 + (x10 - x00) * ty;
    const float y1 = x01 + (x11 - x01) * ty;

    return y0 + (y1 - y0) * tz;
}

static float fbm(Vec3f p, int octaves) {
    float sum   = 0.f;
    float amp   = 0.5f;
    float total = 0.f;                  // running amplitude, to normalise at the end

    for (int i = 0; i < octaves; ++i) {
        sum   += valueNoise(p) * amp;
        total += amp;

        // 2.02 rather than 2: an exact doubling lines every octave's lattice up
        // on the same integer grid, which shows as a regular grid artefact
        p    = p * 2.02f;
        amp *= 0.5f;
    }

    return total > 0.f ? sum / total : 0.f;
}

static inline float heightField(const Vec3f& objPos, float scale) { return fbm(objPos * scale, 4); }

/* ===========================================================================
    Cook-Torrance BRDF

    The specular term is  D * G * F / (4 * nDotL * nDotV), where
      D  how much of the surface's microfacets point at the half vector
      G  how much those microfacets shadow and mask each other
      F  how reflective the surface is at this viewing angle (Fresnel)
=========================================================================== */

// Trowbridge-Reitz / GGX. r is perceptual roughness, so it is squared once to
// get the material's alpha and again inside the formula - hence r^4.
static float distributionGGX(float nDotH, float r) {
    const float a2 = r * r * r * r;
    const float d  = nDotH * nDotH * (a2 - 1.f) + 1.f;
    return a2 / std::max(kPi * d * d, 1e-7f);
}
// Schlick-GGX geometry term, one direction at a time; the caller multiplies the
// light and view directions together (Smith's method).
static float geometrySchlick(float nDotX, float r) {
    const float k = (r + 1.f) * (r + 1.f) / 8.f;
    return nDotX / std::max(nDotX * (1.f - k) + k, 1e-7f);
}
// Fresnel: reflectivity climbs toward 1 at grazing angles. f0 is the head-on
// reflectance - 0.04 for dielectrics, the albedo itself for metals.
static Vec3f fresnelSchlick(float cosTheta, const Vec3f& f0) {
    const float f = std::pow(1.f - saturate(cosTheta), 5.f);
    return f0 + (Vec3f{1.f, 1.f, 1.f} - f0) * f;
}
// Same curve with the ceiling pulled down by roughness, for the environment
// reflection - a rough surface should not mirror at the silhouette.
static Vec3f fresnelSchlickRoughness(float cosTheta, const Vec3f& f0, float r) {
    const float f = std::pow(1.f - saturate(cosTheta), 5.f);
    const float invR = 1.f - r;
    const Vec3f maxR = {std::max(invR, f0.x), std::max(invR, f0.y), std::max(invR, f0.z)};
    return f0 + (maxR - f0) * f;
}
// Reinhard (v / (1 + v)) to pull HDR into 0..1, then gamma for display.
static Vec3f toneMap(const Vec3f& c) {
    auto enc = [](float v) { return std::pow(saturate(v / (1.f + v)), 1.f / 2.2f); };
    return {enc(c.x), enc(c.y), enc(c.z)};
}

// ---------------------------------------------------------------------------
// shaders
// ---------------------------------------------------------------------------
struct Vertex { Vec3f pos; Vec3f normal; Vec2f uv; };

/*
    Camera and lights are the same for every draw in the frame, so they live in
    a uniform buffer bound to a slot rather than being memcpy'd into each
    command. A uniform buffer is read when the queue executes at endFrame, so
    only frame-constant values belong in one - per-draw state stays in Uniforms.
*/
struct SceneUniforms {
    Vec3f camPos;
    Vec3f lightPos[4];           // world space
};

constexpr uint32_t kSceneSlot = 0;

struct Uniforms {
    Mat4f mvp;
    Mat4f model;                 // pure rotation, doubles as the normal matrix
    Vec3f albedo; float metallic, roughness;
    float bump, noiseScale, variation;
    bool  detail, displace;
};

struct Varyings { Vec3f objPos; Vec3f objNormal; Vec3f worldPos; };

static void vs(const Vertex& in, const Uniforms& u, Varyings& out) {
    Vec3f pos = in.pos;
    if (u.displace && u.detail && u.bump > 0.f) {
        const float h = heightField(in.pos, u.noiseScale) - 0.5f;
        pos += in.normal * (h * u.bump * kDisplaceScale);
    }
    // varyings carry the UNdisplaced object position: that's the domain the
    // height field is sampled in, so the bump normals agree with the geometry
    out.objPos    = in.pos;
    out.objNormal = in.normal;
    out.worldPos  = (u.model * Vec4f(pos, 1.f)).xyz;
    valo::valo_Position = u.mvp * Vec4f(pos, 1.f);
}

static void fs(const Varyings& in, const Uniforms& u) {
    // --- surface detail, in object space ---
    const Vec3f objN = in.objNormal.normalized();
    Vec3f n;
    float roughOff = 0.f, metalOff = 0.f;

    if (!u.detail || u.bump <= 0.f) {
        n = (u.model * Vec4f(objN, 0.f)).xyz.normalized();
    } else {
        const Vec3f p = in.objPos;
        const float s = u.noiseScale, eps = 0.35f / std::max(s, 0.001f);
        const Vec3f grad = Vec3f{
            heightField({p.x + eps, p.y, p.z}, s) - heightField({p.x - eps, p.y, p.z}, s),
            heightField({p.x, p.y + eps, p.z}, s) - heightField({p.x, p.y - eps, p.z}, s),
            heightField({p.x, p.y, p.z + eps}, s) - heightField({p.x, p.y, p.z - eps}, s),
        } * (1.f / (2.f * eps));
        const Vec3f tangential = grad - objN * grad.dot(objN);
        const Vec3f bumped     = (objN - tangential * (u.bump * 0.13f)).normalized();
        n = (u.model * Vec4f(bumped, 0.f)).xyz.normalized();

        if (u.variation > 0.f) {
            const float v = fbm(p * (s * 0.45f) + Vec3f{17.3f, 4.1f, 9.7f}, 3);
            roughOff = (v - 0.5f) * u.variation;
            metalOff = (v - 0.5f) * u.variation * 0.35f;
        }
    }

    // --- Cook-Torrance over four lights ---
    const SceneUniforms& scene = valo::uniformBuffer<SceneUniforms>(kSceneSlot);

    const Vec3f v = (scene.camPos - in.worldPos).normalized();
    const float nDotV = std::max(n.dot(v), 1e-4f);
    const float roughness = clampf(u.roughness + roughOff, 0.035f, 1.f);
    const float metallic  = clampf(u.metallic  + metalOff, 0.f, 1.f);
    const Vec3f f0 = lerp(Vec3f{0.04f, 0.04f, 0.04f}, u.albedo, metallic);

    Vec3f radiance = {0.f, 0.f, 0.f};
    for (int i = 0; i < 4; ++i) {
        const Vec3f toLight = scene.lightPos[i] - in.worldPos;
        const float dist = toLight.length();
        if (dist < 1e-5f) continue;
        const Vec3f l = toLight * (1.f / dist);
        const float nDotL = n.dot(l);
        if (nDotL <= 0.f) continue;

        const Vec3f h = (l + v).normalized();
        const float nDotH = std::max(n.dot(h), 0.f), vDotH = std::max(v.dot(h), 0.f);
        const float d = distributionGGX(nDotH, roughness);
        const float g = geometrySchlick(nDotL, roughness) * geometrySchlick(nDotV, roughness);
        const Vec3f f = fresnelSchlick(vDotH, f0);
        const Vec3f specular = f * (d * g / std::max(4.f * nDotL * nDotV, 1e-5f));
        const Vec3f diffuse  = u.albedo * ((1.f / kPi) * (1.f - f.x) * (1.f - metallic));
        const Vec3f incoming = kLights[i].colour * (kLights[i].intensity / (dist * dist));
        radiance += (diffuse + specular) * incoming * nDotL;
    }

    // --- environment: weak diffuse ambient, real specular reflection ---
    const Vec3f irradiance     = lerp(kAmbientDown, kAmbientUp, n.y * 0.5f + 0.5f);
    const Vec3f ambientDiffuse = u.albedo * irradiance * ((1.f - metallic) * 0.15f);
    const Vec3f refl           = (n * (2.f * n.dot(v)) - v).normalized();
    const Vec3f ambientSpec    = environmentColour(refl) * fresnelSchlickRoughness(nDotV, f0, roughness) * (1.f - roughness * 0.7f);

    valo::valo_FragColor = {toneMap(radiance + ambientDiffuse + ambientSpec), 1.f};
}

// sky: one fullscreen triangle, ray per pixel from the fixed camera
struct SkyVertex   { Vec2f ndc; };
struct SkyUniforms { float aspect, tanHalf; };
struct SkyVaryings { Vec2f ndc; };

static void skyVs(const SkyVertex& in, const SkyUniforms&, SkyVaryings& out) {
    out.ndc = in.ndc;
    valo::valo_Position = {in.ndc.x, in.ndc.y, 0.9999f, 1.f};   // just inside the far plane
}
static void skyFs(const SkyVaryings& in, const SkyUniforms& u) {
    const Vec3f dir = Vec3f{in.ndc.x * u.tanHalf * u.aspect, in.ndc.y * u.tanHalf, -1.f}.normalized();
    valo::valo_FragColor = {toneMap(environmentColour(dir)), 1.f};
}

// ---------------------------------------------------------------------------
// meshes
// ---------------------------------------------------------------------------
struct Mesh { std::vector<Vertex> verts; std::vector<uint32_t> idx; };

/*
    Each face is a seg x seg grid rather than two triangles, so the displacement
    in the vertex shader has vertices to actually move. Every vertex on a face
    carries that face's flat normal, which is what keeps the cube's edges sharp -
    a shared, averaged normal would round them off.
*/
static Mesh buildCube(float size, int seg) {
    const float h = size * 0.5f;

    // each face as an origin corner plus two edge directions, so one loop builds
    // all six without six special cases
    struct Face { Vec3f origin, right, up, normal; };

    const Face faces[6] = {
        { {-h, -h,  h}, { 1, 0,  0}, {0, 1,  0}, { 0,  0,  1} },   // +z
        { { h, -h, -h}, {-1, 0,  0}, {0, 1,  0}, { 0,  0, -1} },   // -z
        { {-h, -h, -h}, { 0, 0,  1}, {0, 1,  0}, {-1,  0,  0} },   // -x
        { { h, -h,  h}, { 0, 0, -1}, {0, 1,  0}, { 1,  0,  0} },   // +x
        { {-h,  h,  h}, { 1, 0,  0}, {0, 0, -1}, { 0,  1,  0} },   // +y
        { {-h, -h, -h}, { 1, 0,  0}, {0, 0,  1}, { 0, -1,  0} },   // -y
    };

    Mesh m;
    const int stride = seg + 1;      // vertices per row, not quads

    for (const Face& f : faces) {
        const uint32_t base = uint32_t(m.verts.size());

        for (int j = 0; j <= seg; ++j) {
            for (int i = 0; i <= seg; ++i) {
                const float u = float(i) / seg;
                const float v = float(j) / seg;

                m.verts.push_back({
                    f.origin + f.right * (u * size) + f.up * (v * size),
                    f.normal,
                    {u, v},
                });
            }
        }

        for (int j = 0; j < seg; ++j) {
            for (int i = 0; i < seg; ++i) {
                const uint32_t v00 = base + j * stride + i;
                const uint32_t v10 = v00 + 1;
                const uint32_t v01 = v00 + stride;
                const uint32_t v11 = v01 + 1;

                m.idx.insert(m.idx.end(), {v00, v10, v11,  v00, v11, v01});
            }
        }
    }

    return m;
}

/*
    A UV sphere: stacks run pole to pole (phi), slices run around (theta). For a
    unit sphere the position and the normal are the same vector, which is why
    `unit` is used for both.

    The seam column is duplicated (j goes to <= slices) because the vertex at
    u = 0 and the one at u = 1 are in the same place but need different UVs.
*/
static Mesh buildSphere(float radius, int stacks, int slices) {
    Mesh m;

    for (int i = 0; i <= stacks; ++i) {
        const float v   = float(i) / stacks;
        const float phi = v * kPi;                  // 0 at +y, pi at -y

        for (int j = 0; j <= slices; ++j) {
            const float u     = float(j) / slices;
            const float theta = u * 2.f * kPi;

            const Vec3f unit = {
                std::sin(phi) * std::cos(theta),
                std::cos(phi),
                std::sin(phi) * std::sin(theta),
            };

            m.verts.push_back({unit * radius, unit, {u, v}});
        }
    }

    const uint32_t stride = slices + 1;

    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t tl = i * stride + j;
            const uint32_t tr = tl + 1;
            const uint32_t bl = tl + stride;
            const uint32_t br = bl + 1;

            m.idx.insert(m.idx.end(), {tl, br, bl,  tl, tr, br});
        }
    }

    return m;
}

// ---------------------------------------------------------------------------
int main() {
    valo::Window window("VALO - PBR", {1280, 960});
    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = true;
    if (!valo.init(window, cfg)) return 1;

    valo::VtxLayout layout({valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec2f});
    const Mesh cube   = buildCube(2.f, kCubeSegments);
    const Mesh sphere = buildSphere(1.25f, kSphereStacks, kSphereSlices);
    const valo::VertexBufferID cubeVb = valo.createVertexBuffer(cube.verts.data(), cube.verts.size(), layout);
    const valo::IndexBufferID  cubeIb = valo.createIndexBuffer(cube.idx.data(), cube.idx.size());
    const valo::VertexBufferID sphVb  = valo.createVertexBuffer(sphere.verts.data(), sphere.verts.size(), layout);
    const valo::IndexBufferID  sphIb  = valo.createIndexBuffer(sphere.idx.data(), sphere.idx.size());

    SceneUniforms scene{};
    const valo::UniformBufferID sceneBuffer = valo.createUniformBuffer(&scene, sizeof(scene));

    valo::RenderCommand obj;
    obj.program = valo.createProgram(vs, fs);
    obj.bindUniformBuffer(kSceneSlot, sceneBuffer);

    SkyVertex quad[4] = {{{-1.f, -1.f}}, {{1.f, -1.f}}, {{1.f, 1.f}}, {{-1.f, 1.f}}};
    uint32_t quadIdx[6] = {0, 1, 2, 0, 2, 3};
    valo::RenderCommand sky;
    sky.verts      = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    sky.indices    = valo.createIndexBuffer(quadIdx, 6);
    sky.program    = valo.createProgram(skyVs, skyFs);
    sky.depthTest  = false;
    sky.depthWrite = false;
    sky.cullmode   = valo::CullMode::None;

    // fixed camera; the object rotates
    const Vec3f camPos = {0.f, 0.f, kCameraDist};
    const Mat4f view = Mat4f::lookAt(camPos, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    const Mat4f viewInv = view.inverse();

    Mat4f orientation;
    int   materialIndex = 0;

    bool  useSphere = false;
    bool  detail    = true;
    bool  displace  = false;

    float bump       = kDefaultBump;
    float noiseScale = kDefaultNoise;
    float variation  = kDefaultVariation;

    using clk = std::chrono::steady_clock;
    auto     last      = clk::now();
    auto     fpsStart  = last;
    uint32_t fpsFrames = 0;
    int      fps       = 0;

    while (window.pollEvents()) {
        // clamped so one slow frame cannot make a held key jump the value
        const auto  now = clk::now();
        const float dt  = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        /* --- input: view --- */
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasMousePressed(valo::MouseButton::Left))  window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();
        if (window.isMouseGrabbed()) {
            // pre-multiplied, so each drag rotates about the SCREEN axes rather
            // than the object's own - which is what dragging a thing feels like
            const Vec2f d = window.mouseDelta();
            orientation = Mat4f::rotationX(d.y * kMouseSpeed)
                        * Mat4f::rotationY(d.x * kMouseSpeed)
                        * orientation;
        }

        /* --- input: what to draw --- */
        if (window.wasKeyPressed(valo::Key::C)) useSphere = !useSphere;
        if (window.wasKeyPressed(valo::Key::D)) detail    = !detail;
        if (window.wasKeyPressed(valo::Key::G)) displace  = !displace;

        // wheel or brackets step through the material table; adding
        // kMaterialCount before the modulo keeps -1 from going negative
        int step = int(window.scrollDelta() > 0.f) - int(window.scrollDelta() < 0.f);
        if (window.wasKeyPressed(valo::Key::RightBracket)) step =  1;
        if (window.wasKeyPressed(valo::Key::LeftBracket))  step = -1;
        materialIndex = (materialIndex + kMaterialCount + step) % kMaterialCount;

        /* --- input: held keys, scaled by dt so they tune at the same rate
               whatever the frame rate --- */
        if (window.isKeyDown(valo::Key::Up))       bump       = clampf(bump + 1.0f * dt, 0.f, 4.f);
        if (window.isKeyDown(valo::Key::Down))     bump       = clampf(bump - 1.0f * dt, 0.f, 4.f);
        if (window.isKeyDown(valo::Key::Right))    noiseScale = clampf(noiseScale + 6.f * dt, 0.5f, 48.f);
        if (window.isKeyDown(valo::Key::Left))     noiseScale = clampf(noiseScale - 6.f * dt, 0.5f, 48.f);
        if (window.isKeyDown(valo::Key::PageUp))   variation  = clampf(variation + 0.35f * dt, 0.f, 1.f);
        if (window.isKeyDown(valo::Key::PageDown)) variation  = clampf(variation - 0.35f * dt, 0.f, 1.f);
        if (window.wasKeyPressed(valo::Key::Num1)) valo.setRenderScale(1.00f);
        if (window.wasKeyPressed(valo::Key::Num2)) valo.setRenderScale(0.75f);
        if (window.wasKeyPressed(valo::Key::Num3)) valo.setRenderScale(0.50f);
        if (window.wasKeyPressed(valo::Key::Num4)) valo.setRenderScale(0.25f);

        if (window.wasKeyPressed(valo::Key::R)) {
            orientation = Mat4f();
            detail      = true;
            displace    = false;
            bump        = kDefaultBump;
            noiseScale  = kDefaultNoise;
            variation   = kDefaultVariation;
        }

        /* --- title --- */
        ++fpsFrames;
        if (std::chrono::duration<float>(now - fpsStart).count() >= 1.f) {
            fps       = int(fpsFrames);
            fpsFrames = 0;
            fpsStart  = now;
        }

        const Material& mat = kMaterials[materialIndex];
        window.setTitle(
            std::string("VALO - PBR   ") + mat.name +
            "   bump "  + std::to_string(bump).substr(0, 4) +
            "  scale "  + std::to_string(noiseScale).substr(0, 4) +
            "  var "    + std::to_string(variation).substr(0, 4) +
            (detail   ? "" : "  [detail off]") +
            (displace ? "  [displace]" : "") +
            "   " + std::to_string(fps) + " fps"
        );

        /* --- camera and per-frame uniforms --- */
        const float aspect = window.getAspect();
        const Mat4f proj   = Mat4f::perspective(kFovY, aspect, 0.1f, 100.f);

        // the lights are authored in view space so the rig stays put as the
        // object turns; viewInv puts them back into the world space the shader
        // does its distance maths in
        scene.camPos = camPos;
        for (int i = 0; i < 4; ++i) {
            scene.lightPos[i] = (viewInv * Vec4f(kLights[i].viewPos, 1.f)).xyz;
        }
        valo.updateUniformBuffer(sceneBuffer, &scene, sizeof(scene));

        /* --- per-draw uniforms ---

            The object rotates and the camera does not, so `orientation` is the
            whole model matrix. It is a pure rotation, which is why the shader
            can reuse it to transform normals - a matrix with scale in it would
            need the inverse transpose instead.
        */
        Uniforms u;
        u.mvp   = proj * view * orientation;
        u.model = orientation;

        u.albedo    = mat.albedo;
        u.metallic  = mat.metallic;
        u.roughness = mat.roughness;

        u.bump       = bump;
        u.noiseScale = noiseScale;
        u.variation  = variation;
        u.detail     = detail;
        u.displace   = displace;

        /* --- draw --- */
        obj.verts   = useSphere ? sphVb : cubeVb;
        obj.indices = useSphere ? sphIb : cubeIb;
        obj.setUniforms(u);

        sky.setUniforms(SkyUniforms{aspect, std::tan(kFovY * 0.5f)});

        valo.beginFrame();
        valo.submit(sky);       // first: depth test off, so it never occludes
        valo.submit(obj);
        valo.endFrame();
    }

    return 0;
}
