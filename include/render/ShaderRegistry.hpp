#pragma once

#include "ShaderProgram.hpp"
#include "ShaderPath.hpp"

#include <string>
#include <vector>

/*
    Writing a shader both backends run

    Two headers, name.vert.hpp and name.frag.hpp, the fragment one including
    the vertex one so they share structs:

        struct MyVertex   { Vec3f pos; Vec2f uv; };         // the vertex buffer
        struct MyUniforms { Mat4f mvp; TextureID tex; };    // setUniforms()
        struct MyVaryings { Vec2f uv; };                    // vertex -> fragment

        inline void myVS(const MyVertex& in, const MyUniforms& u, MyVaryings& out);
        VALO_VERTEX_SHADER(myVS)

        inline void myFS(const MyVaryings& in, const MyUniforms& u);
        VALO_FRAGMENT_SHADER(myFS)

    createProgram("dir/name.vert.hpp", "dir/name.frag.hpp") runs the C++ on the
    software backend and a valoc translation of it on Vulkan, retranslated
    whenever the shader or anything it includes changes. valoc refuses, naming
    the line, whatever it would translate wrongly - a bool in a GPU-read
    struct, a non-float varying, a buffer slot that is not a constant. Where
    C++ and GLSL genuinely differ, VALO_GLSL(fn, "...") supplies one function's
    GLSL by hand.
*/

namespace valo {

/*
    One address per type, the same in every translation unit - an inline
    function's static is merged by the linker. Lets createProgram(path, path)
    check that the two halves it found by path agree on their structs, which
    the function-pointer overload gets from the template for free.
*/
template <typename T>
const void* typeTag() {
    static const char tag = 0;
    return &tag;
}

struct ShaderEntry {
    VsThunk     vsThunk         = nullptr;
    FsThunk     fsThunk         = nullptr;
    RawFn       fn              = nullptr;
    uint32_t    varyingCount    = 0;

    const void* uniformsTag     = nullptr;
    const void* varyingsTag     = nullptr;
    size_t      uniformsSize    = 0;
    size_t      varyingsSize    = 0;
};

inline std::vector<std::pair<std::string, ShaderEntry>>& shaderRegistry() {
    static std::vector<std::pair<std::string, ShaderEntry>> registry;
    return registry;
}

template <typename V, typename U, typename Vy>
ShaderEntry makeVertexEntry(void (*vs)(const V&, const U&, Vy&)) {
    static_assert(std::is_trivially_copyable_v<Vy>,
        "Varyings must be trivially copyable - plain fields only, no std::vector or other owning types");
    static_assert(sizeof(Vy) % sizeof(float) == 0,
        "Varyings must be made only of floats (float, Vec2f, Vec3f, Vec4f, Mat3f, Mat4f) - "
        "every field is interpolated as floats between the vertices");
    static_assert(sizeof(Vy) / sizeof(float) <= MAX_VARYINGS,
        "Varyings are limited to MAX_VARYINGS (32) floats - a Mat4f alone is 16");

    return {
        .vsThunk        = &vsThunk<V, U, Vy>,
        .fn             = reinterpret_cast<RawFn>(vs),
        .varyingCount   = sizeof(Vy) / sizeof(float),
        .uniformsTag    = typeTag<U>(),
        .varyingsTag    = typeTag<Vy>(),
        .uniformsSize   = sizeof(U),
        .varyingsSize   = sizeof(Vy)
    };
}

template <typename Vy, typename U>
ShaderEntry makeFragmentEntry(void (*fs)(const Vy&, const U&)) {
    return {
        .fsThunk        = &fsThunk<Vy, U>,
        .fn             = reinterpret_cast<RawFn>(fs),
        .uniformsTag    = typeTag<U>(),
        .varyingsTag    = typeTag<Vy>(),
        .uniformsSize   = sizeof(U),
        .varyingsSize   = sizeof(Vy)
    };
}

inline bool registerShader(const char* file, const ShaderEntry& entry) {
    shaderRegistry().push_back({file, entry});
    return true;
}

#define VALO_GLSL(fn, src) \
    inline const char* valo_glsl_##fn = src;

#define VALO_VERTEX_SHADER(fn) \
    inline const bool valo_reg_##fn = ::valo::registerShader(__FILE__, ::valo::makeVertexEntry(&fn));

#define VALO_FRAGMENT_SHADER(fn) \
    inline const bool valo_reg_##fn = ::valo::registerShader(__FILE__, ::valo::makeFragmentEntry(&fn));

inline const ShaderEntry* findShader(const std::string& path) {
    for (const auto& [file, entry] : shaderRegistry())
        if (shaderPathMatches(file, path)) return &entry;
    return nullptr;
}

} // namespace valo