#pragma once

#include "Sampling.hpp"
#include "common/Math.hpp"

namespace valo {

inline constexpr uint32_t MAX_VARYINGS = 32;


using RawFn = void (*)();
using VsThunk = Vec4f (*)(RawFn fn, const void* vertex, const void* uniforms, float* varyingsOut);
using FsThunk = Color (*)(RawFn fn, const float* varyings, const void* uniforms);


struct Program {
    VsThunk vsThunk; RawFn vsFn;
    FsThunk fsThunk; RawFn fsFn;
    uint32_t varyingCount;
};


template <typename V, typename U, typename Vy>
Vec4f vsThunk(RawFn fn, const void* vertex, const void* uniforms, float* varyingsOut) {
    using TypedFn = Vec4f (*)(const V&, const U&, Vy&);
    auto typed = reinterpret_cast<TypedFn>(fn);
    auto* v = static_cast<const V*>(vertex);
    auto* u = static_cast<const U*>(uniforms);
    auto* vy = reinterpret_cast<Vy*>(varyingsOut);
    return typed(*v, *u, *vy);
}


template <typename Vy, typename U> 
Color fsThunk(RawFn fn, const float* varyings, const void* uniforms) {
    using TypedFn = Color (*)(const Vy&, const U&);
    auto typed = reinterpret_cast<TypedFn>(fn);
    auto* vy = reinterpret_cast<const Vy*>(varyings);
    auto* u = static_cast<const U*>(uniforms);
    return typed(*vy, *u);
}


template <typename V, typename U, typename Vy>
Program makeProgram(Vec4f (*vs)(const V&, const U&, Vy&), Color (*fs)(const Vy&, const U&)) {
    static_assert(std::is_trivially_copyable_v<Vy> && sizeof(Vy) % sizeof(float) == 0, "...");
    static_assert(sizeof(Vy) / sizeof(float) <= MAX_VARYINGS, "...");

    Program p;
    p.vsThunk = &vsThunk<V, U, Vy>; p.vsFn = reinterpret_cast<RawFn>(vs);
    p.fsThunk = &fsThunk<Vy, U>;    p.fsFn = reinterpret_cast<RawFn>(fs);
    p.varyingCount = sizeof(Vy) / sizeof(float);
    return p;
}

};