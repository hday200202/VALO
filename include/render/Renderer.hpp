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
    Renderer() = default;
    ~Renderer() { shutdown(); }
    bool init(const RenderConfig& cfg = {});


    void clear(const Color& clearColor = {0.f, 0.f, 0.f, 1.f}, const RenderTargetID target = BACKBUFFER);
    void draw(const RenderCommand& cmd, const RenderTargetID target = BACKBUFFER);
    void show(const RenderTargetID target = BACKBUFFER);

    void resize(const uint32_t w, const uint32_t h);
    RenderTargetID createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow = true);
    VertexBufferID createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    IndexBufferID createIndexBuffer(const uint32_t* data, const size_t count);
    TextureID createTexture(const uint32_t w, const uint32_t h, uint32_t* data);
    TextureID createTexture(const std::string& path);
    void updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count);
    void updateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout = {});
    
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
    void destroyTarget(const RenderTargetID target);
    void destroyVertexBuffer(const VertexBufferID buffer);
    void destroyIndexBuffer(const IndexBufferID buffer);
    void destroyTexture(const TextureID texture);

    void setVsync(const bool vsync);
    bool getVsync() const { return m_cfg.vsync; }
    void setRenderScale(const float scale) { m_cfg.renderScale = scale; }
    float getRenderScale() const { return m_cfg.renderScale; }

    void shutdown();

    uint32_t width() const { return m_targets[BACKBUFFER]->width(); }
    uint32_t height() const { return m_targets[BACKBUFFER]->height(); }


    static const uint8_t BACKBUFFER = 0;

protected:
    RenderConfig m_cfg;
    std::vector<std::unique_ptr<RenderTarget>> m_targets;
    SDL_Renderer* m_sdlRenderer = nullptr;
    SDL_Texture* m_sdlTexture = nullptr;

    std::vector<VertexBuffer> m_vertexBuffers;
    std::vector<IndexBuffer> m_indexBuffers;
    std::vector<Program> m_programs;

    std::vector<RenderTargetID> m_freeTargets;
    std::vector<VertexBufferID> m_freeVertexBuffers;
    std::vector<IndexBufferID> m_freeIndexBuffers;
    std::vector<ProgramID> m_freePrograms;

    uint32_t m_texW, m_texH;

    ThreadPool m_pool;

    // takes ownership of a target/texture, reusing a freed slot if there is one
    RenderTargetID pushTarget(std::unique_ptr<RenderTarget> target);
    bool recreateTexture(const uint32_t w, const uint32_t h);
};

} // namespace valo