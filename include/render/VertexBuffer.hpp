#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "common/Math.hpp"

namespace valo {

enum class VtxAttrib {
    Float, Int32, Uint32,
    Vec2f, Vec2i, Vec2u,
    Vec3f, Vec3i, Vec3u,
    Vec4f, Vec4i, Vec4u,
};


inline constexpr size_t attribSize(const VtxAttrib attrib) {
    switch (attrib) {
        case VtxAttrib::Float:  return sizeof(float);
        case VtxAttrib::Int32:  return sizeof(int32_t);
        case VtxAttrib::Uint32: return sizeof(uint32_t);
        case VtxAttrib::Vec2f:  return sizeof(Vec2f);
        case VtxAttrib::Vec2i:  return sizeof(Vec2i);
        case VtxAttrib::Vec2u:  return sizeof(Vec2u);
        case VtxAttrib::Vec3f:  return sizeof(Vec3f);
        case VtxAttrib::Vec3i:  return sizeof(Vec3i);
        case VtxAttrib::Vec3u:  return sizeof(Vec3u);
        case VtxAttrib::Vec4f:  return sizeof(Vec4f);
        case VtxAttrib::Vec4i:  return sizeof(Vec4i);
        case VtxAttrib::Vec4u:  return sizeof(Vec4u);
        default: return 0;
    }
}



class VtxLayout {
public:
    VtxLayout() = default;
    VtxLayout(const std::vector<VtxAttrib>& attribs)
    : m_attribs(attribs) { 
        for (auto& attrib : attribs)
            m_stride += attribSize(attrib);
    }

    const std::vector<VtxAttrib>& getAttribs() const { return m_attribs; }

    size_t stride() const { return m_stride; }

private:
    std::vector<VtxAttrib> m_attribs;
    size_t m_stride = 0;
};



struct VertexBuffer {
    std::vector<uint8_t> verts;
    VtxLayout layout;
};

} // namespace valo