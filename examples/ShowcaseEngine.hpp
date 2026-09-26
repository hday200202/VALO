#pragma once

/*
    ShowcaseEngine - the minimal engine with the shading turned all the way up.

    Same shape as MinimalEngine (a Model you position, an Engine that owns the
    window and camera), but every surface goes through a Cook-Torrance metallic
    /roughness BRDF lit by a procedural sky and four movable lights, and the
    frame is assembled in five passes so the floor can mirror the model:

        1  sky      fullscreen quad, ray per pixel, no depth
        2  mirror   the model again through y = 0, front faces, full depth
        3  floor    alpha = 1 - fresnel, so pass 2 shows through at grazing angles
        4  model    the real thing, opaque then blended materials
        5  orbs     emissive spheres so the lights are visible in frame

    Everything is evaluated per pixel on the CPU, so it is a rasteriser wearing
    a ray tracer's clothes: the reflection is a mirrored draw rather than a
    traced ray, and the "IBL" is the same closed-form sky the background uses,
    sampled along the reflection vector and blurred by roughness.
*/

#include <string>
#include <deque>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>
#include "../include/VALO.hpp"

namespace sc {

using namespace valo;

constexpr float kPi         = 3.14159265358979f;
constexpr int   kLightCount = 4;
constexpr float kFloorY     = 0.f;      // the model sits on y = 0, so does the mirror

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
    static constexpr int kSize = 512;
    float v[kSize + 1];
    GammaLUT() { for (int i = 0; i <= kSize; ++i) v[i] = std::pow(float(i) / kSize, 1.f / 2.2f); }
};
static const GammaLUT kGamma;

