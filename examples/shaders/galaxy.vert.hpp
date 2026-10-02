#pragma once

#include <render/ShaderRegistry.hpp>

#include <cmath>
#include <algorithm>

// Each star is static data: orbit radius, base angle, height, and a seed.
struct Star      { float radius; float angle; float height; float seed; };
struct GUniforms { valo::Mat4f viewProj; valo::Mat4f model; float time; };
struct GVaryings { valo::Vec3f color; float glow; };

static constexpr float kGalaxyRadius = 3.5f;

inline void galaxyVS(const Star& in, const GUniforms& u, GVaryings& out) {
    using namespace valo;

    const float r = in.radius;
    // inner stars orbit faster (roughly Keplerian) -> arms wind up over time
    const float a = in.angle + u.time * 0.9f / std::sqrt(r + 0.25f);
    // thin disk, puffier toward the core
    const float y = in.height * (0.35f - 0.25f * (r / kGalaxyRadius));

    Vec3f pos  = {r * std::cos(a), y, r * std::sin(a)};
    Vec4f clip = u.viewProj * u.model * Vec4f(pos, 1.f);

    // color by radius: white-yellow core -> orange -> blue-violet rim
    const float f = r / kGalaxyRadius;
    Vec3f core = {1.0f, 0.95f, 0.8f};
    Vec3f mid  = {1.0f, 0.55f, 0.25f};
    Vec3f rim  = {0.35f, 0.45f, 1.0f};
    out.color  = f < 0.35f ? lerp(core, mid, f / 0.35f) : lerp(mid, rim, (f - 0.35f) / 0.65f);

    // twinkle: each star has its own phase
    out.glow = 0.55f + 0.45f * std::sin(u.time * 2.5f + in.seed * 6.2831853f);

    // point size shrinks with distance (clip.w == view depth), bigger near the core
    valo_PointSize = std::clamp((5.f + 4.f * (1.f - f)) / clip.w * 2.5f, 1.f, 14.f);
    valo_Position = clip;
}

VALO_VERTEX_SHADER(galaxyVS)
