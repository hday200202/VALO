#pragma once

#include "helmet.vert.hpp"

inline void outlineFS(const Varyings&, const Uniforms&) {
    valo::valo_FragColor = {0.f, 0.f, 0.f, 1.f};
}

VALO_FRAGMENT_SHADER(outlineFS)
