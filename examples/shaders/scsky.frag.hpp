#pragma once

#include "scsky.vert.hpp"

namespace sc {

inline void skyFS(const SkyVaryings& in, const SkyUniforms& u) {
    const Vec3f dir = (u.right   * (in.ndc.x * u.tanHalfFov * u.aspect)
                     + u.up      * (in.ndc.y * u.tanHalfFov)
                     + u.forward).normalized();
    valo_FragColor = {tonemap(environment(dir), uniformBuffer<SceneUniforms>(kSceneSlot).exposure), 1.f};
}

// ---------------------------------------------------------------------------
// floor - a glossy plane that fades into the sky, with the mirrored pass
// showing through wherever the fresnel term says the plane is reflective
// ---------------------------------------------------------------------------

VALO_FRAGMENT_SHADER(skyFS)

} // namespace sc
