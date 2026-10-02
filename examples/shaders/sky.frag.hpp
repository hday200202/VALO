#pragma once

#include "sky.vert.hpp"

inline void skyFS(const SkyVaryings& in, const SkyUniforms& u) {
    const Vec3f dir = Vec3f{in.ndc.x * u.tanHalf * u.aspect, in.ndc.y * u.tanHalf, -1.f}.normalized();
    valo::valo_FragColor = {toneMap(environmentColour(dir)), 1.f};
}

VALO_FRAGMENT_SHADER(skyFS)
