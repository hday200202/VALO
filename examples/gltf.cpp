// glTF viewer: DamagedHelmet (the PBR reference model) or the Khronos Box,
// through a textured Cook-Torrance shader. Every glTF material slot is used:
// base color, packed metallic-roughness (G = roughness, B = metallic), normal
// map (tangents computed at load), occlusion, emissive. Lighting and sky are
// the same four lights + environment as pbr.cpp.
//
// Controls
//   left click    grab mouse and drag to rotate; right click releases
//   tab           switch model (helmet / box)
//   wheel         zoom
//   n             normal map on / off
//   1 2 3 4       render scale
//   escape        quit

#include <VALO.hpp>
#include <utils/GLTFParser.hpp>
#include <cmath>
#include <numbers>
#include <chrono>
#include <string>
#include <vector>
#include <map>
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

static void vs(const Vertex& in, const Uniforms& u, Varyings& out) {
    // Positions transform with w = 1 so they pick up the translation; normals
    // and tangents are directions, so they go through with w = 0 instead.
    out.worldPos = (u.model * Vec4f(in.pos,    1.f)).xyz;
    out.normal   = (u.model * Vec4f(in.normal, 0.f)).xyz;
    out.tangent  = { (u.model * Vec4f(in.tangent.xyz, 0.f)).xyz, in.tangent.w };
    out.uv       = in.uv;

    valo::valo_Position = u.mvp * Vec4f(in.pos, 1.f);
}

static void fs(const Varyings& in, const Uniforms& u) {
    const valo::Sampler samp;   // repeat + linear

    /* --- material inputs: constant, multiplied by the map when there is one -- */
    Vec4f base = u.baseColor;
    if (u.baseTex != kNoTex) {
        const Color t = texture(u.baseTex, in.uv, samp);
        base = base * Vec4f(srgbToLinear(t.xyz), t.w);
    }

    float metallic  = u.metallic;
    float roughness = u.roughness;
    if (u.mrTex != kNoTex) {
        const Color t = texture(u.mrTex, in.uv, samp);
        roughness *= t.y;       // glTF packs roughness in green
        metallic  *= t.z;       // and metallic in blue
    }

    // A perfectly smooth surface makes the specular denominator explode, so the
    // roughness floor is a numerical guard as much as an artistic one.
    roughness = std::clamp(roughness, 0.035f, 1.f);

    float ao = 1.f;
    if (u.aoTex != kNoTex) {
        ao = texture(u.aoTex, in.uv, samp).x;
    }

    Vec3f emissive = u.emissive;
    if (u.emissiveTex != kNoTex) {
        emissive = emissive * srgbToLinear(texture(u.emissiveTex, in.uv, samp).xyz);
    }

    /* --- normal: build a TBN frame, or fall back to the vertex normal --------

        A normal map stores directions in tangent space - relative to the
        surface - so it needs a basis to be rotated into world space. That basis
        is the normal, the tangent (the direction u increases in), and their
        cross product. Interpolation across the triangle leaves the tangent
        slightly off perpendicular, so it is re-orthogonalised first.
    */
    Vec3f n = in.normal.normalized();

    if (u.useNormalMap && u.normalTex != kNoTex) {
        Vec3f t = in.tangent.xyz;
        t = (t - n * n.dot(t)).normalized();                // Gram-Schmidt
        const Vec3f b = n.cross(t) * in.tangent.w;          // w flips mirrored UVs

        const Color s  = texture(u.normalTex, in.uv, samp);
        const Vec3f tn = Vec3f{s.x, s.y, s.z} * 2.f - Vec3f{1.f, 1.f, 1.f};   // 0..1 -> -1..1

        n = (t * tn.x + b * tn.y + n * tn.z).normalized();
    }

    /* --- direct lighting ---------------------------------------------------- */
    const Vec3f v      = (u.camPos - in.worldPos).normalized();
    const float nDotV  = std::max(n.dot(v), 1e-4f);
    const Vec3f albedo = base.xyz;

    // Metals have no diffuse and tint their reflection; dielectrics reflect a
    // flat 4% and keep their colour in the diffuse term. metallic blends between.
    const Vec3f f0 = lerp(Vec3f{0.04f, 0.04f, 0.04f}, albedo, metallic);

    Vec3f radiance = {0.f, 0.f, 0.f};

    for (const Light& light : kLights) {
        const Vec3f toLight = light.pos - in.worldPos;
        const float dist    = toLight.length();
        const Vec3f l       = toLight * (1.f / dist);

        const float nDotL = n.dot(l);
        if (nDotL <= 0.f) continue;                         // facing away

        const Vec3f h = (l + v).normalized();               // half vector

        const float d = distributionGGX(std::max(n.dot(h), 0.f), roughness);
        const float g = geometrySchlick(nDotL, roughness) * geometrySchlick(nDotV, roughness);
        const Vec3f f = fresnelSchlick(std::max(v.dot(h), 0.f), f0);

        const Vec3f specular = f * (d * g / std::max(4.f * nDotL * nDotV, 1e-5f));

        // whatever is not reflected (1 - f) and not metal can scatter diffusely
        const Vec3f diffuse  = albedo * ((1.f / kPi) * (1.f - f.x) * (1.f - metallic));

        // inverse square falloff, then Lambert's cosine term
        const Vec3f incoming = light.colour * (light.intensity / (dist * dist));
        radiance += (diffuse + specular) * incoming * nDotL;
    }

    /* --- environment: a stand-in for image based lighting -------------------

        Proper IBL prefilters the environment into mip levels. With one sample
        available, the diffuse half becomes a hemisphere lerp and the specular
        half a single lookup along the reflection vector, dimmed by roughness.
    */
    const Vec3f irradiance  = lerp(kAmbientDown, kAmbientUp, n.y * 0.5f + 0.5f);
    const Vec3f ambDiffuse  = albedo * irradiance * ((1.f - metallic) * 0.25f);

    const Vec3f refl        = (n * (2.f * n.dot(v)) - v).normalized();
    const Vec3f ambSpecular = environmentColour(refl)
                            * fresnelSchlickRoughness(nDotV, f0, roughness)
                            * (1.f - roughness * 0.7f);

    const Vec3f hdr = radiance + (ambDiffuse + ambSpecular) * ao + emissive * 2.f;

    valo::valo_FragColor = { toneMap(hdr), 1.f };
}


