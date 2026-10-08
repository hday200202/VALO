#pragma once

#include <render/ShaderRegistry.hpp>
#include <utils/GLTFParser.hpp>

#include <cmath>
#include <numbers>
#include <algorithm>

using valo::Vec2f;
using valo::Vec3f;
using valo::Vec4f;
using valo::Mat4f;
using valo::Color;
using valo::TextureID;

constexpr float PI   = std::numbers::pi_v<float>;
constexpr float FOV_Y = 0.9f;


static inline float saturate(float x) {
    return std::min(std::max(x, 0.f), 1.f);
}

/*
    No sRGB decoding in here: base colour and emissive are authored in sRGB, but
    loadModel() creates those two textures with srgb = true, so the sampler
    hands back linear values already. The data maps (normal, metallic-roughness,
    occlusion) are loaded linear, as they must be.
*/


/* ===========================================================================
    Lights and environment

    Positions are world space, which is fine here because the camera is fixed
    at +z looking at the origin and the MODEL is what rotates.
=========================================================================== */
struct Light {
    Vec3f pos;
    Vec3f colour;
    float intensity;
};

static const Light LIGHTS[4] = {
    { { -3.5f,  3.5f,  3.5f }, { 1.00f, 0.96f, 0.90f }, 52.0f },   // key
    { {  4.0f,  0.5f,  2.5f }, { 0.55f, 0.68f, 1.00f }, 26.0f },   // cool fill
    { { -1.5f, -3.0f,  2.0f }, { 1.00f, 0.55f, 0.32f }, 16.0f },   // warm bounce
    { {  1.0f,  1.5f, -4.0f }, { 0.85f, 0.88f, 1.00f }, 34.0f },   // rim
};

constexpr Vec3f SKY_TOP     = { 0.012f, 0.035f, 0.120f };
constexpr Vec3f SKY_HORIZON = { 0.115f, 0.125f, 0.145f };
constexpr Vec3f SKY_GROUND  = { 0.020f, 0.020f, 0.023f };
constexpr Vec3f SUN_COLOUR  = { 1.000f, 0.945f, 0.855f };

static const Vec3f SUN_DIR = Vec3f{ -0.52f, 0.62f, 0.59f }.normalized();

/*
    A two-sided gradient: horizon -> top going up, horizon -> ground going down.
    The pow(y, 0.5) biases the blend toward the horizon so the band near y == 0
    is wide, which is what the eye expects from atmosphere.
*/
static Vec3f skyColour(const Vec3f& dir) {
    const float y = std::clamp(dir.y, -1.f, 1.f);

    return y >= 0.f
        ? lerp(SKY_HORIZON, SKY_TOP,    std::pow( y, 0.5f))
        : lerp(SKY_HORIZON, SKY_GROUND, std::pow(-y, 0.5f));
}

/*
    Sky plus a sun built from two powers of the same dot product: a very tight
    one (^900) for the disc itself and a broad one (^12) for the glow around it.
    Cheap, and it reads as a sun from any angle.
*/
static Vec3f environmentColour(const Vec3f& dir) {
    const float toSun = saturate(dir.normalized().dot(SUN_DIR));
    const float disc  = std::pow(toSun, 900.f) * 14.0f;
    const float glow  = std::pow(toSun,  12.f) *  0.30f;

    return skyColour(dir) + SUN_COLOUR * (disc + glow);
}

// The ambient term is a hemisphere: sky colour straight up, ground colour
// straight down, blended by the normal. Both ends are constant, so they are
// evaluated once here instead of per pixel.
static const Vec3f AMBIENT_UP   = skyColour({ 0.f,  1.f, 0.f });
static const Vec3f AMBIENT_DOWN = skyColour({ 0.f, -1.f, 0.f });


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
    const float a2    = r * r * r * r;
    const float denom = nDotH * nDotH * (a2 - 1.f) + 1.f;

    return a2 / std::max(PI * denom * denom, 1e-7f);
}

// Schlick-GGX geometry term, one direction at a time; the caller multiplies the
// light and view directions together (Smith's method).
static float geometrySchlick(float nDotX, float r) {
    const float k = (r + 1.f) * (r + 1.f) / 8.f;

    return nDotX / std::max(nDotX * (1.f - k) + k, 1e-7f);
}

// Fresnel: reflectivity climbs toward 1 at grazing angles. f0 is the
// head-on reflectance - 0.04 for dielectrics, the albedo itself for metals.
static Vec3f fresnelSchlick(float cosTheta, const Vec3f& f0) {
    const float f = std::pow(1.f - saturate(cosTheta), 5.f);

    return f0 + (Vec3f{1.f, 1.f, 1.f} - f0) * f;
}

// Same curve, but the ceiling is pulled down by roughness. Used for the
// environment reflection, where a rough surface should not turn into a mirror
// at the silhouette the way a smooth one does.
static Vec3f fresnelSchlickRoughness(float cosTheta, const Vec3f& f0, float r) {
    const float f    = std::pow(1.f - saturate(cosTheta), 5.f);
    const float invR = 1.f - r;

    const Vec3f ceiling = {
        std::max(invR, f0.x),
        std::max(invR, f0.y),
        std::max(invR, f0.z),
    };

    return f0 + (ceiling - f0) * f;
}

// Reinhard (v / (1 + v)) to pull HDR into 0..1, then gamma for display.
static Vec3f toneMap(const Vec3f& c) {
    auto encode = [](float v) {
        return std::pow(saturate(v / (1.f + v)), 1.f / 2.2f);
    };

    return { encode(c.x), encode(c.y), encode(c.z) };
}


/* ===========================================================================
    Surface shader
=========================================================================== */


struct Uniforms {
    Mat4f               mvp;
    Mat4f               model;
    Vec3f               camPos;
    valo::ModelMaterial material;       // straight from loadModel()
    uint32_t            useNormalMap;
};

struct Varyings {
    Vec3f worldPos;
    Vec3f normal;
    Vec2f uv;
    Vec4f tangent;
};

inline void gltfVS(const valo::GLTFVertex& in, const Uniforms& u, Varyings& out) {
    // Positions transform with w = 1 so they pick up the translation; normals
    // and tangents are directions, so they go through with w = 0 instead.
    out.worldPos = (u.model * Vec4f(in.pos,    1.f)).xyz;
    out.normal   = (u.model * Vec4f(in.normal, 0.f)).xyz;
    out.tangent  = Vec4f{ (u.model * Vec4f(in.tangent.xyz, 0.f)).xyz, in.tangent.w };
    out.uv       = in.uv;

    valo::valo_Position = u.mvp * Vec4f(in.pos, 1.f);
}

VALO_VERTEX_SHADER(gltfVS)
