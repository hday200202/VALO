#pragma once

#include <vector>
#include <cassert>
#include <iostream>
#include <memory>
#include <cstring>
#include <deque>

#include "RenderCommand.hpp"
#include "RenderTarget.hpp"
#include "VertexBuffer.hpp"
#include "IndexBuffer.hpp"
#include "ShaderProgram.hpp"
#include "UniformBuffer.hpp"
#include "DrawFuncs.hpp"
#include "Sampling.hpp"
#include "ThreadPool.hpp"
#include "common/Math.hpp"

struct SDL_Texture;
struct SDL_Renderer;
struct SDL_Window;

namespace valo {

struct RenderConfig {
    SDL_Window*     sdlWindow       = nullptr;
    bool            vsync           = false;
    // backbuffer stores sRGB-encoded bytes; shader output is encoded on write
    bool            srgbBackbuffer  = false;

    uint16_t        maxFps          = 10000;
    uint8_t         framesInFlight  = 2;
    float           renderScale     = 1.f;
};



/* ========================================================================= //
    Renderer
    Features:
// ========================================================================= */
class Renderer {
public:
    static const uint8_t BACKBUFFER = 0;

    /* ---- lifecycle ------------------------------------------------------ */
    Renderer() = default;
    ~Renderer() { shutdown(); }

    bool init(const RenderConfig& cfg = {});
    void shutdown();

    /* ---- frame ---------------------------------------------------------- */
    void clear(const Color& clearColor = {0.f, 0.f, 0.f, 1.f}, const RenderTargetID target = BACKBUFFER, const float clearDepth = 0.f);
    void draw(const RenderCommand& cmd, const RenderTargetID target = BACKBUFFER);
    void show(const RenderTargetID target = BACKBUFFER);

    void flush();

    /* ---- render targets ------------------------------------------------- */
    RenderTargetID createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow = true, const bool srgb = false);
    void destroyTarget(const RenderTargetID target);

    /* ---- vertex buffers ------------------------------------------------- */
    VertexBufferID createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    void updateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout = {});
    void destroyVertexBuffer(const VertexBufferID buffer);

    /* ---- index buffers -------------------------------------------------- */
    IndexBufferID createIndexBuffer(const uint32_t* data, const size_t count);
    void updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count);
    void destroyIndexBuffer(const IndexBufferID buffer);

    /* ---- uniform buffers ------------------------------------------------ */
    UniformBufferID createUniformBuffer(const void* data, const size_t bytes);
    void updateUniformBuffer(const UniformBufferID uniforms, const void* data, const size_t bytes);
    void destroyUniformBuffer(const UniformBufferID buffer);

    /* ---- textures ------------------------------------------------------- */
    TextureID createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb = false, const bool mips = true);
    TextureID createTexture(const std::string& path, const bool srgb = false, const bool mips = true);
    void updateTexture(const TextureID texture, const uint32_t* data);
    void updateTexture(
        const TextureID texture, 
        const uint32_t x, const uint32_t y, 
        const uint32_t w, const uint32_t h, 
        const uint32_t* data
    );
    void destroyTexture(const TextureID texture);

    /* ---- programs ------------------------------------------------------- */
    template <typename V, typename U, typename Vy>
    ProgramID createProgram(void (*vs)(const V&, const U&, Vy&), void (*fs)(const Vy&, const U&)) {
        if (!m_freePrograms.empty()) {
            ProgramID id = m_freePrograms.back();
            m_freePrograms.pop_back();
            m_programs[id] = makeProgram(vs, fs);
            return id;
        }

        m_programs.push_back(makeProgram(vs, fs));
        return m_programs.size() - 1;
    }

    // depth/stencil-only program: no fragment stage at all (shadow maps, rect clips)
    template <typename V, typename U, typename Vy>
    ProgramID createProgram(void (*vs)(const V&, const U&, Vy&)) {
        Program p;
        p.vsThunk = &vsThunk<V, U, Vy>;
        p.vsFn    = reinterpret_cast<RawFn>(vs);
        p.fsThunk = nullptr;
        p.fsFn    = nullptr;
        p.varyingCount = sizeof(Vy) / sizeof(float);

        if (!m_freePrograms.empty()) {
            ProgramID id = m_freePrograms.back();
            m_freePrograms.pop_back();
            m_programs[id] = p;
            return id;
        }

        m_programs.push_back(p);
        return m_programs.size() - 1;
    }

    void destroyProgram(const ProgramID program);

    /* ---- state ---------------------------------------------------------- */
    void resize(const uint32_t w, const uint32_t h);

    void setVsync(const bool vsync);
    bool getVsync() const { return m_cfg.vsync; }

    void setRenderScale(const float scale) { m_cfg.renderScale = scale; }
    float getRenderScale() const { return m_cfg.renderScale; }

    /* ---- queries -------------------------------------------------------- */
    uint32_t width() const { return m_targets[BACKBUFFER]->width(); }
    uint32_t height() const { return m_targets[BACKBUFFER]->height(); }

