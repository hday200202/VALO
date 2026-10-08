#pragma once

#include <render/ShaderRegistry.hpp>

#include <cmath>
#include <numbers>
#include <algorithm>

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Mat4f;
using valo::Color;

constexpr float PI         = std::numbers::pi_v<float>;
constexpr float CAMERA_DIST = 4.2f;
constexpr float FOV_Y       = 0.9f;
constexpr float MOUSE_SPEED = 0.005f;

constexpr float DEFAULT_BUMP      = 0.60f;
constexpr float DEFAULT_NOISE     = 6.00f;
constexpr float DEFAULT_VARIATION = 0.30f;
constexpr float DISPLACE_SCALE    = 0.16f;

constexpr int SPHERE_STACKS = 96, SPHERE_SLICES = 128, CUBE_SEGMENTS = 40;

static inline float saturate(float x) { return std::min(std::max(x, 0.f), 1.f); }
static inline float clampf(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }

// ---------------------------------------------------------------------------
// materials: albedo is linear; for metals it is F0, not a diffuse colour
// ---------------------------------------------------------------------------
// no name field: a const char* has no GLSL type, and the label is the
// example's business rather than the shader's - see MATERIAL_NAMES in pbr.cpp
struct Material { Vec3f albedo; float metallic, roughness; };

constexpr Material MATERIALS[] = {
    { { 1.000f, 0.766f, 0.336f }, 1.0f, 0.22f },
    { { 0.910f, 0.778f, 0.423f }, 1.0f, 0.15f },
    { { 0.780f, 0.600f, 0.280f }, 1.0f, 0.62f },
    { { 0.714f, 0.428f, 0.181f }, 1.0f, 0.40f },
    { { 0.955f, 0.638f, 0.538f }, 1.0f, 0.28f },
    { { 0.972f, 0.960f, 0.915f }, 1.0f, 0.12f },
    { { 0.672f, 0.637f, 0.585f }, 1.0f, 0.20f },
    { { 0.913f, 0.922f, 0.924f }, 1.0f, 0.18f },
    { { 0.550f, 0.556f, 0.554f }, 1.0f, 0.05f },
    { { 0.660f, 0.609f, 0.526f }, 1.0f, 0.30f },
    { { 0.542f, 0.497f, 0.449f }, 1.0f, 0.35f },
    { { 0.300f, 0.420f, 0.720f }, 1.0f, 0.25f },
    { { 0.662f, 0.655f, 0.634f }, 1.0f, 0.40f },
    { { 0.664f, 0.824f, 0.850f }, 1.0f, 0.45f },
    { { 0.632f, 0.626f, 0.641f }, 1.0f, 0.55f },
    { { 0.480f, 0.490f, 0.500f }, 1.0f, 0.38f },
    { { 0.560f, 0.570f, 0.580f }, 1.0f, 0.55f },
    { { 0.220f, 0.230f, 0.250f }, 1.0f, 0.45f },
    { { 0.290f, 0.290f, 0.300f }, 1.0f, 0.70f },
    { { 0.300f, 0.250f, 0.220f }, 0.9f, 0.85f },
    { { 0.280f, 0.150f, 0.090f }, 0.7f, 0.92f },
    { { 0.180f, 0.400f, 0.330f }, 0.6f, 0.80f },
    { { 0.640f, 0.030f, 0.030f }, 0.0f, 0.28f },
    { { 0.850f, 0.850f, 0.850f }, 0.0f, 0.35f },
    { { 0.020f, 0.020f, 0.020f }, 0.0f, 0.30f },
    { { 0.020f, 0.055f, 0.260f }, 0.0f, 0.12f },
    { { 0.330f, 0.012f, 0.020f }, 0.0f, 0.10f },
    { { 0.150f, 0.160f, 0.170f }, 0.0f, 0.72f },
    { { 0.022f, 0.022f, 0.024f }, 0.0f, 0.92f },
    { { 0.560f, 0.540f, 0.560f }, 0.0f, 0.58f },
    { { 0.880f, 0.870f, 0.840f }, 0.0f, 0.08f },
    { { 0.620f, 0.180f, 0.120f }, 0.0f, 0.12f },
    { { 0.700f, 0.620f, 0.540f }, 0.0f, 0.68f },
    { { 0.520f, 0.250f, 0.150f }, 0.0f, 0.82f },
    { { 0.020f, 0.020f, 0.025f }, 0.0f, 0.07f },
    { { 0.750f, 0.830f, 0.880f }, 0.0f, 0.10f },
    { { 0.880f, 0.860f, 0.820f }, 0.0f, 0.16f },
    { { 0.850f, 0.820f, 0.700f }, 0.0f, 0.42f },
    { { 0.780f, 0.770f, 0.740f }, 0.0f, 0.20f },
    { { 0.290f, 0.280f, 0.270f }, 0.0f, 0.75f },
    { { 0.130f, 0.140f, 0.150f }, 0.0f, 0.70f },
    { { 0.560f, 0.450f, 0.320f }, 0.0f, 0.88f },
    { { 0.330f, 0.320f, 0.300f }, 0.0f, 0.95f },
    { { 0.055f, 0.055f, 0.060f }, 0.0f, 0.90f },
    { { 0.880f, 0.880f, 0.860f }, 0.0f, 0.97f },
    { { 0.055f, 0.310f, 0.160f }, 0.0f, 0.14f },
    { { 0.070f, 0.130f, 0.480f }, 0.0f, 0.22f },
    { { 0.060f, 0.330f, 0.240f }, 0.0f, 0.26f },
    { { 0.100f, 0.520f, 0.520f }, 0.0f, 0.25f },
    { { 0.600f, 0.320f, 0.060f }, 0.0f, 0.18f },
    { { 0.190f, 0.085f, 0.030f }, 0.0f, 0.35f },
    { { 0.320f, 0.210f, 0.110f }, 0.0f, 0.78f },
    { { 0.045f, 0.035f, 0.030f }, 0.0f, 0.45f },
    { { 0.430f, 0.300f, 0.170f }, 0.0f, 0.85f },
    { { 0.230f, 0.130f, 0.080f }, 0.0f, 0.65f },
    { { 0.820f, 0.790f, 0.700f }, 0.0f, 0.55f },
    { { 0.180f, 0.030f, 0.060f }, 0.0f, 0.96f },
    { { 0.110f, 0.180f, 0.320f }, 0.0f, 0.85f },
    { { 0.560f, 0.510f, 0.420f }, 0.0f, 0.90f },
    { { 0.018f, 0.018f, 0.018f }, 0.0f, 0.88f },
    { { 0.900f, 0.910f, 0.950f }, 0.0f, 0.60f },
    { { 0.090f, 0.200f, 0.070f }, 0.0f, 0.93f },
};
constexpr int MATERIAL_COUNT = int(sizeof(MATERIALS) / sizeof(MATERIALS[0]));

