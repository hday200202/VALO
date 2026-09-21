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

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kFovY = 0.9f;
constexpr TextureID kNoTex = UINT32_MAX;

static inline float saturate(float x) { return std::min(std::max(x, 0.f), 1.f); }
static inline Vec3f srgbToLinear(const Vec3f& c) { return {std::pow(c.x, 2.2f), std::pow(c.y, 2.2f), std::pow(c.z, 2.2f)}; }

// ---------------------------------------------------------------------------
// lights + environment (same as pbr.cpp); positions are world space since the
// camera is fixed at +z looking at the origin
// ---------------------------------------------------------------------------
struct Light { Vec3f pos; Vec3f colour; float intensity; };
static const Light kLights[4] = {
    { { -3.5f,  3.5f,  3.5f }, { 1.00f, 0.96f, 0.90f }, 52.0f },
    { {  4.0f,  0.5f,  2.5f }, { 0.55f, 0.68f, 1.00f }, 26.0f },
    { { -1.5f, -3.0f,  2.0f }, { 1.00f, 0.55f, 0.32f }, 16.0f },
    { {  1.0f,  1.5f, -4.0f }, { 0.85f, 0.88f, 1.00f }, 34.0f },
};
constexpr Vec3f kSkyTop = {0.012f, 0.035f, 0.120f}, kSkyHorizon = {0.115f, 0.125f, 0.145f}, kSkyGround = {0.020f, 0.020f, 0.023f};
constexpr Vec3f kSunColour = {1.f, 0.945f, 0.855f};
static const Vec3f kSunDir = Vec3f{-0.52f, 0.62f, 0.59f}.normalized();

static Vec3f skyColour(const Vec3f& d) {
    const float y = std::clamp(d.y, -1.f, 1.f);
    return y >= 0.f ? lerp(kSkyHorizon, kSkyTop, std::pow(y, 0.5f)) : lerp(kSkyHorizon, kSkyGround, std::pow(-y, 0.5f));
}
static Vec3f environmentColour(const Vec3f& d) {
    const float s = saturate(d.normalized().dot(kSunDir));
    return skyColour(d) + kSunColour * (std::pow(s, 900.f) * 14.f + std::pow(s, 12.f) * 0.30f);
}
static const Vec3f kAmbientUp = skyColour({0, 1, 0}), kAmbientDown = skyColour({0, -1, 0});

// ---------------------------------------------------------------------------
// BRDF
// ---------------------------------------------------------------------------
static float distributionGGX(float nDotH, float r) { const float a2 = r*r*r*r, d = nDotH*nDotH*(a2-1.f)+1.f; return a2 / std::max(kPi*d*d, 1e-7f); }
static float geometrySchlick(float nDotX, float r) { const float k = (r+1.f)*(r+1.f)/8.f; return nDotX / std::max(nDotX*(1.f-k)+k, 1e-7f); }
static Vec3f fresnelSchlick(float c, const Vec3f& f0) { const float f = std::pow(1.f - saturate(c), 5.f); return f0 + (Vec3f{1,1,1} - f0) * f; }
static Vec3f fresnelSchlickRoughness(float c, const Vec3f& f0, float r) {
    const float f = std::pow(1.f - saturate(c), 5.f), i = 1.f - r;
    return f0 + (Vec3f{std::max(i, f0.x), std::max(i, f0.y), std::max(i, f0.z)} - f0) * f;
}
static Vec3f toneMap(const Vec3f& c) { auto e = [](float v) { return std::pow(saturate(v / (1.f + v)), 1.f / 2.2f); }; return {e(c.x), e(c.y), e(c.z)}; }

// ---------------------------------------------------------------------------
// shaders
// ---------------------------------------------------------------------------
struct Vertex { Vec3f pos; Vec3f normal; Vec2f uv; Vec4f tangent; };   // tangent.w = handedness

struct Uniforms {
    Mat4f mvp, model;
    Vec3f camPos;
    Vec4f baseColor; float metallic, roughness; Vec3f emissive;
    TextureID baseTex, mrTex, normalTex, aoTex, emissiveTex;   // kNoTex when absent
    bool useNormalMap;
};

struct Varyings { Vec3f worldPos; Vec3f normal; Vec2f uv; Vec4f tangent; };

static void vs(const Vertex& in, const Uniforms& u, Varyings& out) {
    out.worldPos = (u.model * Vec4f(in.pos, 1.f)).xyz;
    out.normal   = (u.model * Vec4f(in.normal, 0.f)).xyz;
    out.tangent  = {(u.model * Vec4f(in.tangent.xyz, 0.f)).xyz, in.tangent.w};
    out.uv       = in.uv;
    valo::valo_Position = u.mvp * Vec4f(in.pos, 1.f);
}

