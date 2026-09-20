#pragma once

#include <cstdint>
#include <cstdlib>

namespace valo {

#define EMPTY_BUFFER    UINT32_MAX
#define DEFAULT_PROGRAM UINT32_MAX
#define ALL_INDICES     UINT32_MAX

using RenderTargetID    = uint32_t;
using VertexBufferID    = uint32_t;
using IndexBufferID     = uint32_t;
using ProgramID         = uint32_t;
using TextureID         = uint32_t;

enum class Topology     { Points, Lines, Triangles };
enum class CullMode     { None, Back, Front };
enum class VtxWinding   { CCW, CW };
enum class BlendMode    { Opaque, Alpha, Additive };
enum class FillMode     { Solid, Wireframe };

struct RenderCommand {
    VertexBufferID      verts           = EMPTY_BUFFER;
    IndexBufferID       indices         = EMPTY_BUFFER;
    ProgramID           program         = DEFAULT_PROGRAM;

    uint32_t            firstIndex      = 0;
    uint32_t            indexCount      = ALL_INDICES;

    Topology            topology        = Topology::Triangles;
    CullMode            cullmode        = CullMode::Back;
    VtxWinding          winding         = VtxWinding::CCW;
    BlendMode           blendmode       = BlendMode::Opaque;
    bool                depthTest       = true;
    bool                depthWrite      = true;
    FillMode            fillmode        = FillMode::Solid;

    static constexpr size_t MAX_UNIFORM_BYTES = 256;
    uint8_t uniforms[MAX_UNIFORM_BYTES];
    size_t uniformSize = 0;

    template <typename T> void setUniforms(const T& t) {
        static_assert(std::is_trivially_copyable_v<T>, "...");
        static_assert(sizeof(T) <= MAX_UNIFORM_BYTES, "...");
        
        std::memcpy(uniforms, &t, sizeof(T));
        uniformSize = sizeof(T);
    }
};

} // namespace valo