/* ===========================================================================
    Sky

    A fullscreen quad pinned to the far plane. The fragment shader turns each
    NDC position back into a camera ray and asks the environment what colour it
    sees. Nothing is transformed, so the sky costs one quad.
=========================================================================== */
struct SkyVertex   { Vec2f ndc; };
struct SkyUniforms { float aspect, tanHalf; };
struct SkyVaryings { Vec2f ndc; };

static void skyVs(const SkyVertex& in, const SkyUniforms&, SkyVaryings& out) {
    out.ndc = in.ndc;
    valo::valo_Position = { in.ndc.x, in.ndc.y, 0.9999f, 1.f };   // just inside the far plane
}

static void skyFs(const SkyVaryings& in, const SkyUniforms& u) {
    // tanHalf scales NDC to the frustum's half height at z = -1; aspect widens x
    const Vec3f dir = Vec3f{
        in.ndc.x * u.tanHalf * u.aspect,
        in.ndc.y * u.tanHalf,
        -1.f,
    }.normalized();

    valo::valo_FragColor = { toneMap(environmentColour(dir)), 1.f };
}


/* ===========================================================================
    Loading: glTF -> one vertex buffer + one draw per primitive per node
=========================================================================== */
struct DrawItem {
    valo::RenderCommand cmd;
    Mat4f    world;         // node's world transform, baked at load
    Uniforms uniforms;      // everything material-side, filled once
};

struct Model {
    std::string name;
    valo::VertexBufferID vb;
    valo::IndexBufferID  ib;
    std::vector<DrawItem> items;
    Vec3f center;           // of the world-space bounds, for framing
    float extent;
};

/*
    Tangents from UV derivatives (Lengyel's method).

    A normal map needs to know which way is "u" on the surface. glTF only ships
    that when the exporter felt like it - DamagedHelmet does not - so it is
    derived: for each triangle, solve for the direction in which u increases,
    accumulate it onto all three vertices, then normalise at the end. Summing
    per-vertex is what makes the result continuous across shared edges.
*/
static void computeTangents(std::vector<Vertex>& verts, const std::vector<uint32_t>& idx) {
    std::vector<Vec3f> tan(verts.size(), {0.f, 0.f, 0.f});
    std::vector<Vec3f> bit(verts.size(), {0.f, 0.f, 0.f});

    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const Vertex& a = verts[idx[i]];
        const Vertex& b = verts[idx[i + 1]];
        const Vertex& c = verts[idx[i + 2]];

        const Vec3f e1 = b.pos - a.pos;
        const Vec3f e2 = c.pos - a.pos;
        const Vec2f d1 = b.uv  - a.uv;
        const Vec2f d2 = c.uv  - a.uv;

        // the 2x2 UV matrix is singular on degenerate or unmapped triangles
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;

        const float r = 1.f / det;
        const Vec3f t  = (e1 * d2.y - e2 * d1.y) * r;
        const Vec3f bt = (e2 * d1.x - e1 * d2.x) * r;

        for (int k = 0; k < 3; ++k) {
            tan[idx[i + k]] += t;
            bit[idx[i + k]] += bt;
        }
    }

    for (size_t i = 0; i < verts.size(); ++i) {
        const Vec3f n = verts[i].normal;

        // remove any component along the normal, so t lies in the surface
        Vec3f t = tan[i] - n * n.dot(tan[i]);

        // a vertex no triangle contributed to (or a fully degenerate one) needs
        // some valid frame rather than a zero vector
        if (t.length() < 1e-8f) {
            t = std::fabs(n.x) < 0.9f ? Vec3f{1.f, 0.f, 0.f} : Vec3f{0.f, 1.f, 0.f};
            t = t - n * n.dot(t);
        }
        t = t.normalized();

        // handedness: does the bitangent we derived agree with n x t, or is the
        // UV island mirrored? The shader multiplies by this to match.
        const float w = n.cross(t).dot(bit[i]) < 0.f ? -1.f : 1.f;

        verts[i].tangent = { t, w };
    }
}

