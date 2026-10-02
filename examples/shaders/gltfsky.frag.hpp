#pragma once

#include "gltfsky.vert.hpp"

inline void gltfSkyFS(const SkyVaryings& in, const SkyUniforms& u) {
    // tanHalf scales NDC to the frustum's half height at z = -1; aspect widens x
    const Vec3f dir = Vec3f{
        in.ndc.x * u.tanHalf * u.aspect,
        in.ndc.y * u.tanHalf,
        -1.f,
    }.normalized();

    valo::valo_FragColor = { toneMap(environmentColour(dir)), 1.f };
}

VALO_FRAGMENT_SHADER(gltfSkyFS)
