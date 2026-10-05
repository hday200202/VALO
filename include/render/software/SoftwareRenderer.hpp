#pragma once

/*
    The software backend's state, split out of Renderer so both backends own
    their resources the same way: Renderer holds a SoftwareRenderer* and a
    VulkanRenderer*, and keeps only what is backend-agnostic - the ID space
    (free lists), the deferred-destroy queues, the frame counter and the
    dispatch table.

    Members are public on purpose. This is not public API; it is the storage
    half of Renderer, reached as m_software->m_targets by the software* methods
    that used to reach m_targets directly. Making it a class with accessors
    would add a layer for nothing.

    When the backends eventually become a real class hierarchy, this is the
    half that turns into SoftwareRenderer : public Renderer.
*/

#include <vector>
#include <memory>

#include "RenderTarget.hpp"
#include "../buffers/VertexBuffer.hpp"
#include "../buffers/IndexBuffer.hpp"
#include "../buffers/UniformBuffer.hpp"
#include "../buffers/StorageBuffer.hpp"
#include "../ShaderProgram.hpp"
#include "../ShaderRegistry.hpp"
#include "../RenderCommand.hpp"
#include "DrawFuncs.hpp"
#include "ThreadPool.hpp"

struct SDL_Texture;
struct SDL_Renderer;

namespace valo {

class Renderer;

class SoftwareRenderer {
public:
    ~SoftwareRenderer() = default;

    SoftwareRenderer(const SoftwareRenderer&) = delete;
    SoftwareRenderer& operator=(const SoftwareRenderer&) = delete;

    explicit SoftwareRenderer(Renderer& owner) : m_owner(&owner) {}

    bool            init();
    void            shutdown();

    void            flush();
    void            clear(const Color& clearColor, const RenderTargetID target, const float clearDepth);
    void            draw(const RenderCommand& cmd, const RenderTargetID target);
    void            show(const RenderTargetID target);

    RenderTargetID  createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb);

    uint32_t        slotCount(const ResourceKind kind) const;

    VertexBufferID  createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    void            updateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout);

    IndexBufferID   createIndexBuffer(const uint32_t* data, const size_t count);
    void            updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count);

    UniformBufferID createUniformBuffer(const void* data, const size_t bytes);
    void            updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes);

    StorageBufferID createStorageBuffer(const void* data, const size_t bytes);
    void            updateStorageBuffer(const StorageBufferID buffer, const void* data, const size_t bytes);

    TextureID       createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips);
    TextureID       createTextureFromFile(const std::string& path, const bool srgb, const bool mips);
    void            updateTexture(const TextureID texture, const uint32_t* data);
    void            updateTextureRect(const TextureID texture, const uint32_t x, const uint32_t y, const uint32_t w, const uint32_t h, const uint32_t* data);

    ProgramID       createProgramFromCallbacks(const Program& program);
    ProgramID       createProgramFromFiles(const std::string& vsPath, const std::string& fsPath);

    void            resize(const uint32_t w, const uint32_t h);
    void            setVsync(const bool vsync);

    void            releaseTarget(const RenderTargetID id);
    void            releaseVertexBuffer(const VertexBufferID id);
    void            releaseIndexBuffer(const IndexBufferID id);
    void            releaseUniformBuffer(const UniformBufferID id);
    void            releaseStorageBuffer(const StorageBufferID id);
    void            releaseProgram(const ProgramID id);

    uint32_t        width() const;
    uint32_t        height() const;

    // the SDL streaming texture that show() blits the backbuffer through
    bool recreateTexture(const uint32_t w, const uint32_t h);

    /* ---- resources, indexed by the IDs Renderer hands out ---------------- */
    std::vector<std::unique_ptr<RenderTarget>>  m_targets;
    std::vector<VertexBuffer>                   m_vertexBuffers;
    std::vector<IndexBuffer>                    m_indexBuffers;
    std::vector<Program>                        m_programs;
    std::vector<UniformBuffer>                  m_uniformBuffers;
    std::vector<StorageBuffer>                  m_storageBuffers;

    /* ---- presentation --------------------------------------------------- */
    SDL_Renderer*   m_sdlRenderer   = nullptr;
    SDL_Texture*    m_sdlTexture    = nullptr;
    uint32_t        m_texW          = 0;
    uint32_t        m_texH          = 0;

    /* ---- rasterisation -------------------------------------------------- */
    Renderer*   m_owner = nullptr;

    ThreadPool  m_pool;

    // triangles accumulate here across draws; see TriBatch in DrawFuncs.hpp
    TriBatch    m_batch;

    // resolved once per draw, handed down through DrawContext
    const void* m_resolvedUniforms[MAX_UNIFORM_SLOTS] = {};
    const void* m_resolvedStorage[MAX_STORAGE_SLOTS] = {};
    uint32_t m_resolvedStorageBytes[MAX_STORAGE_SLOTS] = {};
};

} // namespace valo