static Model loadModel(
    valo::VALO& valo,
    const std::string& path,
    const std::string& name,
    std::map<std::string, TextureID>& texCache
) {
    Model model;
    model.name = name;

    valo::GLTF g = valo::parseGLTF(path);
    if (g.vertices.empty()) return model;

    // the parser hands back pos/normal/uv; this example's vertex adds a tangent
    std::vector<Vertex> verts(g.vertices.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        verts[i] = { g.vertices[i].pos, g.vertices[i].normal, g.vertices[i].uv, {1.f, 0.f, 0.f, 1.f} };
    }
    computeTangents(verts, g.indices);

    const valo::VtxLayout layout({
        valo::VtxAttrib::Vec3f,     // pos
        valo::VtxAttrib::Vec3f,     // normal
        valo::VtxAttrib::Vec2f,     // uv
        valo::VtxAttrib::Vec4f,     // tangent
    });

    // one buffer for the whole file; primitives become index ranges into it
    model.vb = valo.createVertexBuffer(verts.data(), verts.size(), layout);
    model.ib = valo.createIndexBuffer(g.indices.data(), g.indices.size());

    // materials share images constantly, and the cache is keyed on path so the
    // same file is decoded once even across the two models
    auto tex = [&](const std::string& p) -> TextureID {
        if (p.empty()) return kNoTex;

        const auto it = texCache.find(p);
        if (it != texCache.end()) return it->second;

        return texCache[p] = valo.createTexture(p);
    };

    Vec3f lo = { 1e9f,  1e9f,  1e9f};
    Vec3f hi = {-1e9f, -1e9f, -1e9f};

    for (const valo::GLTFNode& node : g.nodes) {
        for (const valo::GLTFPrimitive& prim : g.meshes[node.mesh].primitives) {
            DrawItem item;
            item.world = node.world;

            item.cmd.verts      = model.vb;
            item.cmd.indices    = model.ib;
            item.cmd.firstIndex = prim.firstIndex;
            item.cmd.indexCount = prim.indexCount;

            Uniforms& u = item.uniforms;

            if (prim.material >= 0) {
                const valo::GLTFMaterial& m = g.materials[prim.material];

                u.baseColor = m.baseColor;
                u.metallic  = m.metallic;
                u.roughness = m.roughness;
                u.emissive  = m.emissive;

                u.baseTex     = tex(m.baseColorTex);
                u.mrTex       = tex(m.metallicRoughnessTex);
                u.normalTex   = tex(m.normalTex);
                u.aoTex       = tex(m.occlusionTex);
                u.emissiveTex = tex(m.emissiveTex);
            } else {
                // glTF allows a primitive with no material at all
                u.baseColor = {0.8f, 0.8f, 0.8f, 1.f};
                u.metallic  = 0.f;
                u.roughness = 0.6f;
                u.emissive  = {0.f, 0.f, 0.f};

                u.baseTex = u.mrTex = u.normalTex = u.aoTex = u.emissiveTex = kNoTex;
            }

            model.items.push_back(item);

            // bounds in world space, so the framing below works whatever units
            // and placement the file happened to use
            for (uint32_t k = prim.firstIndex; k < prim.firstIndex + prim.indexCount; ++k) {
                const Vec3f p = (node.world * Vec4f(verts[g.indices[k]].pos, 1.f)).xyz;
                lo = min(lo, p);
                hi = max(hi, p);
            }
        }
    }

    model.center = (lo + hi) * 0.5f;
    model.extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});

    return model;
}


