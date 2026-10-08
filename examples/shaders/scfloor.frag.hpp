#pragma once

#include "scfloor.vert.hpp"

namespace sc {

inline void floorFS(const FloorVaryings& in, const FloorUniforms& u) {
    const SceneUniforms& scene = uniformBuffer<SceneUniforms>(SCENE_SLOT);

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
    for (int i = 0; i < LIGHT_COUNT; ++i) {
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
        const Vec3f diff  = albedo * ((1.f / PI) * occl);

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

VALO_FRAGMENT_SHADER(floorFS)

} // namespace sc
