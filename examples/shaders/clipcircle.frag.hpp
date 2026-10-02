#pragma once

#include "clipcircle.vert.hpp"

inline void clipFS(const ClipVaryings& in, const ClipUniforms& u) {
    if ((valo::Vec2f{in.ndc.x * u.aspect, in.ndc.y}).length() > u.radius) discard;
    valo::valo_FragColor = {1.f, 1.f, 1.f, 1.f};
}

VALO_FRAGMENT_SHADER(clipFS)
