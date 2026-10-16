#pragma once

#include <string>
#include <vector>

namespace valo {

inline std::string normalizeShaderPath(std::string s) {
    for (auto& c : s) if (c == '\\') c = '/';

    std::vector<std::string> parts;
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();

        const std::string seg = s.substr(i, j - i);
        if (seg == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else if (seg != "." && !seg.empty()) parts.push_back(seg);

        i = j + 1;
    }

    std::string out;
    for (const auto& seg : parts) {
        if (!out.empty()) out += '/';
        out += seg;
    }
    return out;
}

// a registered path matches any request it ends with at a '/', so "shaders/x.vert.hpp"
// finds an entry registered under its full source path but "sky.vert.hpp" never finds gltfsky's
inline bool shaderPathMatches(const std::string& registered, const std::string& requested) {
    const std::string have = normalizeShaderPath(registered);
    const std::string want = normalizeShaderPath(requested);
    if (want.empty() || have.size() < want.size()) return false;
    if (have.compare(have.size() - want.size(), want.size(), want) != 0) return false;
    return have.size() == want.size() || have[have.size() - want.size() - 1] == '/';
}

} // namespace valo
