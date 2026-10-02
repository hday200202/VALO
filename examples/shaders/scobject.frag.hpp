#pragma once

#include "scobject.vert.hpp"

namespace sc {

inline void objectFS(const Varyings& in, const Uniforms& u) {
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

VALO_FRAGMENT_SHADER(objectFS)

} // namespace sc