protected:
    RenderConfig m_cfg;
    std::vector<std::unique_ptr<RenderTarget>> m_targets;
    SDL_Renderer*                   m_sdlRenderer = nullptr;
    SDL_Texture*                    m_sdlTexture = nullptr;
    uint64_t                        m_frame = 0;

    std::vector<VertexBuffer>       m_vertexBuffers;
    std::vector<IndexBuffer>        m_indexBuffers;
    std::vector<Program>            m_programs;
    std::vector<UniformBuffer>      m_uniformBuffers;

    // Deferred destroy queues
    std::vector<std::pair<RenderTargetID, uint64_t>>     m_destroyedTargets;
    std::vector<std::pair<VertexBufferID, uint64_t>>     m_destroyedVertexBuffers;
    std::vector<std::pair<IndexBufferID, uint64_t>>      m_destroyedIndexBuffers;
    std::vector<std::pair<ProgramID, uint64_t>>          m_destroyedPrograms;
    std::vector<std::pair<UniformBufferID, uint64_t>>    m_destroyedUniformBuffers;

    std::vector<RenderTargetID>     m_freeTargets;
    std::vector<VertexBufferID>     m_freeVertexBuffers;
    std::vector<IndexBufferID>      m_freeIndexBuffers;
    std::vector<ProgramID>          m_freePrograms;
    std::vector<UniformBufferID>    m_freeUniformBuffers;

    uint32_t m_texW, m_texH;

    ThreadPool m_pool;

    // triangles accumulate here across draws; see TriBatch in DrawFuncs.hpp
    TriBatch m_batch;
    const void* m_resolvedUniforms[MAX_UNIFORM_SLOTS] = {};

    // takes ownership of a target/texture, reusing a freed slot if there is one
    RenderTargetID pushTarget(std::unique_ptr<RenderTarget> target);
    bool recreateTexture(const uint32_t w, const uint32_t h);

    bool destroyed(const std::vector<std::pair<uint32_t, uint64_t>>& destroyQueue, uint32_t id) const {
        for (auto& [t, frame] : destroyQueue) if (t == id) return true;
        return false;
    }

    template <typename FreeList>
    bool inFreeList(const FreeList& freeList, const uint32_t id) const {
        for (auto& i : freeList) if (id == i) return true;
        return false;
    }

    // EMPTY_BUFFER, DEFAULT_PROGRAM and stale IDs are all out of range
    bool validTarget (const RenderTargetID id)  const { return id < m_targets.size()        && !destroyed(m_destroyedTargets, id)           && m_targets[id] && !inFreeList(m_freeTargets, id); }
    bool validVerts  (const VertexBufferID id)  const { return id < m_vertexBuffers.size()  && !destroyed(m_destroyedVertexBuffers, id)     && !inFreeList(m_freeVertexBuffers, id); }
    bool validIndices(const IndexBufferID id)   const { return id < m_indexBuffers.size()   && !destroyed(m_destroyedIndexBuffers, id)      && !inFreeList(m_freeIndexBuffers, id); }
    bool validProgram(const ProgramID id)       const { return id < m_programs.size()       && !destroyed(m_destroyedPrograms, id)          && !inFreeList(m_freePrograms, id); }
    bool validUniform(const UniformBufferID id) const { return id < m_uniformBuffers.size() && !destroyed(m_destroyedUniformBuffers, id)    && !inFreeList(m_freeUniformBuffers, id); }

    template <typename Queue, typename FreeList, typename Release>
    void retireQueue(Queue& queue, FreeList& freeList, Release release, const bool force) {
        size_t keep = 0;
        for (size_t i = 0; i < queue.size(); ++i) {
            const auto [id, frame] = queue[i];
            if (force || m_frame - frame >= m_cfg.framesInFlight) {
                release(id);
                freeList.push_back(id);
            } else queue[keep++] = queue[i];
        }
        queue.resize(keep);
    }

    // force: ignore the age check and release everything (shutdown)
    void destroyAllQueued(const bool force = false) {
        retireQueue(m_destroyedTargets, m_freeTargets,
            [this](RenderTargetID id)  { m_targets[id].reset(); }, force);

        retireQueue(m_destroyedVertexBuffers, m_freeVertexBuffers,
            [this](VertexBufferID id)  { m_vertexBuffers[id] = VertexBuffer{}; }, force);

        retireQueue(m_destroyedIndexBuffers, m_freeIndexBuffers,
            [this](IndexBufferID id)   { m_indexBuffers[id] = IndexBuffer{}; }, force);

        retireQueue(m_destroyedPrograms, m_freePrograms,
            [this](ProgramID id)       { m_programs[id] = Program{}; }, force);

        retireQueue(m_destroyedUniformBuffers, m_freeUniformBuffers,
            [this](UniformBufferID id) { m_uniformBuffers[id] = UniformBuffer{}; }, force);
    }

    friend class VALO;
};

} // namespace valo
