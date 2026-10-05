#pragma once

#include "../../include/VALO.hpp"
#include "../shaders/trace.vert.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sc {

using namespace valo;

/*
    Binned SAH: each split is chosen from 16 candidate planes per axis by the
    surface-area cost of the two halves, which keeps sibling boxes from
    overlapping the way a median split does on a dense mesh. Overlap is what
    makes a ray visit both children, so this is most of traversal cost.
*/
class BVH {
public:
    void build(std::vector<TraceTri>& tris) {
        m_nodes.clear();
        if (tris.empty()) return;

        m_nodes.reserve(tris.size() * 2);
        m_nodes.push_back({});
        subdivide(tris, 0, 0, uint32_t(tris.size()));
    }

    const std::vector<BVHNode>& nodes() const { return m_nodes; }

private:
    static constexpr int BINS = 16;

    struct Box {
        Vec3f lo = { 1e30f,  1e30f,  1e30f};
        Vec3f hi = {-1e30f, -1e30f, -1e30f};

        void grow(const Vec3f& p) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        void grow(const Box& b) { if (b.lo.x <= b.hi.x) { grow(b.lo); grow(b.hi); } }

        float area() const {
            const Vec3f e = hi - lo;
            return (e.x < 0.f) ? 0.f : e.x * e.y + e.y * e.z + e.z * e.x;
        }
    };

    static Vec3f centroid(const TraceTri& t) {
        return (t.p0 + t.p1 + t.p2) * (1.f / 3.f);
    }

    static float axisOf(const Vec3f& v, const int axis) {
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    }

    void makeLeaf(uint32_t node, uint32_t first, uint32_t count) {
        m_nodes[node].leftFirst = first;
        m_nodes[node].count     = count;
    }

    void subdivide(std::vector<TraceTri>& tris, uint32_t node, uint32_t first, uint32_t count) {
        Box box, cbox;
        for (uint32_t i = first; i < first + count; ++i) {
            const TraceTri& t = tris[i];
            box.grow(t.p0); box.grow(t.p1); box.grow(t.p2);
            cbox.grow(centroid(t));
        }

        m_nodes[node].lo = box.lo;
        m_nodes[node].hi = box.hi;

        if (count <= 2) { makeLeaf(node, first, count); return; }

        int   bestAxis = -1;
        int   bestBin  = 0;
        float bestCost = 1e30f;

        for (int axis = 0; axis < 3; ++axis) {
            const float lo = axisOf(cbox.lo, axis);
            const float hi = axisOf(cbox.hi, axis);
            if (hi - lo < 1e-9f) continue;

            Box      bins[BINS];
            uint32_t counts[BINS] = {};
            const float scale = BINS / (hi - lo);

            for (uint32_t i = first; i < first + count; ++i) {
                const TraceTri& t = tris[i];
                const int b = std::min(BINS - 1, int((axisOf(centroid(t), axis) - lo) * scale));
                bins[b].grow(t.p0); bins[b].grow(t.p1); bins[b].grow(t.p2);
                ++counts[b];
            }

            float    leftArea[BINS - 1];
            uint32_t leftCount[BINS - 1];
            Box acc; uint32_t n = 0;
            for (int b = 0; b < BINS - 1; ++b) {
                acc.grow(bins[b]); n += counts[b];
                leftArea[b] = acc.area(); leftCount[b] = n;
            }

            acc = Box{}; n = 0;
            for (int b = BINS - 1; b > 0; --b) {
                acc.grow(bins[b]); n += counts[b];
                const float cost = leftCount[b - 1] * leftArea[b - 1] + n * acc.area();
                if (leftCount[b - 1] > 0 && n > 0 && cost < bestCost) {
                    bestCost = cost; bestAxis = axis; bestBin = b;
                }
            }
        }

        // the 1.0 is the relative cost of a traversal step against a triangle
        // test; a split that doesn't beat testing everything here isn't taken
        const float leafCost = float(count) * box.area();
        if (bestAxis < 0 || (count <= 8 && bestCost + box.area() >= leafCost)) {
            makeLeaf(node, first, count);
            return;
        }

        const float lo    = axisOf(cbox.lo, bestAxis);
        const float scale = BINS / (axisOf(cbox.hi, bestAxis) - lo);

        auto it = std::partition(
            tris.begin() + first, tris.begin() + first + count,
            [&](const TraceTri& t) {
                return std::min(BINS - 1, int((axisOf(centroid(t), bestAxis) - lo) * scale)) < bestBin;
            });
        const uint32_t mid = uint32_t(it - tris.begin());

        if (mid == first || mid == first + count) { makeLeaf(node, first, count); return; }

        const uint32_t left = uint32_t(m_nodes.size());
        m_nodes.push_back({});
        m_nodes.push_back({});

        m_nodes[node].leftFirst = left;
        m_nodes[node].count     = 0;

        subdivide(tris, left,     first, mid - first);
        subdivide(tris, left + 1, mid,   first + count - mid);
    }

    std::vector<BVHNode> m_nodes;
};

} // namespace sc