static void fs(const Varyings& in, const Uniforms& u) {
    const valo::Sampler samp;   // repeat + linear

    // --- material inputs ---
    Vec4f base = u.baseColor;
    if (u.baseTex != kNoTex) { Color t = texture(u.baseTex, in.uv, samp); base = base * Vec4f(srgbToLinear(t.xyz), t.w); }
    float metallic = u.metallic, roughness = u.roughness;
    if (u.mrTex != kNoTex) { Color t = texture(u.mrTex, in.uv, samp); roughness *= t.y; metallic *= t.z; }
    roughness = std::clamp(roughness, 0.035f, 1.f);
    float ao = 1.f;
    if (u.aoTex != kNoTex) ao = texture(u.aoTex, in.uv, samp).x;
    Vec3f emissive = u.emissive;
    if (u.emissiveTex != kNoTex) emissive = emissive * srgbToLinear(texture(u.emissiveTex, in.uv, samp).xyz);

    // --- normal: TBN from the interpolated tangent, or just the vertex normal ---
    Vec3f n = in.normal.normalized();
    if (u.useNormalMap && u.normalTex != kNoTex) {
        Vec3f t = in.tangent.xyz;
        t = (t - n * n.dot(t)).normalized();                    // re-orthogonalise after interpolation
        const Vec3f b = n.cross(t) * in.tangent.w;
        const Color s = texture(u.normalTex, in.uv, samp);
        const Vec3f tn = Vec3f{s.x, s.y, s.z} * 2.f - Vec3f{1.f, 1.f, 1.f};
        n = (t * tn.x + b * tn.y + n * tn.z).normalized();
    }

    // --- Cook-Torrance ---
    const Vec3f v = (u.camPos - in.worldPos).normalized();
    const float nDotV = std::max(n.dot(v), 1e-4f);
    const Vec3f albedo = base.xyz;
    const Vec3f f0 = lerp(Vec3f{0.04f, 0.04f, 0.04f}, albedo, metallic);

    Vec3f radiance = {0, 0, 0};
    for (const Light& L : kLights) {
        const Vec3f toL = L.pos - in.worldPos; const float dist = toL.length();
        const Vec3f l = toL * (1.f / dist);
        const float nDotL = n.dot(l); if (nDotL <= 0.f) continue;
        const Vec3f h = (l + v).normalized();
        const float d = distributionGGX(std::max(n.dot(h), 0.f), roughness);
        const float g = geometrySchlick(nDotL, roughness) * geometrySchlick(nDotV, roughness);
        const Vec3f f = fresnelSchlick(std::max(v.dot(h), 0.f), f0);
        const Vec3f spec = f * (d * g / std::max(4.f * nDotL * nDotV, 1e-5f));
        const Vec3f diff = albedo * ((1.f / kPi) * (1.f - f.x) * (1.f - metallic));
        radiance += (diff + spec) * L.colour * (L.intensity / (dist * dist)) * nDotL;
    }

    const Vec3f irradiance  = lerp(kAmbientDown, kAmbientUp, n.y * 0.5f + 0.5f);
    const Vec3f ambDiffuse  = albedo * irradiance * ((1.f - metallic) * 0.25f);
    const Vec3f refl        = (n * (2.f * n.dot(v)) - v).normalized();
    const Vec3f ambSpecular = environmentColour(refl) * fresnelSchlickRoughness(nDotV, f0, roughness) * (1.f - roughness * 0.7f);

    valo::valo_FragColor = {toneMap(radiance + (ambDiffuse + ambSpecular) * ao + emissive * 2.f), 1.f};
}

struct SkyVertex { Vec2f ndc; }; struct SkyUniforms { float aspect, tanHalf; }; struct SkyVaryings { Vec2f ndc; };
static void skyVs(const SkyVertex& in, const SkyUniforms&, SkyVaryings& out) { out.ndc = in.ndc; valo::valo_Position = {in.ndc.x, in.ndc.y, 0.9999f, 1.f}; return; }
static void skyFs(const SkyVaryings& in, const SkyUniforms& u) {
    valo::valo_FragColor = {toneMap(environmentColour(Vec3f{in.ndc.x * u.tanHalf * u.aspect, in.ndc.y * u.tanHalf, -1.f}.normalized())), 1.f};
}

