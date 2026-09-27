#pragma once

#include <vector>
#include <memory>
#include "RenderTarget.hpp"
#include "RenderCommand.hpp"
#include "common/Math.hpp"
#include "common/Srgb.hpp"

namespace valo {

using TextureID = uint32_t;

inline constexpr uint32_t MAX_VARYINGS = 32;

namespace detail {
    inline thread_local const std::vector<std::unique_ptr<RenderTarget>>* boundTargets = nullptr;
    inline thread_local const void* const* boundUniforms = nullptr;

    struct VaryingDerivs {
        const float* interp = nullptr;
        const float* vary[3] = {};
        float w[3] = {};
        float dBdx[3] = {}, dBdy[3] = {};
        float dDenDx = 0.f, dDenDy = 0.f;
        float invW = 0.f;
        bool valid = false;
    };

    inline thread_local VaryingDerivs varyingDerivs;
}

inline thread_local float       valo_PointSize      = 1.f;
inline thread_local uint32_t    valo_InstanceIndex  = 0;
inline thread_local bool        valo_Discard        = false;
inline thread_local uint32_t    valo_VertexIndex    = 0;
inline thread_local Vec4f       valo_FragCoord      = {0.f, 0.f, 0.f, 0.f};
inline thread_local bool        valo_FrontFacing    = false;
inline thread_local Vec4f       valo_Position;          // written by the vertex shader (gl_Position)
inline thread_local Color       valo_FragColor;         // written by the fragment shader (gl_FragColor)
inline thread_local float       valo_FragDepth      = 1.f;
inline thread_local Vec2f       valo_PointCoord     = {0.f, 0.f};

inline void vDiscard()          { valo_Discard = true; }

enum class Wrap     { Repeat, Clamp, Mirror };
enum class Filter   { Nearest, Linear };

struct Sampler {
    Sampler(
        const Wrap wrapU = Wrap::Repeat, 
        const Wrap wrapV = Wrap::Repeat, 
        const Filter filter = Filter::Linear
    ) : wrapU(wrapU), wrapV(wrapV), filter(filter) {}

    Wrap    wrapU   = Wrap::Repeat;
    Wrap    wrapV   = Wrap::Repeat;
    Filter  filter  = Filter::Linear;
};


Color texture(const TextureID tex, const Vec2f& uv, const Sampler& sampler = {});
float textureDepth(const TextureID tex, const Vec2f& uv);

template <typename T>
const T& uniformBuffer(const uint32_t slot) {
    assert(slot < MAX_UNIFORM_SLOTS && detail::boundUniforms[slot] && "uniformBuffer(): nothing bound to slot");
    return *static_cast<const T*>(detail::boundUniforms[slot]);
}

#define discard valo::vDiscard()

}