#pragma once

#include "minimal.vert.hpp"

namespace me {

inline void minimalFS(const Varyings& in, const Uniforms& u) {
    Color color = (u.material.baseColorTex == EMPTY_TEXTURE)
        ? u.material.baseColor
        : texture(u.material.baseColorTex, in.uv,
                  Sampler(Wrap::Repeat, Wrap::Repeat, Filter::Nearest));

    if (color.a < 0.3f) discard;
    // color.a = 1.f;
    valo_FragColor = color;
}

VALO_FRAGMENT_SHADER(minimalFS)

} // namespace me