static inline float linearToSrgb(const float c) {
    const float x = saturate(c) * GammaLUT::kSize;
    const int   i = std::min(int(x), GammaLUT::kSize - 1);   // c == 1 lands on the last pair, not past it
    const float f = x - float(i);
    return kGamma.v[i] + (kGamma.v[i + 1] - kGamma.v[i]) * f;
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
constexpr Vec3f kZenith   = {0.008f, 0.022f, 0.075f};
constexpr Vec3f kHorizon  = {0.075f, 0.070f, 0.105f};
constexpr Vec3f kNadir    = {0.006f, 0.006f, 0.009f};
constexpr Vec3f kSunTint  = {1.000f, 0.880f, 0.680f};
constexpr Vec3f kHaze     = {0.400f, 0.230f, 0.180f};
static const  Vec3f kSunDir = Vec3f{-0.42f, 0.22f, 0.62f}.normalized();

// the ramp wants y^0.45 and the gamma table already holds y^(1/2.2) = y^0.4545,
// which is the same curve to the eye and one lookup instead of a pow
static inline Vec3f skyGradient(const Vec3f& d) {
    const float y = clampf(d.y, -1.f, 1.f);
    if (y >= 0.f) return lerp(kHorizon, kZenith, linearToSrgb(y));
    return lerp(kHorizon, kNadir, linearToSrgb(-y));
}

// dir must already be unit length - every caller has one, and re-normalising
// here costs a sqrt on well over a million fragments a frame
static inline Vec3f environment(const Vec3f& d) {
    const float toSun = saturate(d.dot(kSunDir));

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

    return skyGradient(d) + kSunTint * (disc + glow) + kHaze * (band * toSun * 0.42f);
}

/*
    A real prefiltered cubemap blurs the environment by roughness. There is no
    cubemap here, so rough surfaces instead fade from the sharp reflection
    toward the flat hemisphere average - same end points, no storage.
*/
static const Vec3f kIrradianceUp   = skyGradient({0.f,  1.f, 0.f}) * 0.85f + kSunTint * 0.05f;
static const Vec3f kIrradianceDown = skyGradient({0.f, -1.f, 0.f});

static inline Vec3f environmentBlurred(const Vec3f& dir, const float roughness) {
    const Vec3f sharp = environment(dir);
    const Vec3f flat  = lerp(kIrradianceDown, kIrradianceUp, saturate(dir.y * 0.5f + 0.5f));
    return lerp(sharp, flat, saturate(roughness * roughness));
}

static inline Vec3f irradiance(const Vec3f& n) {
    return lerp(kIrradianceDown, kIrradianceUp, saturate(n.y * 0.5f + 0.5f));
}

// ---------------------------------------------------------------------------
// BRDF
// ---------------------------------------------------------------------------
static inline float distributionGGX(const float nDotH, const float a) {
    const float a2 = a * a;
    const float d  = nDotH * nDotH * (a2 - 1.f) + 1.f;
    return a2 / std::max(kPi * d * d, 1e-7f);
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
    shader below reads with uniformBuffer<SceneUniforms>(kSceneSlot).

    Note the read happens when the queue executes, at endFrame - so a uniform
    buffer can only hold values that are the same for the whole frame. Anything
    that changes per draw (mvp, material, handedness) stays in setUniforms.
*/
struct SceneUniforms {
    Light lights[kLightCount];
    Vec3f camPos;
    float exposure;
};

inline constexpr uint32_t kSceneSlot = 0;

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

static inline void objectVs(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.worldPos = (u.model * Vec4f{in.pos, 1.f}).xyz;
    out.normal   = (u.model * Vec4f{in.normal, 0.f}).xyz;
    out.tangent  = Vec4f{(u.model * Vec4f{in.tangent.xyz, 0.f}).xyz, in.tangent.w};
    out.uv       = in.uv;
    valo_Position = u.mvp * Vec4f{in.pos, 1.f};
}

static inline void objectFs(const Varyings& in, const Uniforms& u) {
    const Material& m = u.material;

    // --- albedo / alpha ---
    Vec3f albedo = m.baseColor.xyz;
    float alpha  = m.baseColor.w;
    if (m.baseColorTex != EMPTY_TEXTURE) {
        const Color s = texture(m.baseColorTex, in.uv);
        albedo = mul(albedo, srgbToLinear(s.xyz));
        alpha *= s.w;
    }

    // --- metallic / roughness: glTF packs them in G and B, already linear ---
    float metallic  = m.metallic;
    float roughness = m.roughness;
    if (m.metallicRoughnessTex != EMPTY_TEXTURE) {
        const Color s = texture(m.metallicRoughnessTex, in.uv);
        roughness *= s.y;
        metallic  *= s.z;
    }
    roughness = clampf(roughness, 0.045f, 1.f);   // a perfectly sharp GGX lobe aliases badly
    metallic  = saturate(metallic);

    // --- normal mapping in the interpolated tangent frame ---
    Vec3f n = in.normal.normalized();
    if (m.normalTex != EMPTY_TEXTURE) {
        const Vec3f rawT = in.tangent.xyz;
        if (rawT.dot(rawT) > 1e-8f) {
            const Vec3f t = (rawT - n * n.dot(rawT)).normalized();   // re-orthogonalise after interpolation
            const Vec3f b = n.cross(t) * (in.tangent.w * u.handedness);
            const Color s = texture(m.normalTex, in.uv);
            const Vec3f tn = {s.x * 2.f - 1.f, s.y * 2.f - 1.f, s.z * 2.f - 1.f};
            n = (t * tn.x + b * tn.y + n * tn.z).normalized();
        }
    }

    const SceneUniforms& scene = uniformBuffer<SceneUniforms>(kSceneSlot);

    const Vec3f v = (scene.camPos - in.worldPos).normalized();
    if (n.dot(v) < 0.f) n = n * -1.f;                                // back faces of blended glass
    const float nDotV = std::max(n.dot(v), 1e-4f);

    const float ao = (m.occlusionTex != EMPTY_TEXTURE) ? texture(m.occlusionTex, in.uv).x : 1.f;
    const Vec3f f0 = lerp(Vec3f{0.04f, 0.04f, 0.04f}, albedo, metallic);
    const Vec3f kD = albedo * ((1.f - metallic) / kPi);

    // --- punctual lights ---
    Vec3f radiance = {0.f, 0.f, 0.f};
    for (int i = 0; i < kLightCount; ++i) {
        const Light& light = scene.lights[i];
        if (light.intensity <= 0.f) continue;

        const Vec3f toLight = light.pos - in.worldPos;
        const float dist2   = std::max(toLight.dot(toLight), 1e-4f);
        if (light.intensity < dist2 * 0.0025f) continue;

        const Vec3f l     = toLight * (1.f / std::sqrt(dist2));
        const float nDotL = n.dot(l);
        if (nDotL <= 0.f) continue;

        const Vec3f h     = (l + v).normalized();
        const float nDotH = std::max(n.dot(h), 0.f);
        const float vDotH = std::max(v.dot(h), 0.f);

        const Vec3f f   = fresnelSchlick(vDotH, f0);
        const float d   = distributionGGX(nDotH, roughness * roughness);
        const float g   = geometrySmith(nDotV, nDotL, roughness);
        const Vec3f spec = f * (d * g / std::max(4.f * nDotV * nDotL, 1e-5f));
        const Vec3f diff = mul(kD, Vec3f{1.f, 1.f, 1.f} - f);

        radiance += mul(diff + spec, light.colour * (light.intensity / dist2)) * nDotL;
    }

    // --- image based lighting, split-sum style ---
    const Vec3f reflDir  = (n * (2.f * n.dot(v)) - v).normalized();
    const Vec3f fEnv     = fresnelRoughness(nDotV, f0, roughness);
    const Vec2f dfg      = envBRDF(nDotV, roughness);
    const Vec3f specIBL  = mul(environmentBlurred(reflDir, roughness), fEnv * dfg.x + Vec3f{dfg.y, dfg.y, dfg.y});
    const Vec3f diffIBL  = mul(albedo * (1.f - metallic), irradiance(n));
    const Vec3f ambient  = (diffIBL * 0.55f + specIBL) * ao;

    // The exporter leaves emissiveFactor black on the lamp meshes even though
    // they ship an emissive map, so a bare map is taken at face value - the
    // headlights are meant to glow.
    Vec3f emissive = m.emissive;
    if (m.emissiveTex != EMPTY_TEXTURE) {
        const Vec3f tex = srgbToLinear(texture(m.emissiveTex, in.uv).xyz);
        const bool  blackFactor = (m.emissive.x + m.emissive.y + m.emissive.z) < 1e-4f;
        emissive = blackFactor ? tex : mul(m.emissive, tex);
    }

    const Vec3f hdr = (radiance + ambient + emissive * u.emissiveBoost) * u.dim;
    valo_FragColor = {tonemap(hdr, scene.exposure), alpha};
}

// ---------------------------------------------------------------------------
// sky - one fullscreen quad, a camera ray per pixel
// ---------------------------------------------------------------------------
struct SkyVertex   { Vec2f ndc; };
struct SkyVaryings { Vec2f ndc; };
struct SkyUniforms {
    Vec3f right, up, forward;
    float tanHalfFov, aspect;
};

static inline void skyVs(const SkyVertex& in, const SkyUniforms&, SkyVaryings& out) {
    out.ndc = in.ndc;
    valo_Position = {in.ndc.x, in.ndc.y, 0.99999f, 1.f};    // pinned to the far plane
}

static inline void skyFs(const SkyVaryings& in, const SkyUniforms& u) {
    const Vec3f dir = (u.right   * (in.ndc.x * u.tanHalfFov * u.aspect)
                     + u.up      * (in.ndc.y * u.tanHalfFov)
                     + u.forward).normalized();
    valo_FragColor = {tonemap(environment(dir), uniformBuffer<SceneUniforms>(kSceneSlot).exposure), 1.f};
}

// ---------------------------------------------------------------------------
// floor - a glossy plane that fades into the sky, with the mirrored pass
// showing through wherever the fresnel term says the plane is reflective
// ---------------------------------------------------------------------------
struct FloorVertex   { Vec3f pos; };
struct FloorVaryings { Vec3f worldPos; };
struct FloorUniforms {
    Mat4f viewProj;
    Vec3f shadowCentre;
    Vec2f shadowRadius;     // xz half extents of the model footprint
    float fadeStart, fadeEnd, gridSpacing;
};
static_assert(sizeof(FloorUniforms) <= RenderCommand::MAX_UNIFORM_BYTES,
              "FloorUniforms must fit in RenderCommand's fixed uniform block");

static inline void floorVs(const FloorVertex& in, const FloorUniforms& u, FloorVaryings& out) {
    out.worldPos = in.pos;
    valo_Position = u.viewProj * Vec4f{in.pos, 1.f};
}

static inline void floorFs(const FloorVaryings& in, const FloorUniforms& u) {
    const SceneUniforms& scene = uniformBuffer<SceneUniforms>(kSceneSlot);

    const Vec3f p = in.worldPos;
    const Vec3f toCam = scene.camPos - p;
    const float dist  = toCam.length();

    // past the fade the quad contributes nothing, so bail before paying for the
    // lights and the environment lookup - that is most of the lower screen
    if (dist >= u.fadeEnd) { discard; return; }

    const Vec3f v     = toCam * (1.f / std::max(dist, 1e-5f));
    const float nDotV = std::max(v.y, 1e-4f);   // the plane's normal is +y, so n.dot(x) is just x.y

    // roughness rises with distance: near the camera the plane is a mirror, far
    // away it should not resolve detail it has no samples for
    const float roughness = clampf(0.055f + dist * 0.004f, 0.055f, 0.65f);
    const Vec3f albedo    = {0.022f, 0.023f, 0.028f};
    const Vec3f f0        = {0.045f, 0.045f, 0.048f};

    // a faint grid, thinned out with distance so it stops shimmering
    float grid = 0.f;
    if (dist < 38.f) {
        const float g = u.gridSpacing, half = g * 0.5f;
        const float gx = std::abs(p.x - std::floor(p.x / g) * g - half);
        const float gz = std::abs(p.z - std::floor(p.z / g) * g - half);
        const float lineW = 0.010f + dist * 0.0030f;
        grid = (1.f - smoothstepf(0.f, lineW, std::min(gx, gz))) * (1.f - smoothstepf(5.f, 38.f, dist));
    }

    // soft contact shadow: an ellipse under the model, no ray needed
    const float ex = (p.x - u.shadowCentre.x) / u.shadowRadius.x;
    const float ez = (p.z - u.shadowCentre.z) / u.shadowRadius.y;
    const float occl = 1.f - 0.88f * (1.f - smoothstepf(0.30f, 1.20f, std::sqrt(ex * ex + ez * ez)));

    Vec3f radiance = {0.f, 0.f, 0.f};
    for (int i = 0; i < kLightCount; ++i) {
        const Light& light = scene.lights[i];
        if (light.intensity <= 0.f) continue;

        const Vec3f toLight = light.pos - p;
        const float dist2   = std::max(toLight.dot(toLight), 1e-4f);

        // the plane runs out to the horizon, so most of its pixels are far
        // enough from every light that the whole GGX evaluation rounds to black
        if (light.intensity < dist2 * 0.0025f) continue;

        const Vec3f l     = toLight * (1.f / std::sqrt(dist2));
        const float nDotL = l.y;
        if (nDotL <= 0.f) continue;

        const Vec3f h     = (l + v).normalized();
        const float nDotH = std::max(h.y, 0.f);
        const Vec3f f     = fresnelSchlick(std::max(v.dot(h), 0.f), f0);
        const float d     = distributionGGX(nDotH, roughness * roughness);
        const float g     = geometrySmith(nDotV, nDotL, roughness);
        const Vec3f spec  = f * (d * g / std::max(4.f * nDotV * nDotL, 1e-5f));
        const Vec3f diff  = albedo * ((1.f / kPi) * occl);

        radiance += mul(diff + spec, light.colour * (light.intensity / dist2)) * nDotL;
    }

    // the plane's own view of the sky, so the floor is never flat black
    const Vec3f reflDir = {-v.x, v.y, -v.z};
    const Vec3f fEnv    = fresnelRoughness(nDotV, f0, roughness);
    const Vec3f skyRefl = mul(environmentBlurred(reflDir, roughness), fEnv);
    const Vec3f ambient = albedo * 0.5f + skyRefl;

    Vec3f hdr = radiance + mul(ambient, Vec3f{occl, occl, occl});
    hdr += Vec3f{0.16f, 0.17f, 0.21f} * grid;

    /*
        alpha is what lets the mirrored pass underneath come through: 1 - fresnel
        means the plane is opaque looking straight down and turns into a mirror
        at grazing angles, which is exactly how a real dielectric floor behaves.
        The distance fade then dissolves the whole quad into the sky so its far
        edge never shows.
    */
    const float fresnel      = saturate(fEnv.x * 6.5f);
    const float fade         = 1.f - smoothstepf(u.fadeStart, u.fadeEnd, dist);
    const float reflectivity = fresnel * 0.92f * (0.3f + 0.7f * occl);   // occluded ground is a poor mirror
    valo_FragColor = {tonemap(hdr, scene.exposure), (1.f - reflectivity) * fade};
}

// ---------------------------------------------------------------------------
// light orbs - emissive spheres with a fresnel halo, drawn additively
// ---------------------------------------------------------------------------
struct OrbVertex   { Vec3f pos; Vec3f normal; };
struct OrbVaryings { Vec3f normal; Vec3f worldPos; };
struct OrbUniforms { Mat4f viewProj; Vec3f centre, colour; float brightness, radius; };

static inline void orbVs(const OrbVertex& in, const OrbUniforms& u, OrbVaryings& out) {
    out.normal   = in.normal;
    out.worldPos = in.pos * u.radius + u.centre;
    valo_Position = u.viewProj * Vec4f{out.worldPos, 1.f};
}

static inline void orbFs(const OrbVaryings& in, const OrbUniforms& u) {
    const SceneUniforms& scene = uniformBuffer<SceneUniforms>(kSceneSlot);

    const Vec3f v = (scene.camPos - in.worldPos).normalized();
    const float facing = saturate(in.normal.normalized().dot(v));
    // bright core, halo climbing toward the silhouette
    const float halo = std::pow(1.f - facing, 2.2f);
    const Vec3f hdr  = u.colour * (u.brightness * (0.55f + halo * 2.6f));
    valo_FragColor = {tonemap(hdr, scene.exposure), 1.f};
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
class Model {
public:
    Model() = default;

    void setVALO(VALO* valo)                    { m_valo = valo; }
    void setSceneBuffer(UniformBufferID scene)  { m_sceneBuffer = scene; }
    void setTransform(const Mat4f& transform)   { m_transform = transform; }
    void setPosition(const Vec3f& position)     { m_transform = Mat4f::translation(position); }
    bool isLoaded() const                       { return m_loaded; }

    /*
        glTF files agree on nothing about placement or units: of the models to
        hand one is 0.06 units end to end and another is 4, and most are centred
        on the origin rather than standing on it - which puts half the geometry
        under the floor, where the mirror pass reflects it back up through
        itself and it reads as a second copy of the model.

        So every model is fitted on load: scaled uniformly until its longest
        axis is targetSize, centred on x/z, and dropped until its lowest point
        rests on groundY. Calling setTransform() or setPosition() afterwards
        replaces this outright.
    */
    void fitToGround(const float targetSize, const float groundY) {
        if (!m_loaded) return;

        const Vec3f size = m_boundsMax - m_boundsMin;
        const float longest = std::max({size.x, size.y, size.z});
        if (!(longest > 1e-9f)) return;                      // also catches a NaN axis

        const float scale  = targetSize / longest;
        const Vec3f centre = (m_boundsMin + m_boundsMax) * 0.5f;

        m_transform = Mat4f::translation({-centre.x * scale,
                                          groundY - m_boundsMin.y * scale,
                                          -centre.z * scale})
                    * Mat4f::scale({scale, scale, scale});
    }

    Vec3f boundsMin() const { return m_boundsMin; }
    Vec3f boundsMax() const { return m_boundsMax; }

    // bounds with the model's own transform applied, so the contact shadow
    // tracks setPosition() instead of staying where the model was loaded
    void worldBounds(Vec3f& lo, Vec3f& hi) const {
        lo = { 1e30f,  1e30f,  1e30f};
        hi = {-1e30f, -1e30f, -1e30f};
        for (int c = 0; c < 8; ++c) {
            const Vec3f corner = {(c & 1) ? m_boundsMax.x : m_boundsMin.x,
                                  (c & 2) ? m_boundsMax.y : m_boundsMin.y,
                                  (c & 4) ? m_boundsMax.z : m_boundsMin.z};
            const Vec3f w = (m_transform * Vec4f{corner, 1.f}).xyz;
            lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
            hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
        }
    }

    bool loadGLTF(const std::string& path) {
        if (!m_valo) return false;

        const GLTF gltf = parseGLTF(path);
        if (gltf.vertices.empty() || gltf.indices.empty()) return false;

        const std::vector<Vertex> verts = buildTangents(gltf);
        m_vertices = m_valo->createVertexBuffer(verts.data(), verts.size(), Vertex::layout());
        m_indices  = m_valo->createIndexBuffer(gltf.indices.data(), gltf.indices.size());
        m_program  = m_valo->createProgram(objectVs, objectFs);

        for (const GLTFMaterial& src : gltf.materials) {
            m_materials.push_back(Material{
                .baseColorTex           = loadTexture(src.baseColorTex),
                .metallicRoughnessTex   = loadTexture(src.metallicRoughnessTex),
                .normalTex              = loadTexture(src.normalTex),
                .occlusionTex           = loadTexture(src.occlusionTex),
                .emissiveTex            = loadTexture(src.emissiveTex),
                .baseColor              = src.baseColor,
                .emissive               = src.emissive,
                .metallic               = src.metallic,
                .roughness              = src.roughness,
            });
        }

        /*
            The node graph is flattened once here instead of every frame. Each
            primitive also gets a mesh-space centroid, which is what the blended
            pass sorts on - glass needs to arrive back to front or it composites
            in the wrong order.
        */
        for (const GLTFNode& node : gltf.nodes) {
            if (node.mesh < 0 || size_t(node.mesh) >= gltf.meshes.size()) continue;

            for (const GLTFPrimitive& prim : gltf.meshes[node.mesh].primitives) {
                Vec3f centroid = {0.f, 0.f, 0.f};
                const uint32_t last = std::min<uint32_t>(prim.firstIndex + prim.indexCount,
                                                         uint32_t(gltf.indices.size()));
                uint32_t counted = 0;
                for (uint32_t i = prim.firstIndex; i < last; ++i) {
                    const Vec3f& p = gltf.vertices[gltf.indices[i]].pos;
                    centroid += p;
                    ++counted;
                    trackBounds((node.world * Vec4f{p, 1.f}).xyz);
                }
                if (counted) centroid = centroid * (1.f / float(counted));

                const bool blended = materialFor(prim.material).baseColor.w < 0.995f;
                m_items.push_back(Item{node.world, centroid, prim.firstIndex, prim.indexCount,
                                       prim.material, blended});
            }
        }

        return m_loaded = true;
    }

    // pass = the world matrix the whole model is drawn through (identity for the
    // real draw, the mirror for the reflection)
    void draw(const Mat4f& viewProj, const Mat4f& pass, const Vec3f& camPos,
              const float handedness, const float dim, const float emissiveBoost,
              const bool blendedPass, const CullMode cull) {
        if (!m_valo || !m_loaded) return;

        const Mat4f root = pass * m_transform;

        if (blendedPass) {
            m_sorted.clear();
            for (size_t i = 0; i < m_items.size(); ++i) {
                if (!m_items[i].blended) continue;
                const Vec3f world = (root * m_items[i].world * Vec4f{m_items[i].centroid, 1.f}).xyz;
                m_sorted.push_back({(world - camPos).length(), i});
            }
            std::sort(m_sorted.begin(), m_sorted.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            for (const auto& [_, index] : m_sorted)
                submit(m_items[index], viewProj, root,
                       handedness, dim, emissiveBoost, true, cull);
            return;
        }

        for (const Item& item : m_items)
            if (!item.blended)
                submit(item, viewProj, root,
                       handedness, dim, emissiveBoost, false, cull);
    }

private:
    struct Item {
        Mat4f    world;
        Vec3f    centroid;
        uint32_t firstIndex, indexCount;
        int      material;
        bool     blended;
    };

    void submit(const Item& item, const Mat4f& viewProj, const Mat4f& root, const float handedness,
                const float dim, const float emissiveBoost, const bool blended, const CullMode cull) {
        const Mat4f model = root * item.world;

        Uniforms u{};
        u.mvp           = viewProj * model;
        u.model         = model;
        u.material      = materialFor(item.material);
        u.handedness    = handedness;
        u.dim           = dim;
        u.emissiveBoost = emissiveBoost;

        RenderCommand cmd;
        cmd.verts      = m_vertices;
        cmd.indices    = m_indices;
        cmd.program    = m_program;
        cmd.firstIndex = item.firstIndex;
        cmd.indexCount = item.indexCount;
        cmd.cullmode   = cull;
        cmd.blendmode  = blended ? BlendMode::Alpha : BlendMode::Opaque;
        cmd.depthWrite = !blended;
        cmd.setUniforms(u);
        cmd.bindUniformBuffer(kSceneSlot, m_sceneBuffer);
        m_valo->submit(cmd);
    }

    /*
        glTF only ships tangents when the exporter felt like it, and this model
        does not, so they are derived from the UV parameterisation: per triangle
        the tangent is the direction in which u increases, accumulated onto each
        vertex and orthonormalised against the normal at the end.
    */
    static std::vector<Vertex> buildTangents(const GLTF& gltf) {
        const size_t n = gltf.vertices.size();
        std::vector<Vertex> out(n);
        std::vector<Vec3f> tan(n, Vec3f{0.f, 0.f, 0.f}), bitan(n, Vec3f{0.f, 0.f, 0.f});

        for (size_t i = 0; i < n; ++i)
            out[i] = Vertex{gltf.vertices[i].pos, gltf.vertices[i].normal,
                            Vec4f{0.f, 0.f, 0.f, 1.f}, gltf.vertices[i].uv};

        for (size_t i = 0; i + 2 < gltf.indices.size(); i += 3) {
            const uint32_t i0 = gltf.indices[i], i1 = gltf.indices[i + 1], i2 = gltf.indices[i + 2];
            if (i0 >= n || i1 >= n || i2 >= n) continue;

            const Vec3f e1 = gltf.vertices[i1].pos - gltf.vertices[i0].pos;
            const Vec3f e2 = gltf.vertices[i2].pos - gltf.vertices[i0].pos;
            const Vec2f d1 = gltf.vertices[i1].uv  - gltf.vertices[i0].uv;
            const Vec2f d2 = gltf.vertices[i2].uv  - gltf.vertices[i0].uv;

            const float det = d1.x * d2.y - d2.x * d1.y;
            if (std::abs(det) < 1e-12f) continue;        // degenerate UVs contribute nothing
            const float r = 1.f / det;

            const Vec3f t = (e1 * d2.y - e2 * d1.y) * r;
            const Vec3f b = (e2 * d1.x - e1 * d2.x) * r;
            for (const uint32_t idx : {i0, i1, i2}) { tan[idx] += t; bitan[idx] += b; }
        }

        for (size_t i = 0; i < n; ++i) {
            const Vec3f nrm = out[i].normal.normalized();
            Vec3f t = tan[i] - nrm * nrm.dot(tan[i]);

            if (t.dot(t) < 1e-12f) {
                // no usable UV gradient here, so any perpendicular will do
                const Vec3f axis = std::abs(nrm.y) < 0.99f ? Vec3f{0.f, 1.f, 0.f} : Vec3f{1.f, 0.f, 0.f};
                t = axis.cross(nrm);
            }
            t = t.normalized();

            const float w = nrm.cross(t).dot(bitan[i]) < 0.f ? -1.f : 1.f;
            out[i].tangent = Vec4f{t, w};
        }
        return out;
    }

    TextureID loadTexture(const std::string& path) {
        return path.empty() ? EMPTY_TEXTURE : m_valo->createTexture(path);
    }

    const Material& materialFor(const int index) const {
        if (index < 0 || size_t(index) >= m_materials.size()) return m_fallback;
        return m_materials[index];
    }

    void trackBounds(const Vec3f& p) {
        m_boundsMin = {std::min(m_boundsMin.x, p.x), std::min(m_boundsMin.y, p.y), std::min(m_boundsMin.z, p.z)};
        m_boundsMax = {std::max(m_boundsMax.x, p.x), std::max(m_boundsMax.y, p.y), std::max(m_boundsMax.z, p.z)};
    }

    VertexBufferID m_vertices = EMPTY_BUFFER;
    IndexBufferID  m_indices  = EMPTY_BUFFER;
    ProgramID      m_program  = DEFAULT_PROGRAM;

    std::vector<Material> m_materials;
    std::vector<Item>     m_items;
    std::vector<std::pair<float, size_t>> m_sorted;   // kept alive so sorting does not allocate per frame

    Material m_fallback;
    Mat4f    m_transform;

    Vec3f m_boundsMin = { 1e30f,  1e30f,  1e30f};
    Vec3f m_boundsMax = {-1e30f, -1e30f, -1e30f};

    VALO* m_valo = nullptr;
    UniformBufferID m_sceneBuffer = EMPTY_BUFFER;
    bool  m_loaded = false;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
class Engine {
public:
    Engine() = default;

    bool init(const std::string& title = "VALO - PBR Showcase", const Vec2u& size = {1280, 720}) {
        /*
            Every pixel of this runs on the CPU, and the frame is fragment bound
            - measured on a 16 core box at ~1MP it is 9 fps at full resolution
            and 16 at 65%, for a picture that is hard to tell apart once it is
            moving. Keys 1-4 override it; 1 is the beauty shot.
        */
        RenderConfig cfg;
        cfg.vsync       = true;
        cfg.renderScale = 0.65f;

        if (!m_window.create(title, size) || !m_valo.init(m_window, cfg)) return false;
        m_running = true;

        // created empty; render() refills it every frame before the first submit
        const SceneUniforms scene{};
        m_sceneBuffer = m_valo.createUniformBuffer(&scene, sizeof(scene));

        buildSky();
        buildFloor();
        buildOrb();
        resetCamera();
        return true;
    }

    // targetSize is the length of the fitted model's longest axis, so every
    // model arrives at roughly the same size however its file was authored
    Model& addModel(const std::string& pathToGltf, const float targetSize = 4.f) {
        m_models.emplace_back();
        Model& model = m_models.back();
        model.setVALO(&m_valo);
        model.setSceneBuffer(m_sceneBuffer);
        model.loadGLTF(pathToGltf);

        if (model.isLoaded()) {
            model.fitToGround(targetSize, kFloorY);
            frameModels();
        }
        return model;
    }

    // the camera framing, the light rig and the contact shadow all key off the
    // fitted size, so they follow whatever the model turned out to be
    void frameModels() {
        Vec3f lo, hi;
        if (!worldBounds(lo, hi)) return;

        m_focus       = (lo + hi) * 0.5f;
        m_focus.y     = lo.y + (hi.y - lo.y) * 0.42f;
        m_orbitRadius = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * 1.55f;
        resetCamera();
    }

    void update() {
        m_running = m_window.pollEvents();
        if (!m_running) return;

        const float dt = float(m_valo.getDeltaTime());
        m_time += dt;

        if (m_window.wasKeyPressed(Key::Escape)) { m_window.close(); m_running = false; return; }
        if (m_window.wasKeyPressed(Key::R))      { m_reflections = !m_reflections; }
        if (m_window.wasKeyPressed(Key::L))      { m_lightsMove  = !m_lightsMove; }
        if (m_window.wasKeyPressed(Key::F))      { m_showFloor   = !m_showFloor; }
        if (m_window.wasKeyPressed(Key::O))      { m_showOrbs    = !m_showOrbs; }
        if (m_window.wasKeyPressed(Key::Num1))   { m_valo.setRenderScale(1.00f); }
        if (m_window.wasKeyPressed(Key::Num2))   { m_valo.setRenderScale(0.75f); }
        if (m_window.wasKeyPressed(Key::Num3))   { m_valo.setRenderScale(0.50f); }
        if (m_window.wasKeyPressed(Key::Num4))   { m_valo.setRenderScale(0.25f); }
        if (m_window.wasKeyPressed(Key::Num5))   { m_valo.setRenderScale(2.00f); }
        if (m_window.wasKeyPressed(Key::Num6))   { m_valo.setRenderScale(3.00f); }
        if (m_window.wasKeyPressed(Key::Num4))   { m_valo.setRenderScale(4.00f); }
        if (m_window.wasKeyPressed(Key::Tab))    { resetCamera(); m_autoOrbit = true; }

        m_exposure = clampf(m_exposure + m_window.scrollDelta() * 0.06f, 0.15f, 6.f);
        if (m_lightsMove) m_lightPhase += dt * 0.35f;
        animateLights();

        const float moveSpeed = (m_window.isKeyDown(Key::LCtrl) ? 9.f : 3.f) * dt;
        Vec3f movement = {0.f, 0.f, 0.f};
        if (m_window.isKeyDown(Key::W))      movement.z =  moveSpeed;
        if (m_window.isKeyDown(Key::S))      movement.z = -moveSpeed;
        if (m_window.isKeyDown(Key::A))      movement.x = -moveSpeed;
        if (m_window.isKeyDown(Key::D))      movement.x =  moveSpeed;
        if (m_window.isKeyDown(Key::LShift)) movement.y = -moveSpeed;
        if (m_window.isKeyDown(Key::Space))  movement.y =  moveSpeed;

        bool justGrabbed = false;
        if (m_window.wasMousePressed(MouseButton::Left)) {
            if (m_window.isMouseGrabbed()) m_window.releaseMouse();
            else { m_window.grabMouse(); justGrabbed = true; m_autoOrbit = false; }
        }
        if (m_window.wasMousePressed(MouseButton::Right)) m_window.releaseMouse();

        if (m_window.isMouseGrabbed()) {
            // the delta is read after pollEvents, and dropped on the frame the
            // grab starts: that frame's motion is the trip to the click, not a look
            m_camera.update(justGrabbed ? Vec2f{0.f, 0.f} : m_window.mouseDelta(), movement);
        } else if (m_autoOrbit) {
            orbitCamera(dt);
        } else {
            m_camera.update({0.f, 0.f}, movement);
        }

        updateTitle(dt);
    }

    void render() {
        if (!m_running) return;

        const Mat4f view = m_camera.view();
        const Mat4f proj = Mat4f::perspective(m_camera.fov, m_window.getAspect(),
                                              m_camera.nearPlane, m_camera.farPlane);
        const Mat4f viewProj = proj * view;
        const bool  aboveFloor = m_camera.pos.y > kFloorY + 0.02f;

        // one upload for the whole frame; every command below binds it
        SceneUniforms scene{};
        for (int i = 0; i < kLightCount; ++i) scene.lights[i] = m_lights[i];
        scene.camPos   = m_camera.pos;
        scene.exposure = m_exposure;
        m_valo.updateUniformBuffer(m_sceneBuffer, &scene, sizeof(scene));

        m_valo.beginFrame();

        drawSky();

        // 2 - the model again, mirrored through the floor. Mirroring reverses
        //     the winding, so the cull flips with it.
        if (m_reflections && m_showFloor && aboveFloor) {
            const Mat4f mirror = Mat4f::translation({0.f, 2.f * kFloorY, 0.f}) * Mat4f::scale({1.f, -1.f, 1.f});
            for (Model& m : m_models) {
                m.draw(viewProj, mirror, m_camera.pos, -1.f, 0.78f, m_emissiveBoost, false, CullMode::Front);
                m.draw(viewProj, mirror, m_camera.pos, -1.f, 0.78f, m_emissiveBoost, true,  CullMode::Front);
            }
        }

        if (m_showFloor) drawFloor(viewProj);

        for (Model& m : m_models)
            m.draw(viewProj, Mat4f::identity(), m_camera.pos, 1.f, 1.f, m_emissiveBoost, false, CullMode::Back);
        for (Model& m : m_models)
            m.draw(viewProj, Mat4f::identity(), m_camera.pos, 1.f, 1.f, m_emissiveBoost, true,  CullMode::Back);

        if (m_showOrbs) drawOrbs(viewProj);

        m_valo.endFrame();
    }

    bool isRunning() const { return m_running; }
    VALO&   getVALO()   { return m_valo; }
    Window& getWindow() { return m_window; }
    Camera& getCamera() { return m_camera; }

private:
    // -----------------------------------------------------------------------
    // the union of every loaded model's fitted bounds; false if nothing loaded
    bool worldBounds(Vec3f& lo, Vec3f& hi) const {
        bool any = false;
        lo = { 1e30f,  1e30f,  1e30f};
        hi = {-1e30f, -1e30f, -1e30f};

        for (const Model& m : m_models) {
            if (!m.isLoaded()) continue;
            Vec3f mlo, mhi;
            m.worldBounds(mlo, mhi);
            lo = {std::min(lo.x, mlo.x), std::min(lo.y, mlo.y), std::min(lo.z, mlo.z)};
            hi = {std::max(hi.x, mhi.x), std::max(hi.y, mhi.y), std::max(hi.z, mhi.z)};
            any = true;
        }
        return any;
    }

    void resetCamera() {
        m_orbitAngle  = 0.72f;
        m_camera.pos  = m_focus + Vec3f{std::sin(m_orbitAngle) * m_orbitRadius,
                                        m_orbitRadius * 0.34f,
                                        std::cos(m_orbitAngle) * m_orbitRadius};
        m_camera.lookAtPoint(m_focus);
    }

    void orbitCamera(const float dt) {
        m_orbitAngle += dt * 0.12f;
        m_camera.pos = m_focus + Vec3f{std::sin(m_orbitAngle) * m_orbitRadius,
                                       m_orbitRadius * (0.30f + 0.06f * std::sin(m_orbitAngle * 0.7f)),
                                       std::cos(m_orbitAngle) * m_orbitRadius};
        m_camera.lookAtPoint(m_focus);
    }

    /*
        A three point rig plus one accent: a warm key that swings across the
        front, a cool fill opposite it, a low warm bounce that picks out the
        sills, and a magenta rim behind the model to separate it from the sky.
    */
    void animateLights() {
        const float a = m_lightPhase;
        const float r = std::max(m_orbitRadius, 3.f);

        m_lights[0] = {{std::sin(a) * r * 0.85f, r * 0.75f, std::cos(a) * r * 0.85f},
                       {1.00f, 0.93f, 0.82f}, 46.f * r};
        m_lights[1] = {{std::sin(a + kPi) * r * 0.9f, r * 0.42f, std::cos(a + kPi) * r * 0.9f},
                       {0.42f, 0.62f, 1.00f}, 26.f * r};
        m_lights[2] = {{std::sin(a * 0.6f + 1.9f) * r * 0.7f, r * 0.13f, std::cos(a * 0.6f + 1.9f) * r * 0.7f},
                       {1.00f, 0.44f, 0.16f}, 15.f * r};
        m_lights[3] = {{std::sin(-a * 0.8f + 3.6f) * r * 0.8f, r * 0.55f, std::cos(-a * 0.8f + 3.6f) * r * 0.8f},
                       {0.85f, 0.25f, 0.95f}, 20.f * r};

        for (Light& l : m_lights) l.pos += Vec3f{m_focus.x, 0.f, m_focus.z};
    }

    // -----------------------------------------------------------------------
    void buildSky() {
        const SkyVertex quad[4] = {{{-1.f, -1.f}}, {{1.f, -1.f}}, {{1.f, 1.f}}, {{-1.f, 1.f}}};
        const uint32_t  idx[6]  = {0, 1, 2, 0, 2, 3};

        m_sky.verts      = m_valo.createVertexBuffer(quad, 4, VtxLayout({VtxAttrib::Vec2f}));
        m_sky.indices    = m_valo.createIndexBuffer(idx, 6);
        m_sky.program    = m_valo.createProgram(skyVs, skyFs);
        m_sky.depthTest  = false;
        m_sky.depthWrite = false;
        m_sky.cullmode   = CullMode::None;
        m_sky.bindUniformBuffer(kSceneSlot, m_sceneBuffer);
    }

    void buildFloor() {
        const float e = 130.f;   // comfortably beyond fadeEnd; the shader discards past it anyway
        const FloorVertex quad[4] = {{{-e, kFloorY, -e}}, {{e, kFloorY, -e}}, {{e, kFloorY, e}}, {{-e, kFloorY, e}}};
        const uint32_t    idx[6]  = {0, 2, 1, 0, 3, 2};

        m_floor.verts      = m_valo.createVertexBuffer(quad, 4, VtxLayout({VtxAttrib::Vec3f}));
        m_floor.indices    = m_valo.createIndexBuffer(idx, 6);
        m_floor.program    = m_valo.createProgram(floorVs, floorFs);
        m_floor.cullmode   = CullMode::None;
        m_floor.blendmode  = BlendMode::Alpha;
        m_floor.depthWrite = true;
        m_floor.bindUniformBuffer(kSceneSlot, m_sceneBuffer);
    }

    void buildOrb() {
        constexpr int stacks = 14, slices = 20;
        std::vector<OrbVertex> verts;
        std::vector<uint32_t>  idx;

        for (int i = 0; i <= stacks; ++i) {
            const float phi = float(i) / stacks * kPi;
            for (int j = 0; j <= slices; ++j) {
                const float theta = float(j) / slices * 2.f * kPi;
                const Vec3f unit = {std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
                verts.push_back({unit, unit});
            }
        }
        const uint32_t stride = slices + 1;
        for (int i = 0; i < stacks; ++i) for (int j = 0; j < slices; ++j) {
            const uint32_t tl = uint32_t(i) * stride + uint32_t(j), tr = tl + 1, bl = tl + stride, br = bl + 1;
            idx.insert(idx.end(), {tl, br, bl, tl, tr, br});
        }

        m_orb.verts      = m_valo.createVertexBuffer(verts.data(), verts.size(),
                                                     VtxLayout({VtxAttrib::Vec3f, VtxAttrib::Vec3f}));
        m_orb.indices    = m_valo.createIndexBuffer(idx.data(), idx.size());
        m_orb.program    = m_valo.createProgram(orbVs, orbFs);
        m_orb.blendmode  = BlendMode::Additive;
        m_orb.depthWrite = false;
        m_orb.cullmode   = CullMode::None;
        m_orb.bindUniformBuffer(kSceneSlot, m_sceneBuffer);
    }

    // -----------------------------------------------------------------------
    void drawSky() {
        SkyUniforms u{};
        u.right      = m_camera.right;
        u.up         = m_camera.up;
        u.forward    = m_camera.forward;
        u.tanHalfFov = std::tan(m_camera.fov * 0.5f);
        u.aspect     = m_window.getAspect();

        m_sky.setUniforms(u);
        m_valo.submit(m_sky);
    }

    void drawFloor(const Mat4f& viewProj) {
        FloorUniforms u{};
        u.viewProj     = viewProj;
        Vec3f lo, hi;
        if (!worldBounds(lo, hi)) { lo = {-1.f, 0.f, -1.f}; hi = {1.f, 1.f, 1.f}; }
        u.shadowCentre = {(lo.x + hi.x) * 0.5f, kFloorY, (lo.z + hi.z) * 0.5f};
        u.shadowRadius = {std::max((hi.x - lo.x) * 0.62f, 0.2f),
                          std::max((hi.z - lo.z) * 0.62f, 0.2f)};
        u.fadeStart    = 18.f;
        u.fadeEnd      = 62.f;
        u.gridSpacing  = 1.f;

        m_floor.setUniforms(u);
        m_valo.submit(m_floor);
    }

    void drawOrbs(const Mat4f& viewProj) {
        for (const Light& l : m_lights) {
            if (l.intensity <= 0.f) continue;

            // core then halo: the halo is a bigger, dimmer shell added on top,
            // which is as close to a bloom as a single pass gets
            constexpr float scale = 0.055f;
            for (int pass = 0; pass < 2; ++pass) {
                OrbUniforms u{};
                u.viewProj   = viewProj;
                u.centre     = l.pos;
                u.colour     = l.colour;
                u.radius     = pass == 0 ? scale : scale * 3.4f;
                u.brightness = pass == 0 ? 34.f  : 2.2f;

                m_orb.setUniforms(u);
                m_valo.submit(m_orb);
            }
        }
    }

    /*
        Measured off a steady_clock rather than getDeltaTime(), which clamps at
        0.1s so a heavy frame cannot run the animation away - feed that to a
        counter and it reads a flat 10 fps for anything slower than 10 fps.
    */
    void updateTitle(const float) {
        const auto now = std::chrono::steady_clock::now();
        m_fpsAccum += std::chrono::duration<float>(now - m_fpsMark).count();
        m_fpsMark = now;
        ++m_fpsFrames;
        if (m_fpsAccum < 0.5f) return;

        const int fps = int(float(m_fpsFrames) / m_fpsAccum + 0.5f);
        m_window.setTitle("VALO - PBR Showcase   |   " + std::to_string(fps) + " fps   |   "
                        + "exposure " + std::to_string(m_exposure).substr(0, 4)
                        + (m_reflections ? "   |   reflections on" : "   |   reflections off"));
        m_fpsAccum = 0.f;
        m_fpsFrames = 0;
    }

    VALO   m_valo;
    Window m_window;
    Camera m_camera;

    std::deque<Model> m_models;
    RenderCommand m_sky, m_floor, m_orb;
    Light m_lights[kLightCount];
    UniformBufferID m_sceneBuffer = EMPTY_BUFFER;

    Vec3f m_focus       = {0.f, 0.8f, 0.f};
    float m_orbitRadius = 7.f;
    float m_orbitAngle  = 0.72f;

    float m_time = 0.f, m_lightPhase = 0.6f;
    float m_exposure = 1.f, m_emissiveBoost = 2.6f;
    float m_fpsAccum = 0.f;
    uint32_t m_fpsFrames = 0;
    std::chrono::steady_clock::time_point m_fpsMark = std::chrono::steady_clock::now();

    bool m_running     = false;
    bool m_reflections = true;
    bool m_showFloor   = true;
    bool m_showOrbs    = true;
    bool m_lightsMove  = true;
    bool m_autoOrbit   = true;
};

}
