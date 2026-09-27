#pragma once

#include <vector>
#include <cassert>
#include <iostream>
#include <memory>
#include <cstring>

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

    /*
        Rasterises any triangles still sitting in the batch. Called by VALO at
        the end of each layer, because the layer's command queue is cleared
        right afterwards and the batch holds pointers into it. clear() and
        show() flush too, so a caller driving Renderer directly stays correct
        without knowing the batch exists.
    */
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

    std::vector<VertexBuffer>       m_vertexBuffers;
    std::vector<IndexBuffer>        m_indexBuffers;
    std::vector<Program>            m_programs;
    std::vector<UniformBuffer>      m_uniformBuffers;

    std::vector<RenderTargetID>     m_freeTargets;
    std::vector<VertexBufferID>     m_freeVertexBuffers;
    std::vector<IndexBufferID>      m_freeIndexBuffers;
    std::vector<ProgramID>          m_freePrograms;
    std::vector<UniformBufferID>    m_freeUniformBuffers;

    uint32_t m_texW, m_texH;

    ThreadPool m_pool;

    // triangles accumulate here across draws; see TriBatch in DrawFuncs.hpp
    TriBatch m_batch;

    // takes ownership of a target/texture, reusing a freed slot if there is one
    RenderTargetID pushTarget(std::unique_ptr<RenderTarget> target);
    bool recreateTexture(const uint32_t w, const uint32_t h);

    // EMPTY_BUFFER, DEFAULT_PROGRAM and stale IDs are all out of range
    bool validTarget (const RenderTargetID id)  const { return id < m_targets.size() && m_targets[id]; }
    bool validVerts  (const VertexBufferID id)  const { return id < m_vertexBuffers.size(); }
    bool validIndices(const IndexBufferID id)   const { return id < m_indexBuffers.size(); }
    bool validProgram(const ProgramID id)       const { return id < m_programs.size(); }
    bool validUniform(const UniformBufferID id) const { return id < m_uniformBuffers.size(); }
};

} // namespace valo
