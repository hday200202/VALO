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