/* ===========================================================================
    Main
=========================================================================== */
int main() {
    valo::Window window("VALO - glTF", {1280, 960});

    valo::VALO valo;
    valo::RenderConfig cfg;
    cfg.vsync = true;
    if (!valo.init(window, cfg)) return 1;

    std::map<std::string, TextureID> texCache;
    Model models[2] = {
        loadModel(valo, "examples/gltf/DamagedHelmet/DamagedHelmet.gltf", "DamagedHelmet", texCache),
        loadModel(valo, "examples/gltf/Box/Box.gltf",                     "Box",           texCache),
    };
    if (models[0].items.empty() && models[1].items.empty()) return 1;

    const valo::ProgramID prog = valo.createProgram(vs, fs);

    // --- sky: one quad, never depth tested, drawn first ---
    SkyVertex quad[4]   = { {{-1.f, -1.f}}, {{1.f, -1.f}}, {{1.f, 1.f}}, {{-1.f, 1.f}} };
    uint32_t  quadIdx[6] = { 0, 1, 2, 0, 2, 3 };

    valo::RenderCommand sky;
    sky.verts      = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    sky.indices    = valo.createIndexBuffer(quadIdx, 6);
    sky.program    = valo.createProgram(skyVs, skyFs);
    sky.depthTest  = false;
    sky.depthWrite = false;
    sky.cullmode   = valo::CullMode::None;

    int   which        = 0;
    bool  useNormalMap = true;
    float zoom         = 1.f;
    Mat4f orientation;

    using clk = std::chrono::steady_clock;
    auto     fpsStart  = clk::now();
    uint32_t fpsFrames = 0;
    int      fps       = 0;

    while (window.pollEvents()) {
        /* --- input --- */
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::Tab))    which = 1 - which;
        if (window.wasKeyPressed(valo::Key::N))      useNormalMap = !useNormalMap;
        if (window.wasKeyPressed(valo::Key::V))      valo.setVsync(!valo.getVsync());

        if (window.wasMousePressed(valo::MouseButton::Left))  window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();

        if (window.isMouseGrabbed()) {
            // pre-multiplied, so each drag rotates about the SCREEN axes rather
            // than the model's own - which is what dragging an object feels like
            const Vec2f d = window.mouseDelta();
            orientation = Mat4f::rotationX(d.y * 0.005f)
                        * Mat4f::rotationY(d.x * 0.005f)
                        * orientation;
        }

        zoom = std::clamp(zoom - window.scrollDelta() * 0.05f, 0.3f, 3.f);

        if (window.wasKeyPressed(valo::Key::Num1)) valo.setRenderScale(1.00f);
        if (window.wasKeyPressed(valo::Key::Num2)) valo.setRenderScale(0.75f);
        if (window.wasKeyPressed(valo::Key::Num3)) valo.setRenderScale(0.50f);
        if (window.wasKeyPressed(valo::Key::Num4)) valo.setRenderScale(0.25f);

        /* --- title --- */
        ++fpsFrames;
        const auto now = clk::now();
        if (std::chrono::duration<float>(now - fpsStart).count() >= 1.f) {
            fps       = int(fpsFrames);
            fpsFrames = 0;
            fpsStart  = now;
        }

        const Model& model = models[which];
        window.setTitle(
            "VALO - glTF   " + model.name +
            "   " + std::to_string(model.items.size()) + " draw(s)" +
            (useNormalMap ? "" : "   [no normal map]") +
            "   " + std::to_string(fps) + " fps"
        );

        /* --- camera ---

            Fit rather than trust the file: scale so the largest extent is about
            2 units and move the centre to the origin. glTF files disagree wildly
            on scale, so without this one model fills the screen and the next is
            a speck.
        */
        const float fit    = 2.f / std::max(model.extent, 1e-6f);
        const Vec3f camPos = { 0.f, 0.f, 4.2f * zoom };

        const Mat4f view = Mat4f::lookAt(camPos, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
        const float aspect = window.getAspect();
        const Mat4f proj = Mat4f::perspective(kFovY, aspect, 0.1f, 100.f);

        // read right to left: centre it, scale it, then apply the mouse rotation
        const Mat4f base = orientation
                         * Mat4f::scale({fit, fit, fit})
                         * Mat4f::translation(-model.center);

        /* --- draw --- */
        valo.beginFrame();

        sky.setUniforms(SkyUniforms{aspect, std::tan(kFovY * 0.5f)});
        valo.submit(sky);

        for (const DrawItem& item : model.items) {
            Uniforms u = item.uniforms;              // material, filled at load
            u.model        = base * item.world;      // node placement, then framing
            u.mvp          = proj * view * u.model;
            u.camPos       = camPos;
            u.useNormalMap = useNormalMap;

            valo::RenderCommand cmd = item.cmd;
            cmd.program = prog;
            cmd.setUniforms(u);
            valo.submit(cmd);
        }

        valo.endFrame();
    }

    return 0;
}
