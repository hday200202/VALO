#pragma once

#include "bloom.vert.hpp"

// One half of a separable Gaussian; texelStep carries both the direction and
// the texel size, so the same shader runs horizontally and vertically.
inline void bloomBlurFS(const BloomVaryings& in, const BloomUniforms& u) {
    using namespace valo;

    const Sampler s(Wrap::Clamp, Wrap::Clamp, Filter::Linear);
    const Vec2f d = u.texelStep;

    Vec3f sum = texture(u.srcTex, in.uv, s).xyz * 0.227027f;

    sum += texture(u.srcTex, in.uv + d, s).xyz * 0.1945946f;
    sum += texture(u.srcTex, in.uv - d, s).xyz * 0.1945946f;
    sum += texture(u.srcTex, in.uv + d * 2.f, s).xyz * 0.1216216f;
    sum += texture(u.srcTex, in.uv - d * 2.f, s).xyz * 0.1216216f;
    sum += texture(u.srcTex, in.uv + d * 3.f, s).xyz * 0.054054f;
    sum += texture(u.srcTex, in.uv - d * 3.f, s).xyz * 0.054054f;
    sum += texture(u.srcTex, in.uv + d * 4.f, s).xyz * 0.016216f;
    sum += texture(u.srcTex, in.uv - d * 4.f, s).xyz * 0.016216f;

    valo_FragColor = Color{sum.x, sum.y, sum.z, 1.f};
}

VALO_FRAGMENT_SHADER(bloomBlurFS)
