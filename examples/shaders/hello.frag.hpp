#pragma once

#include "hello.vert.hpp"

inline void helloFS(const HelloVaryings& in, const HelloUniforms&) {
    valo_FragColor = Color(in.colour.x, in.colour.y, in.colour.z, 1.f);
}

VALO_FRAGMENT_SHADER(helloFS)
