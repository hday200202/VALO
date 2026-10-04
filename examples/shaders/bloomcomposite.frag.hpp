#pragma once

#include "bloom.vert.hpp"

// Additive: the scene as rendered, plus the blurred bright pass.
inline void bloomCompositeFS(const BloomVaryings& in, const BloomUniforms& u) {
    using namespace valo;

    const Sampler s(Wrap::Clamp, Wrap::Clamp, Filter::Linear);

    const Color scene = texture(u.srcTex,   in.uv, s);
    const Color glow  = texture(u.bloomTex, in.uv, s);

    valo_FragColor = Color{
        scene.r + glow.r * u.intensity,
        scene.g + glow.g * u.intensity,
        scene.b + glow.b * u.intensity,
        1.f
    };
}

VALO_FRAGMENT_SHADER(bloomCompositeFS)