// ---------------------------------------------------------------------------
// loading: glTF -> vertex buffer (with tangents) + one RenderCommand per primitive per node
// ---------------------------------------------------------------------------
struct DrawItem { valo::RenderCommand cmd; Mat4f world; Uniforms uniforms; };
struct Model {
    std::string name;
    valo::VertexBufferID vb; valo::IndexBufferID ib;
    std::vector<DrawItem> items;
    Vec3f center; float extent;
};

// per-triangle tangents from uv derivatives, accumulated per vertex (Lengyel)
static void computeTangents(std::vector<Vertex>& verts, const std::vector<uint32_t>& idx) {
    std::vector<Vec3f> tan(verts.size(), {0, 0, 0}), bit(verts.size(), {0, 0, 0});
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const Vertex &a = verts[idx[i]], &b = verts[idx[i + 1]], &c = verts[idx[i + 2]];
        const Vec3f e1 = b.pos - a.pos, e2 = c.pos - a.pos;
        const Vec2f d1 = b.uv - a.uv,   d2 = c.uv - a.uv;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;
        const float r = 1.f / det;
        const Vec3f t = (e1 * d2.y - e2 * d1.y) * r, bt = (e2 * d1.x - e1 * d2.x) * r;
        for (int k = 0; k < 3; ++k) { tan[idx[i + k]] += t; bit[idx[i + k]] += bt; }
    }
    for (size_t i = 0; i < verts.size(); ++i) {
        const Vec3f n = verts[i].normal;
        Vec3f t = tan[i] - n * n.dot(tan[i]);
        if (t.length() < 1e-8f) { t = std::fabs(n.x) < 0.9f ? Vec3f{1, 0, 0} : Vec3f{0, 1, 0}; t = t - n * n.dot(t); }
        t = t.normalized();
        const float w = n.cross(t).dot(bit[i]) < 0.f ? -1.f : 1.f;
        verts[i].tangent = {t, w};
    }
}

static Model loadModel(valo::VALO& valo, const std::string& path, const std::string& name, std::map<std::string, TextureID>& texCache) {
    Model model; model.name = name;
    valo::GLTF g = valo::parseGLTF(path);
    if (g.vertices.empty()) return model;

    std::vector<Vertex> verts(g.vertices.size());
    for (size_t i = 0; i < verts.size(); ++i) verts[i] = {g.vertices[i].pos, g.vertices[i].normal, g.vertices[i].uv, {1, 0, 0, 1}};
    computeTangents(verts, g.indices);

    valo::VtxLayout layout({valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec3f, valo::VtxAttrib::Vec2f, valo::VtxAttrib::Vec4f});
    model.vb = valo.createVertexBuffer(verts.data(), verts.size(), layout);
    model.ib = valo.createIndexBuffer(g.indices.data(), g.indices.size());

    auto tex = [&](const std::string& p) -> TextureID {
        if (p.empty()) return kNoTex;
        auto it = texCache.find(p); if (it != texCache.end()) return it->second;
        return texCache[p] = valo.createTexture(p);
    };

    Vec3f lo = {1e9f, 1e9f, 1e9f}, hi = {-1e9f, -1e9f, -1e9f};
    for (const valo::GLTFNode& node : g.nodes) {
        for (const valo::GLTFPrimitive& prim : g.meshes[node.mesh].primitives) {
            DrawItem item; item.world = node.world;
            item.cmd.verts = model.vb; item.cmd.indices = model.ib;
            item.cmd.firstIndex = prim.firstIndex; item.cmd.indexCount = prim.indexCount;
            Uniforms& u = item.uniforms;
            if (prim.material >= 0) {
                const valo::GLTFMaterial& m = g.materials[prim.material];
                u.baseColor = m.baseColor; u.metallic = m.metallic; u.roughness = m.roughness; u.emissive = m.emissive;
                u.baseTex = tex(m.baseColorTex); u.mrTex = tex(m.metallicRoughnessTex); u.normalTex = tex(m.normalTex);
                u.aoTex = tex(m.occlusionTex); u.emissiveTex = tex(m.emissiveTex);
            } else {
                u.baseColor = {0.8f, 0.8f, 0.8f, 1.f}; u.metallic = 0.f; u.roughness = 0.6f; u.emissive = {0, 0, 0};
                u.baseTex = u.mrTex = u.normalTex = u.aoTex = u.emissiveTex = kNoTex;
            }
            model.items.push_back(item);
            for (uint32_t k = prim.firstIndex; k < prim.firstIndex + prim.indexCount; ++k) {
                const Vec3f p = (node.world * Vec4f(verts[g.indices[k]].pos, 1.f)).xyz;
                lo = min(lo, p); hi = max(hi, p);
            }
        }
    }
    model.center = (lo + hi) * 0.5f;
    model.extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    return model;
}

