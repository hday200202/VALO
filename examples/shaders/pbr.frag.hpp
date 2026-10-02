#pragma once

#include "pbr.vert.hpp"

inline void pbrFS(const Varyings& in, const Uniforms& u) {
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

VALO_FRAGMENT_SHADER(pbrFS)