// ---------------------------------------------------------------------------
// lights (view space: camera at origin looking down -z, object at z = -4.2)
// ---------------------------------------------------------------------------
struct Light { Vec3f viewPos; Vec3f colour; float intensity; };
constexpr Light LIGHTS[4] = {
    { { -3.5f,  3.5f, -0.7f }, { 1.00f, 0.96f, 0.90f }, 52.0f },  // key
    { {  4.0f,  0.5f, -1.7f }, { 0.55f, 0.68f, 1.00f }, 26.0f },  // cool fill
    { { -1.5f, -3.0f, -2.2f }, { 1.00f, 0.55f, 0.32f }, 16.0f },  // warm bounce
    { {  1.0f,  1.5f, -8.2f }, { 0.85f, 0.88f, 1.00f }, 34.0f },  // rim
};

// ---------------------------------------------------------------------------
// environment: sky gradient + a sun along the key light
// ---------------------------------------------------------------------------
constexpr Vec3f SKY_TOP     = { 0.040f, 0.140f, 0.500f };  // saturated deep blue
constexpr Vec3f SKY_HORIZON = { 0.380f, 0.500f, 0.700f };  // lighter blue, less haze
constexpr Vec3f SKY_GROUND  = { 0.160f, 0.105f, 0.060f };  // warmer, sandier earth
constexpr float SKY_FALLOFF = 0.50f;
constexpr Vec3f SUN_COLOUR  = { 1.000f, 0.920f, 0.780f };  // a touch warmer
static const Vec3f SUN_DIR  = Vec3f{ -0.45f, 0.75f, 0.48f }.normalized();

static Vec3f skyColour(const Vec3f& d) {
    const float y = clampf(d.y, -1.f, 1.f);
    return y >= 0.f ? lerp(SKY_HORIZON, SKY_TOP,    std::pow( y, SKY_FALLOFF))
                    : lerp(SKY_HORIZON, SKY_GROUND, std::pow(-y, SKY_FALLOFF));
}

static Vec3f environmentColour(const Vec3f& d) {
    const float toSun = saturate(d.normalized().dot(SUN_DIR));
    const float disc  = std::pow(toSun, 900.f) * 14.f;
    const float haze  = std::pow(toSun,  12.f) * 0.30f;
    return skyColour(d) + SUN_COLOUR * (disc + haze);
}

static const Vec3f AMBIENT_UP   = skyColour({0.f,  1.f, 0.f});
static const Vec3f AMBIENT_DOWN = skyColour({0.f, -1.f, 0.f});

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
    return a2 / std::max(PI * d * d, 1e-7f);
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

constexpr uint32_t SCENE_SLOT = 0;

struct Uniforms {
    Mat4f mvp;
    Mat4f model;                 // pure rotation, doubles as the normal matrix
    Vec3f albedo; float metallic, roughness;
    float bump, noiseScale, variation;
    uint32_t detail, displace;
};

struct Varyings { Vec3f objPos; Vec3f objNormal; Vec3f worldPos; };

inline void pbrVS(const Vertex& in, const Uniforms& u, Varyings& out) {
    Vec3f pos = in.pos;
    if (u.displace != 0 && u.detail != 0 && u.bump > 0.f) {
        const float h = heightField(in.pos, u.noiseScale) - 0.5f;
        pos += in.normal * (h * u.bump * DISPLACE_SCALE);
    }
    // varyings carry the UNdisplaced object position: that's the domain the
    // height field is sampled in, so the bump normals agree with the geometry
    out.objPos    = in.pos;
    out.objNormal = in.normal;
    out.worldPos  = (u.model * Vec4f(pos, 1.f)).xyz;
    valo::valo_Position = u.mvp * Vec4f(pos, 1.f);
}

VALO_VERTEX_SHADER(pbrVS)
