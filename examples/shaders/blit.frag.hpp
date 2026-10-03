#pragma once

#include "blit.vert.hpp"

inline void blitFS(const BlitVaryings& in, const BlitUniforms& u) {
    using namespace valo;

    valo_FragColor = texture(u.tex, in.uv,
        Sampler(Wrap::Clamp, Wrap::Clamp, Filter::Linear));
}

VALO_FRAGMENT_SHADER(blitFS)
