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

constexpr float kPi   = std::numbers::pi_v<float>;
constexpr float kFovY = 0.9f;

// glTF materials leave slots empty all the time, so every texture field needs a
// "nothing here" value. Sampling it would be an out-of-bounds read, so every use
// is guarded.
constexpr TextureID kNoTex = UINT32_MAX;

static inline float saturate(float x) {
    return std::min(std::max(x, 0.f), 1.f);
}

/*
    glTF authors base color and emissive in sRGB, because that is what image
    editors produce. Lighting maths is only correct in linear space, so those
    two get decoded on read. Normal, metallic-roughness and occlusion maps are
    DATA, not colour - they are already linear and must not be touched.

    (VALO can now do this in the sampler via createTexture(path, srgb=true),
    which is both faster and more correct than pow() per pixel. This file still
    does it by hand so the shader shows the whole pipeline in one place.)
*/
static inline Vec3f srgbToLinear(const Vec3f& c) {
    return { std::pow(c.x, 2.2f), std::pow(c.y, 2.2f), std::pow(c.z, 2.2f) };
}


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

static const Light kLights[4] = {
    { { -3.5f,  3.5f,  3.5f }, { 1.00f, 0.96f, 0.90f }, 52.0f },   // key
    { {  4.0f,  0.5f,  2.5f }, { 0.55f, 0.68f, 1.00f }, 26.0f },   // cool fill
    { { -1.5f, -3.0f,  2.0f }, { 1.00f, 0.55f, 0.32f }, 16.0f },   // warm bounce
    { {  1.0f,  1.5f, -4.0f }, { 0.85f, 0.88f, 1.00f }, 34.0f },   // rim
};

constexpr Vec3f kSkyTop     = { 0.012f, 0.035f, 0.120f };
constexpr Vec3f kSkyHorizon = { 0.115f, 0.125f, 0.145f };
constexpr Vec3f kSkyGround  = { 0.020f, 0.020f, 0.023f };
constexpr Vec3f kSunColour  = { 1.000f, 0.945f, 0.855f };

static const Vec3f kSunDir = Vec3f{ -0.52f, 0.62f, 0.59f }.normalized();

/*
    A two-sided gradient: horizon -> top going up, horizon -> ground going down.
    The pow(y, 0.5) biases the blend toward the horizon so the band near y == 0
    is wide, which is what the eye expects from atmosphere.
*/
static Vec3f skyColour(const Vec3f& dir) {
    const float y = std::clamp(dir.y, -1.f, 1.f);

    return y >= 0.f
        ? lerp(kSkyHorizon, kSkyTop,    std::pow( y, 0.5f))
        : lerp(kSkyHorizon, kSkyGround, std::pow(-y, 0.5f));
}

/*
    Sky plus a sun built from two powers of the same dot product: a very tight
    one (^900) for the disc itself and a broad one (^12) for the glow around it.
    Cheap, and it reads as a sun from any angle.
*/
static Vec3f environmentColour(const Vec3f& dir) {
    const float toSun = saturate(dir.normalized().dot(kSunDir));
    const float disc  = std::pow(toSun, 900.f) * 14.0f;
    const float glow  = std::pow(toSun,  12.f) *  0.30f;

    return skyColour(dir) + kSunColour * (disc + glow);
}

// The ambient term is a hemisphere: sky colour straight up, ground colour
// straight down, blended by the normal. Both ends are constant, so they are
// evaluated once here instead of per pixel.
static const Vec3f kAmbientUp   = skyColour({ 0.f,  1.f, 0.f });
static const Vec3f kAmbientDown = skyColour({ 0.f, -1.f, 0.f });


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

    return a2 / std::max(kPi * denom * denom, 1e-7f);
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
struct Vertex {
    Vec3f pos;
    Vec3f normal;
    Vec2f uv;
    Vec4f tangent;      // xyz = tangent, w = bitangent handedness (+1 / -1)
};

struct Uniforms {
    Mat4f mvp;
    Mat4f model;
    Vec3f camPos;

    // material constants; glTF multiplies these by the textures below
    Vec4f baseColor;
    float metallic;
    float roughness;
    Vec3f emissive;

    // kNoTex when the material does not have that slot
    TextureID baseTex;
    TextureID mrTex;            // G = roughness, B = metallic, packed by glTF
    TextureID normalTex;
    TextureID aoTex;
    TextureID emissiveTex;

    bool useNormalMap;
};

struct Varyings {
    Vec3f worldPos;
    Vec3f normal;
    Vec2f uv;
    Vec4f tangent;
};

inline void gltfVS(const Vertex& in, const Uniforms& u, Varyings& out) {
    // Positions transform with w = 1 so they pick up the translation; normals
    // and tangents are directions, so they go through with w = 0 instead.
    out.worldPos = (u.model * Vec4f(in.pos,    1.f)).xyz;
    out.normal   = (u.model * Vec4f(in.normal, 0.f)).xyz;
    out.tangent  = Vec4f{ (u.model * Vec4f(in.tangent.xyz, 0.f)).xyz, in.tangent.w };
    out.uv       = in.uv;

    valo::valo_Position = u.mvp * Vec4f(in.pos, 1.f);
}

VALO_VERTEX_SHADER(gltfVS)
