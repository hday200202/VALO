#pragma once

#include "gltf.vert.hpp"

inline void gltfFS(const Varyings& in, const Uniforms& u) {
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

VALO_FRAGMENT_SHADER(gltfFS)
