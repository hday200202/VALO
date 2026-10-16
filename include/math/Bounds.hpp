#pragma once

#include "Mat.hpp"

#include <algorithm>

namespace valo {

struct Bounds {
    Vec3f min = {  1e30f,  1e30f,  1e30f };
    Vec3f max = { -1e30f, -1e30f, -1e30f };

    bool valid() const { return min.x <= max.x; }
    Vec3f center() const { return (min + max) * 0.5f; }
    Vec3f size() const { return max - min; }
    float radius() const { return size().length() * 0.5f; }

    void grow(const Vec3f& p) {
        min = { std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z) };
        max = { std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z) };
    }

    Bounds transformed(const Mat4f& m) const {
        if (!valid()) return *this;

        Bounds out;
        out.min = out.max = m.cols[3].xyz;

        const float* lo = min.data();
        const float* hi = max.data();
        float* outLo = out.min.data();
        float* outHi = out.max.data();

        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                const float a = m.cols[c][r] * lo[c];
                const float b = m.cols[c][r] * hi[c];
                outLo[r] += std::min(a, b);
                outHi[r] += std::max(a, b);
            }
        }
        return out;
    }
};

} // namespace valo