// ---------------------------------------------------------------------------
int main() {
    valo::Window window("VALO - glTF", {1280, 960});
    valo::VALO valo; valo::RenderConfig cfg; cfg.vsync = true;
    if (!valo.init(window, cfg)) return 1;

    std::map<std::string, TextureID> texCache;
    Model models[2] = {
        loadModel(valo, "examples/gltf/DamagedHelmet/DamagedHelmet.gltf", "DamagedHelmet", texCache),
        loadModel(valo, "examples/gltf/Box/Box.gltf", "Box", texCache),
    };
    if (models[0].items.empty() && models[1].items.empty()) return 1;
    const valo::ProgramID prog = valo.createProgram(vs, fs);

    SkyVertex quad[4] = {{{-1, -1}}, {{1, -1}}, {{1, 1}}, {{-1, 1}}}; uint32_t quadIdx[6] = {0, 1, 2, 0, 2, 3};
    valo::RenderCommand sky;
    sky.verts = valo.createVertexBuffer(quad, 4, valo::VtxLayout({valo::VtxAttrib::Vec2f}));
    sky.indices = valo.createIndexBuffer(quadIdx, 6); sky.program = valo.createProgram(skyVs, skyFs);
    sky.depthTest = false; sky.depthWrite = false; sky.cullmode = valo::CullMode::None;

    int which = 0; bool useNormalMap = true; float zoom = 1.f;
    Mat4f orientation;
    using clk = std::chrono::steady_clock; auto fpsStart = clk::now(); uint32_t fpsFrames = 0; int fps = 0;

    while (window.pollEvents()) {
        if (window.wasKeyPressed(valo::Key::Escape)) window.close();
        if (window.wasKeyPressed(valo::Key::Tab)) which = 1 - which;
        if (window.wasKeyPressed(valo::Key::N)) useNormalMap = !useNormalMap;
        if (window.wasMousePressed(valo::MouseButton::Left)) window.grabMouse();
        if (window.wasMousePressed(valo::MouseButton::Right)) window.releaseMouse();
        if (window.isMouseGrabbed()) { const Vec2f d = window.mouseDelta(); orientation = Mat4f::rotationX(d.y * 0.005f) * Mat4f::rotationY(d.x * 0.005f) * orientation; }
        zoom = std::clamp(zoom - window.scrollDelta() * 0.05f, 0.3f, 3.f);
        if (window.wasKeyPressed(valo::Key::Num1)) valo.setRenderScale(1.f);
        if (window.wasKeyPressed(valo::Key::Num2)) valo.setRenderScale(0.75f);
        if (window.wasKeyPressed(valo::Key::Num3)) valo.setRenderScale(0.5f);
        if (window.wasKeyPressed(valo::Key::Num4)) valo.setRenderScale(0.25f);

        ++fpsFrames; const auto now = clk::now();
        if (std::chrono::duration<float>(now - fpsStart).count() >= 1.f) { fps = int(fpsFrames); fpsFrames = 0; fpsStart = now; }
        const Model& model = models[which];
        window.setTitle("VALO - glTF   " + model.name + "   " + std::to_string(model.items.size()) + " draw(s)" + (useNormalMap ? "" : "   [no normal map]") + "   " + std::to_string(fps) + " fps");

        // fit: scale the model so its largest extent is ~2 units, centered at the origin
        const float fit = 2.f / std::max(model.extent, 1e-6f);
        const Vec3f camPos = {0.f, 0.f, 4.2f * zoom};
        const Mat4f view = Mat4f::lookAt(camPos, {0, 0, 0}, {0, 1, 0});
        const float aspect = window.getAspect();
        const Mat4f proj = Mat4f::perspective(kFovY, aspect, 0.1f, 100.f);
        const Mat4f base = orientation * Mat4f::scale({fit, fit, fit}) * Mat4f::translation(-model.center);

        valo.beginFrame();
        sky.setUniforms(SkyUniforms{aspect, std::tan(kFovY * 0.5f)});
        valo.submit(sky);
        for (const DrawItem& item : model.items) {
            Uniforms u = item.uniforms;
            u.model = base * item.world; u.mvp = proj * view * u.model; u.camPos = camPos; u.useNormalMap = useNormalMap;
            valo::RenderCommand cmd = item.cmd; cmd.program = prog; cmd.setUniforms(u);
            valo.submit(cmd);
        }
        valo.endFrame();
    }
    return 0;
}
