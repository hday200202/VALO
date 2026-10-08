#pragma once

#include "scorb.vert.hpp"

namespace sc {

inline void orbFS(const OrbVaryings& in, const OrbUniforms& u) {
    const SceneUniforms& scene = uniformBuffer<SceneUniforms>(SCENE_SLOT);

    const Vec3f v = (scene.camPos - in.worldPos).normalized();
    const float facing = saturate(in.normal.normalized().dot(v));
    // bright core, halo climbing toward the silhouette
    const float halo = std::pow(1.f - facing, 2.2f);
    const Vec3f hdr  = u.colour * (u.brightness * (0.55f + halo * 2.6f));
    valo_FragColor = {tonemap(hdr, scene.exposure), 1.f};
}

VALO_FRAGMENT_SHADER(orbFS)

} // namespace sc
