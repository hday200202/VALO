#pragma once

#include "galaxy.vert.hpp"

inline void galaxyFS(const GVaryings& in, const GUniforms&) {
    // low alpha so additive blending needs several overlapping stars to saturate
    valo::valo_FragColor = valo::Color(in.color * in.glow, 0.35f);
}

VALO_FRAGMENT_SHADER(galaxyFS)
