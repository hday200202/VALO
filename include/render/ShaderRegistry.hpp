#pragma once

#include "ShaderProgram.hpp"

#include <string>
#include <vector>

namespace valo {

struct ShaderEntry {
    VsThunk     vsThunk         = nullptr;
    FsThunk     fsThunk         = nullptr;
    RawFn       fn              = nullptr;
    uint32_t    varyingCount    = 0;
};

inline std::vector<std::pair<std::string, ShaderEntry>>& shaderRegistry() {
    static std::vector<std::pair<std::string, ShaderEntry>> registry;
    return registry;
}

template <typename V, typename U, typename Vy>
ShaderEntry makeVertexEntry(void (*vs)(const V&, const U&, Vy&)) {
    static_assert(std::is_trivially_copyable_v<Vy> && sizeof(Vy) % sizeof(float) == 0, "...");
    static_assert(sizeof(Vy) / sizeof(float) <= MAX_VARYINGS, "...");

    return {
        .vsThunk        = &vsThunk<V, U, Vy>,
        .fn             = reinterpret_cast<RawFn>(vs),
        .varyingCount   = sizeof(Vy) / sizeof(float)
    };
}

template <typename Vy, typename U>
ShaderEntry makeFragmentEntry(void (*fs)(const Vy&, const U&)) {
    return {
        .fsThunk    = &fsThunk<Vy, U>,
        .fn         = reinterpret_cast<RawFn>(fs)
    };
}

inline bool registerShader(const char* file, const ShaderEntry& entry) {
    shaderRegistry().push_back({file, entry});
    return true;
}

/*
    An override for valoc: `fn` keeps its C++ body for the software backend, and
    the translator emits `src` verbatim instead of trying to read it. For the
    cases where the two genuinely differ - a CPU lookup table standing in for
    something the GPU does in one instruction, or anything with no C++ spelling.

    The declaration is what valoc looks for; nothing reads it at runtime.
*/
#define VALO_GLSL(fn, src) \
    inline const char* valo_glsl_##fn = src;

#define VALO_VERTEX_SHADER(fn) \
    inline const bool valo_reg_##fn = ::valo::registerShader(__FILE__, ::valo::makeVertexEntry(&fn));

#define VALO_FRAGMENT_SHADER(fn) \
    inline const bool valo_reg_##fn = ::valo::registerShader(__FILE__, ::valo::makeFragmentEntry(&fn));

inline const ShaderEntry* findShader(const std::string& path) {
    auto normalize = [](std::string s) {
        for (auto& c : s) if (c == '\\') c = '/';
        return s;
    };

    const std::string want = normalize(path);

    for (const auto& [file, entry] : shaderRegistry()) {
        const std::string have = normalize(file);
        if (have.size() >= want.size() && have.compare(have.size() - want.size(), want.size(), want) == 0)
            return &entry;
    }

    return nullptr;
}

} // namespace valo