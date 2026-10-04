#pragma once

#include "bloom.vert.hpp"

/*
    Bright pass with a narrow knee.

    Rescaling by how far a pixel sits above the threshold (over / (1 - t)) is
    the obvious thing and it does not work here: this scene is tone mapped, so
    its luminance is compressed into roughly 0.30-0.40 and nothing sits far
    enough above any useful threshold to survive that division. A fixed, narrow
    knee instead lets a pixel reach full strength shortly after it passes.
*/
inline void bloomThreshFS(const BloomVaryings& in, const BloomUniforms& u) {
    using namespace valo;

    const Color c = texture(u.srcTex, in.uv, Sampler(Wrap::Clamp, Wrap::Clamp, Filter::Linear));

    const float lum  = c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f;
    const float knee = 0.08f;

    float k = (lum - u.threshold) / knee;
    if (k < 0.f) k = 0.f;
    if (k > 1.f) k = 1.f;

    valo_FragColor = Color{c.r * k, c.g * k, c.b * k, 1.f};
}

VALO_FRAGMENT_SHADER(bloomThreshFS)
