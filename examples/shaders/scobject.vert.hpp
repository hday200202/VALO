#pragma once

#include <render/ShaderRegistry.hpp>
#include <utils/GLTFParser.hpp>

#include <cmath>
#include <algorithm>

namespace sc {

using namespace valo;

constexpr float PI         = 3.14159265358979f;
constexpr int   LIGHT_COUNT = 4;
constexpr float FLOOR_Y     = 0.f;      // the model sits on y = 0, so does the mirror

// ---------------------------------------------------------------------------
// scalar helpers
// ---------------------------------------------------------------------------
static inline float saturate(const float x) { return std::min(std::max(x, 0.f), 1.f); }
static inline float clampf(const float x, const float lo, const float hi) { return std::min(std::max(x, lo), hi); }
static inline float smoothstepf(const float e0, const float e1, const float x) {
    const float t = saturate((x - e0) / std::max(e1 - e0, 1e-6f));
    return t * t * (3.f - 2.f * t);
}

static inline Vec3f mul(const Vec3f& a, const Vec3f& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

// Fixed exponents by repeated squaring. The sky shader runs on every pixel on
// screen, so a general std::pow there costs more than everything the model does.
static inline float pow5(const float x)  { const float x2 = x * x; return x2 * x2 * x; }
static inline float pow16(float x) { x *= x; x *= x; x *= x; x *= x; return x; }
static inline float pow32(float x) { x *= x; x *= x; x *= x; x *= x; x *= x; return x; }

/*
    glTF base colour and emissive textures are authored in sRGB; lighting has to
    happen in linear, so they get decoded on the way in and re-encoded at the
    end. Both directions run per pixel, several times over with overdraw, and
    std::pow is far too slow to call six times a fragment - the exact curves are
    replaced by a cubic fit going in and a table coming out, which between them
    stay inside a 255th of the real thing.
*/
static inline float srgbToLinear(const float c) {
    return c * (c * (c * 0.305306011f + 0.682171111f) + 0.012522878f);
}
static inline Vec3f srgbToLinear(const Vec3f& c) {
    return {srgbToLinear(c.x), srgbToLinear(c.y), srgbToLinear(c.z)};
}

struct GammaLUT {
    static constexpr int SIZE = 512;
    float v[SIZE + 1];
    GammaLUT() { for (int i = 0; i <= SIZE; ++i) v[i] = std::pow(float(i) / SIZE, 1.f / 2.2f); }
};
static const GammaLUT GAMMA;

/*
    The table is a CPU optimisation: std::pow six times a fragment is far too
    slow there, and the fit stays inside a 255th of the real curve. A GPU does
    pow in one instruction and has no way to hold the table, so the translated
    half just says what the table approximates.
*/
VALO_GLSL(linearToSrgb,
    "float linearToSrgb(float c) {\n"
    "    return pow(clamp(c, 0.0, 1.0), 1.0 / 2.2);\n"
    "}")

static inline float linearToSrgb(const float c) {
    const float x = saturate(c) * GammaLUT::SIZE;
    const int   i = std::min(int(x), GammaLUT::SIZE - 1);   // c == 1 lands on the last pair, not past it
    const float f = x - float(i);
    return GAMMA.v[i] + (GAMMA.v[i + 1] - GAMMA.v[i]) * f;
}

// Narkowicz's ACES fit - filmic shoulder, keeps the bright sun and orbs from
// clipping to flat white the way a plain x/(1+x) does
static inline Vec3f acesFilmic(const Vec3f& x) {
    auto f = [](const float v) {
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        return saturate((v * (a * v + b)) / (v * (c * v + d) + e));
    };
    return {f(x.x), f(x.y), f(x.z)};
}
static inline Vec3f encodeGamma(const Vec3f& c) {
    return {linearToSrgb(c.x), linearToSrgb(c.y), linearToSrgb(c.z)};
}
static inline Vec3f tonemap(const Vec3f& hdr, const float exposure) {
    return encodeGamma(acesFilmic(hdr * exposure));
}

// ---------------------------------------------------------------------------
// environment - one closed-form sky, used as the background, the diffuse
// irradiance and the specular reflection, so all three always agree
// ---------------------------------------------------------------------------
constexpr Vec3f ZENITH   = {0.008f, 0.022f, 0.075f};
constexpr Vec3f HORIZON  = {0.075f, 0.070f, 0.105f};
constexpr Vec3f NADIR    = {0.006f, 0.006f, 0.009f};
constexpr Vec3f SUN_TINT  = {1.000f, 0.880f, 0.680f};
constexpr Vec3f HAZE     = {0.400f, 0.230f, 0.180f};
static const  Vec3f SUN_DIR = Vec3f{-0.42f, 0.22f, 0.62f}.normalized();

// the ramp wants y^0.45 and the gamma table already holds y^(1/2.2) = y^0.4545,
// which is the same curve to the eye and one lookup instead of a pow
static inline Vec3f skyGradient(const Vec3f& d) {
    const float y = clampf(d.y, -1.f, 1.f);
    if (y >= 0.f) return lerp(HORIZON, ZENITH, linearToSrgb(y));
    return lerp(HORIZON, NADIR, linearToSrgb(-y));
}

// dir must already be unit length - every caller has one, and re-normalising
// here costs a sqrt on well over a million fragments a frame
static inline Vec3f environment(const Vec3f& d) {
    const float toSun = saturate(d.dot(SUN_DIR));

    /*
        A hard-edged disc for the sun, a tight glow around it, and a warm haze
        that only builds up near the horizon on the sun's side - a dusk sky
        rather than an overcast one, so the dark paint has something bright and
        directional to reflect. The disc is a smoothstep, not a huge exponent:
        pow(x, 2200) costs the same everywhere on screen for a result that is
        zero across all but a few hundred pixels.
    */
    const float disc = smoothstepf(0.99955f, 0.99985f, toSun) * 165.f;
    const float glow = pow32(toSun) * 0.55f + pow5(toSun) * 0.045f;
    const float band = pow16(saturate(1.f - std::abs(d.y)));

    return skyGradient(d) + SUN_TINT * (disc + glow) + HAZE * (band * toSun * 0.42f);
}

/*
    A real prefiltered cubemap blurs the environment by roughness. There is no
    cubemap here, so rough surfaces instead fade from the sharp reflection
    toward the flat hemisphere average - same end points, no storage.
*/
static const Vec3f IRRADIANCE_UP   = skyGradient({0.f,  1.f, 0.f}) * 0.85f + SUN_TINT * 0.05f;
static const Vec3f IRRADIANCE_DOWN = skyGradient({0.f, -1.f, 0.f});

static inline Vec3f environmentBlurred(const Vec3f& dir, const float roughness) {
    const Vec3f sharp = environment(dir);
    const Vec3f flat  = lerp(IRRADIANCE_DOWN, IRRADIANCE_UP, saturate(dir.y * 0.5f + 0.5f));
    return lerp(sharp, flat, saturate(roughness * roughness));
}

static inline Vec3f irradiance(const Vec3f& n) {
    return lerp(IRRADIANCE_DOWN, IRRADIANCE_UP, saturate(n.y * 0.5f + 0.5f));
}

// ---------------------------------------------------------------------------
// BRDF
// ---------------------------------------------------------------------------
static inline float distributionGGX(const float nDotH, const float a) {
    const float a2 = a * a;
    const float d  = nDotH * nDotH * (a2 - 1.f) + 1.f;
    return a2 / std::max(PI * d * d, 1e-7f);
}
static inline float geometrySmith(const float nDotV, const float nDotL, const float r) {
    const float k = (r + 1.f) * (r + 1.f) * 0.125f;
    const float gv = nDotV / std::max(nDotV * (1.f - k) + k, 1e-7f);
    const float gl = nDotL / std::max(nDotL * (1.f - k) + k, 1e-7f);
    return gv * gl;
}
static inline Vec3f fresnelSchlick(const float cosTheta, const Vec3f& f0) {
    const float f = pow5(1.f - saturate(cosTheta));
    return f0 + (Vec3f{1.f, 1.f, 1.f} - f0) * f;
}
static inline Vec3f fresnelRoughness(const float cosTheta, const Vec3f& f0, const float r) {
    const float f = pow5(1.f - saturate(cosTheta));
    const float inv = 1.f - r;
    const Vec3f ceiling = {std::max(inv, f0.x), std::max(inv, f0.y), std::max(inv, f0.z)};
    return f0 + (ceiling - f0) * f;
}

// Karis' analytic DFG approximation - stands in for the split-sum BRDF LUT
static inline Vec2f envBRDF(const float nDotV, const float roughness) {
    const Vec4f c0 = {-1.f, -0.0275f, -0.572f, 0.022f};
    const Vec4f c1 = { 1.f,  0.0425f,  1.040f, -0.040f};
    const Vec4f r  = {c0.x * roughness + c1.x, c0.y * roughness + c1.y,
                      c0.z * roughness + c1.z, c0.w * roughness + c1.w};
    const float a004 = std::min(r.x * r.x, std::exp2(-9.28f * nDotV)) * r.x + r.y;
    return {a004 * -1.04f + r.z, a004 * 1.04f + r.w};
}

// ---------------------------------------------------------------------------
// lights
// ---------------------------------------------------------------------------
struct Light {
    Vec3f pos       = {0.f, 0.f, 0.f};
    Vec3f colour    = {1.f, 1.f, 1.f};
    float intensity = 0.f;
};

/*
    Per-frame constants: the light rig plus the camera position and exposure.
    Nothing here varies between draws, so instead of memcpy'ing it into every
    RenderCommand it is uploaded once a frame and bound to a slot, which every
    shader below reads with uniformBuffer<SceneUniforms>(SCENE_SLOT).

    Note the read happens when the queue executes, at endFrame - so a uniform
    buffer can only hold values that are the same for the whole frame. Anything
    that changes per draw (mvp, material, handedness) stays in setUniforms.
*/
struct SceneUniforms {
    Light lights[LIGHT_COUNT];
    Vec3f camPos;
    float exposure;
};

inline constexpr uint32_t SCENE_SLOT = 0;

// ---------------------------------------------------------------------------
// camera - yaw/pitch are the authority, forward is always derived from them
// ---------------------------------------------------------------------------
struct Camera {
    float pitch = 0.f, yaw = 0.f;
    float nearPlane = 0.05f, farPlane = 400.f, fov = 1.05f;

    Vec3f pos     = {0.f, 1.2f, 7.f};
    Vec3f forward = {0.f, 0.f, -1.f};
    Vec3f right   = {1.f, 0.f,  0.f};
    Vec3f up      = {0.f, 1.f,  0.f};

    void setForward(const Vec3f& dir) {
        const Vec3f f = dir.normalized();
        pitch = std::asin(clampf(f.y, -1.f, 1.f));
        yaw   = std::atan2(f.x, -f.z);
        rebuildBasis();
    }

    void lookAtPoint(const Vec3f& target) { setForward(target - pos); }

    void update(const Vec2f& mouseDelta, const Vec3f& movement) {
        yaw   += mouseDelta.x * 0.005f;
        pitch -= mouseDelta.y * 0.005f;
        pitch  = clampf(pitch, -1.5f, 1.5f);
        rebuildBasis();
        pos += (forward * movement.z) + (right * movement.x) + (up * movement.y);
    }

    void rebuildBasis() {
        forward = Vec3f{
            std::sin(yaw) * std::cos(pitch),
            std::sin(pitch),
            -std::cos(yaw) * std::cos(pitch)
        }.normalized();
        right = forward.cross(Vec3f{0.f, 1.f, 0.f}).normalized();
        up    = right.cross(forward).normalized();
    }

    Mat4f view() const { return Mat4f::lookAt(pos, pos + forward, {0.f, 1.f, 0.f}); }
};

// ---------------------------------------------------------------------------
// vertex - GLTFVertex plus a tangent, so normal maps have a frame to sit in
// ---------------------------------------------------------------------------
struct Vertex {
    Vec3f pos;
    Vec3f normal;
    Vec4f tangent;      // xyz = tangent, w = bitangent handedness
    Vec2f uv;

    static VtxLayout layout() {
        return {{VtxAttrib::Vec3f, VtxAttrib::Vec3f, VtxAttrib::Vec4f, VtxAttrib::Vec2f}};
    }
};

struct Material {
    TextureID baseColorTex          = EMPTY_TEXTURE;
    TextureID metallicRoughnessTex  = EMPTY_TEXTURE;
    TextureID normalTex             = EMPTY_TEXTURE;
    TextureID occlusionTex          = EMPTY_TEXTURE;
    TextureID emissiveTex           = EMPTY_TEXTURE;

    Vec4f baseColor = {1.f, 1.f, 1.f, 1.f};
    Vec3f emissive  = {0.f, 0.f, 0.f};
    float metallic  = 1.f;
    float roughness = 1.f;
};

// ---------------------------------------------------------------------------
// surface shader - one Cook-Torrance evaluation per pixel
// ---------------------------------------------------------------------------
struct Uniforms {
    Mat4f    mvp;
    Mat4f    model;                     // world transform, doubles as the normal matrix
    Material material;
    float    handedness;                // -1 in the mirrored pass, flips the bitangent
    float    dim;                       // reflections come back a touch darker than the real thing
    float    emissiveBoost;
};
static_assert(sizeof(Uniforms) <= RenderCommand::MAX_UNIFORM_BYTES,
              "Uniforms must fit in RenderCommand's fixed uniform block");

struct Varyings {
    Vec3f worldPos;
    Vec3f normal;
    Vec4f tangent;
    Vec2f uv;
};

inline void objectVS(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.worldPos = (u.model * Vec4f{in.pos, 1.f}).xyz;
    out.normal   = (u.model * Vec4f{in.normal, 0.f}).xyz;
    out.tangent  = Vec4f{(u.model * Vec4f{in.tangent.xyz, 0.f}).xyz, in.tangent.w};
    out.uv       = in.uv;
    valo_Position = u.mvp * Vec4f{in.pos, 1.f};
}

VALO_VERTEX_SHADER(objectVS)

} // namespace sc
