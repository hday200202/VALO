#pragma once

#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <type_traits>
#include <array>

namespace valo {

inline constexpr uint32_t EMPTY_BUFFER      = UINT32_MAX;
inline constexpr uint32_t DEFAULT_PROGRAM   = UINT32_MAX;
inline constexpr uint32_t ALL_INDICES       = UINT32_MAX;
inline constexpr uint32_t MAX_INSTANCES     = UINT32_MAX; // Essentially unlimited for now
inline constexpr uint32_t EMPTY_TEXTURE     = UINT32_MAX;
inline constexpr uint32_t MAX_UNIFORM_SLOTS = 8;

using RenderTargetID    = uint32_t;
using VertexBufferID    = uint32_t;
using IndexBufferID     = uint32_t;
using ProgramID         = uint32_t;
using TextureID         = uint32_t;
using UniformBufferID   = uint32_t;

enum class Topology     { Points, Lines, Triangles };
enum class CullMode     { None, Back, Front };
enum class VtxWinding   { CCW, CW };
enum class BlendMode    { Opaque, Alpha, Additive };
enum class FillMode     { Solid, Wireframe };
enum class StencilTest  { Always, Equal, NotEqual, Less, Greater };
enum class StencilOp    { Keep, Zero, Replace, Incr, Decr };

struct RenderCommand {
    VertexBufferID      verts           = EMPTY_BUFFER;
    IndexBufferID       indices         = EMPTY_BUFFER;
    ProgramID           program         = DEFAULT_PROGRAM;

    // UniformBufferID     uniforms        = EMPTY_BUFFER;

    uint32_t            firstIndex      = 0;
    uint32_t            indexCount      = ALL_INDICES;

    Topology            topology        = Topology::Triangles;
    CullMode            cullmode        = CullMode::Back;
    VtxWinding          winding         = VtxWinding::CCW;
    BlendMode           blendmode       = BlendMode::Opaque;
    StencilTest         stencilTest     = StencilTest::Always;
    StencilOp           stencilOp       = StencilOp::Keep;
    uint8_t             stencilRef      = 0;

    bool                depthTest       = true;
    bool                depthWrite      = true;
    bool                colorWrite      = true;

    // Set when the fragment shader writes valo_FragDepth. The depth test then
    // has to run after shading instead of before it, since the interpolated z
    // the early test would use is not the value that gets written.
    bool                fragDepthWrite  = false;
    FillMode            fillmode        = FillMode::Solid;

    uint32_t            instanceCount   = 1;

    static constexpr size_t MAX_UNIFORM_BYTES = 512;
    uint8_t uniforms[MAX_UNIFORM_BYTES];
    size_t uniformSize = 0;

    template <typename T> void setUniforms(const T& t) {
        static_assert(std::is_trivially_copyable_v<T>, "...");
        static_assert(sizeof(T) <= MAX_UNIFORM_BYTES, "...");

        std::memcpy(uniforms, &t, sizeof(T));
        uniformSize = sizeof(T);
    }

    inline constexpr std::array<UniformBufferID, MAX_UNIFORM_SLOTS> emptyUniformSlots() {
        std::array<UniformBufferID, MAX_UNIFORM_SLOTS> slots{};
        for (auto& s : slots) s = EMPTY_BUFFER;
        return slots;
    }

    std::array<UniformBufferID, MAX_UNIFORM_SLOTS> uniformBuffers = emptyUniformSlots();

    void bindUniformBuffer(const uint32_t slot, const UniformBufferID buffer) {
        assert(slot < MAX_UNIFORM_SLOTS && "bindUniformBuffer(): slot out of range");
        uniformBuffers[slot] = buffer;
    }
};

} // namespace valo