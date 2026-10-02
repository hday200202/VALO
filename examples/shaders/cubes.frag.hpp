#pragma once

#include "cubes.vert.hpp"

inline void cubesFS(const Varyings& in, const Uniforms& u) {
    valo::valo_FragColor = texture(
        u.tex, in.uv,
        valo::Sampler(
            valo::Wrap::Repeat,
            valo::Wrap::Repeat,
            valo::Filter::Linear
        )
    );
}

VALO_FRAGMENT_SHADER(cubesFS)
