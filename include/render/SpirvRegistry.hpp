#pragma once

#include "ShaderPath.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace valo {

struct SpirvEntry {
    const uint32_t* words = nullptr;
    size_t          count = 0;
};

inline std::vector<std::pair<std::string, SpirvEntry>>& spirvRegistry() {
    static std::vector<std::pair<std::string, SpirvEntry>> registry;
    return registry;
}

inline bool registerSpirv(const char* path, const uint32_t* words, const size_t count) {
    spirvRegistry().push_back({path, {words, count}});
    return true;
}

inline const SpirvEntry* findSpirv(const std::string& path) {
    for (const auto& [file, entry] : spirvRegistry())
        if (shaderPathMatches(file, path)) return &entry;
    return nullptr;
}

#define VALO_EMBED_SPIRV(words, path) \
    [[maybe_unused]] static const bool valo_spirv_##words = \
        ::valo::registerSpirv(path, words, sizeof(words) / sizeof(words[0]));

} // namespace valo

/*
    For a static library that embeds shaders with valo_embed_shaders(): its
    objects are only linked when something references them, so the library
    calls its generated index once before creating programs. Declare at global
    scope, call from anywhere:

        VALO_DECLARE_EMBEDDED_SHADERS(mylib)
        ...
        VALO_REGISTER_EMBEDDED_SHADERS(mylib);

    `target` is the CMake target name, with anything that is not valid in an
    identifier replaced by '_'. An executable needs neither - its shaders
    register themselves - and calling it anyway is harmless.
*/
#define VALO_DECLARE_EMBEDDED_SHADERS(target) \
    void valo_register_embedded_shaders_##target();

#define VALO_REGISTER_EMBEDDED_SHADERS(target) \
    ::valo_register_embedded_shaders_##target()
