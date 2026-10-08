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
inline constexpr uint32_t MAX_STORAGE_SLOTS = 4;

using RenderTargetID    = uint32_t;
using VertexBufferID    = uint32_t;
using IndexBufferID     = uint32_t;
using ProgramID         = uint32_t;
using TextureID         = uint32_t;
using UniformBufferID   = uint32_t;
using StorageBufferID   = uint32_t;

enum class ResourceKind {
    Target,
    VertexBuffer,
    IndexBuffer,
    Program,
    UniformBuffer,
    StorageBuffer
};

enum class Topology     { Points, Lines, Triangles };
enum class CullMode     { None, Back, Front };
enum class VtxWinding   { CCW, CW };
enum class BlendMode    { Opaque, Alpha, Additive };
enum class FillMode     { Solid, Wireframe };
enum class StencilTest  { Always, Equal, NotEqual, Less, Greater };
enum class StencilOp    { Keep, Zero, Replace, Incr, Decr };
enum class DepthCompare { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, Always, Never };

struct Viewport {
    float x         = 0.f,  y           = 0.f;
    float width     = 0.f,  height      = 0.f;
    float minDepth  = 0.f,  maxDepth    = 1.f;
};

struct Scissor {
    int32_t     x       = 0,    y       = 0;
    uint32_t    width   = 0,    height  = 0;
};

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
    DepthCompare        depthCompare    = DepthCompare::Greater;   // reversed depth: nearer wins
    bool                depthWrite      = true;
    bool                colorWrite      = true;
    bool                fragDepthWrite  = false;
    
    FillMode            fillmode        = FillMode::Solid;

    Viewport            viewport        = {};
    Scissor             scissor         = {};

    uint32_t            instanceCount   = 1;

    /*
        Three ways to get data into a shader, by how often it changes:

        setUniforms(T)          Per draw. The struct is copied into this command
                                at submit, so every draw keeps its own values;
                                it is the shader's Uniforms parameter. Small -
                                512 bytes, and on Vulkan it becomes push
                                constants, which only guarantee 128.

        bindUniformBuffer()     Per frame, shared by many draws: camera, lights,
                                time. The shader reads uniformBuffer<T>(slot).
                                It is read when the frame runs at endFrame, not
                                at submit - updating it between two submits
                                changes both draws.

        bindStorageBuffer()     Bulk and read-only: arrays of any length. The
                                shader reads storageBuffer<T>(slot)[i] and
                                .size(). Same timing as a uniform buffer.

        Slots are compile-time constants in the shader, and flags in any of
        these structs are uint32_t, never bool.
    */
    static constexpr size_t MAX_UNIFORM_BYTES = 512;
    uint8_t uniforms[MAX_UNIFORM_BYTES];
    size_t uniformSize = 0;

    std::array<UniformBufferID, MAX_UNIFORM_SLOTS> uniformBuffers = emptyUniformSlots();
    std::array<StorageBufferID, MAX_STORAGE_SLOTS> storageBuffers = emptyStorageSlots();

    template <typename T> void setUniforms(const T& t) {
        static_assert(std::is_trivially_copyable_v<T>,
            "setUniforms() copies the struct into the command, so it must be trivially copyable - "
            "no std::vector, std::string or anything else that owns memory");
        static_assert(sizeof(T) <= MAX_UNIFORM_BYTES,
            "Uniforms are limited to RenderCommand::MAX_UNIFORM_BYTES (512). Move frame-wide data "
            "into a uniform buffer, or bulk data into a storage buffer");

        std::memcpy(uniforms, &t, sizeof(T));
        uniformSize = sizeof(T);
    }

    inline constexpr std::array<UniformBufferID, MAX_UNIFORM_SLOTS> emptyUniformSlots() {
        std::array<UniformBufferID, MAX_UNIFORM_SLOTS> slots{};
        for (auto& s : slots) s = EMPTY_BUFFER;
        return slots;
    }

    inline constexpr std::array<StorageBufferID, MAX_STORAGE_SLOTS> emptyStorageSlots() {
        std::array<StorageBufferID, MAX_STORAGE_SLOTS> slots{};
        for (auto& s : slots) s = EMPTY_BUFFER;
        return slots;
    }

    void bindUniformBuffer(const uint32_t slot, const UniformBufferID buffer) {
        assert(slot < MAX_UNIFORM_SLOTS && "bindUniformBuffer(): slot out of range");
        uniformBuffers[slot] = buffer;
    }

    void bindStorageBuffer(const uint32_t slot, const StorageBufferID buffer) {
        assert(slot < MAX_STORAGE_SLOTS && "bindStorageBuffer(): slot out of range");
        storageBuffers[slot] = buffer;
    }
};

} // namespace valo