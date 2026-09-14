#pragma once

#include <SDL3/SDL.h>
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
#include "common/Math.hpp"

namespace valo {

struct RenderConfig {
    SDL_Window*     sdlWindow       = nullptr;
    bool            vsync           = false;
    uint16_t        maxFps          = 10000;
    uint8_t         framesInFlight  = 2;
};



class Renderer {
public:
    Renderer() = default;
    ~Renderer() { shutdown(); }
    bool init(const RenderConfig& cfg = {});


    void clear(
        const Color& clearColor = {0.f, 0.f, 0.f, 1.f}, 
        const RenderTargetID target = BACKBUFFER
    );

    void draw(const RenderCommand& cmd, const RenderTargetID target = BACKBUFFER);
    void show(const RenderTargetID target = BACKBUFFER);


    void resize(const uint32_t w, const uint32_t h);
    RenderTargetID createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow = true);
    VertexBufferID createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    IndexBufferID createIndexBuffer(const uint32_t* data, const size_t count);
    

    template <typename V, typename U, typename Vy>
    ProgramID createProgram(
        Vec4f (*vs)(const V&, const U&, Vy&), 
        Color (*fs)(const Vy&, const U&)
    ) {
        if (!m_freePrograms.empty()) {
            ProgramID id = m_freePrograms.back();
            m_freePrograms.pop_back();
            m_programs[id] = makeProgram(vs, fs);
            return id;
        }

        m_programs.push_back(makeProgram(vs, fs));
        return m_programs.size() - 1;
    }

    void destroyProgram(const ProgramID program);


    void destroyTarget(const RenderTargetID target);
    void destroyVertexBuffer(const VertexBufferID buffer);
    void destroyIndexBuffer(const IndexBufferID buffer);


    void setVsync(const bool vsync) { 
        m_cfg.vsync = vsync; 
        SDL_SetRenderVSync(m_sdlRenderer, m_cfg.vsync); 
    }


    bool getVsync() const { return m_cfg.vsync; }


    void shutdown() { 
        SDL_DestroyTexture(m_sdlTexture); 
        SDL_DestroyRenderer(m_sdlRenderer);
        m_sdlTexture = nullptr;
        m_sdlRenderer = nullptr;
    }


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


    bool recreateTexture(const uint32_t w, const uint32_t h) {
        SDL_DestroyTexture(m_sdlTexture);
        m_sdlTexture = SDL_CreateTexture(
            m_sdlRenderer, 
            SDL_PIXELFORMAT_RGBA8888, 
            SDL_TEXTUREACCESS_STREAMING, 
            w, h
        );

        if (!m_sdlTexture) {
            std::cout << "Renderer: " << SDL_GetError() << std::endl;
            return false;
        }

        m_texW = w; m_texH = h;
        return true;
    }
};



inline bool Renderer::init(const RenderConfig& cfg) {
    m_cfg = cfg;
    m_sdlRenderer = SDL_CreateRenderer(cfg.sdlWindow, NULL);

    int w, h;
    SDL_GetWindowSizeInPixels(cfg.sdlWindow, &w, &h);
    m_targets.push_back(std::make_unique<RenderTarget>(w, h));

    if (!m_sdlRenderer) {
        std::cout << "Renderer: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!recreateTexture(w, h)) return false;

    SDL_SetRenderVSync(m_sdlRenderer, m_cfg.vsync);

    return true;
}


inline void Renderer::clear(
    const Color& clearColor,
    const RenderTargetID target
) { 
    assert(m_targets[target] && "Renderer: clear on destroyed target");
    m_targets[target]->clear(clearColor); 
}


inline void Renderer::draw(
    const RenderCommand& cmd, 
    const RenderTargetID target
) {
    drawTriangles(
        m_targets[target].get(), 
        m_vertexBuffers[cmd.verts], 
        m_indexBuffers[cmd.indices], 
        m_programs[cmd.program], 
        cmd
    );
}


inline void Renderer::show(const RenderTargetID target) {
    RenderTarget* tgt = m_targets[target].get();
    assert(tgt && "Renderer: show on destroyed target");

    if ((tgt->width() != m_texW) || (tgt->height() != m_texH)) 
        recreateTexture(tgt->width(), tgt->height());

    SDL_UpdateTexture(m_sdlTexture, NULL, tgt->data(), tgt->width() * 4);
    SDL_RenderClear(m_sdlRenderer);
    SDL_RenderTexture(m_sdlRenderer, m_sdlTexture, NULL, NULL);
    SDL_RenderPresent(m_sdlRenderer);
}


inline void Renderer::resize(
    const uint32_t w, 
    const uint32_t h
) {
    for (auto& tgt : m_targets)
        if (tgt && tgt->tracksWindow()) // destroyed slots are null
            tgt->resize(w, h);

    SDL_DestroyTexture(m_sdlTexture);
    recreateTexture(w, h);
}


inline RenderTargetID Renderer::createTarget(
    const uint32_t w,
    const uint32_t h,
    const bool tracksWindow
) {
    if (!m_freeTargets.empty()) {
        RenderTargetID id = m_freeTargets.back();
        m_freeTargets.pop_back();
        m_targets[id] = std::make_unique<RenderTarget>(w, h, tracksWindow);
        return id;
    }

    m_targets.push_back(std::make_unique<RenderTarget>(w, h, tracksWindow));
    return m_targets.size() - 1;
}


inline VertexBufferID Renderer::createVertexBuffer(
    const void* data, 
    const size_t count, 
    const VtxLayout& layout
) {
    size_t sizeInBytes = count * layout.stride();
    VertexBuffer newBuffer;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    newBuffer.layout = layout;
    newBuffer.verts.assign(bytes, bytes + sizeInBytes);

    if (!m_freeVertexBuffers.empty()) {
        VertexBufferID id = m_freeVertexBuffers.back();
        m_freeVertexBuffers.pop_back();
        m_vertexBuffers[id] = std::move(newBuffer);
        return id;
    }

    m_vertexBuffers.push_back(std::move(newBuffer));
    return m_vertexBuffers.size() - 1;
}


inline IndexBufferID Renderer::createIndexBuffer(
    const uint32_t* data,
    const size_t count
) {
    IndexBuffer newBuffer;
    newBuffer.indices.assign(data, data + count);

    if (!m_freeIndexBuffers.empty()) {
        IndexBufferID id = m_freeIndexBuffers.back();
        m_freeIndexBuffers.pop_back();
        m_indexBuffers[id] = std::move(newBuffer);
        return id;
    }

    m_indexBuffers.push_back(std::move(newBuffer));
    return m_indexBuffers.size() - 1;
}


inline void Renderer::destroyTarget(const RenderTargetID target) {
    assert(target != BACKBUFFER && "Renderer: cannot destroy the backbuffer");
    assert(target < m_targets.size() && m_targets[target] && "Renderer: invalid or already destroyed target");

    m_targets[target].reset();
    m_freeTargets.push_back(target);
}


inline void Renderer::destroyVertexBuffer(const VertexBufferID buffer) {
    assert(buffer < m_vertexBuffers.size() && "Renderer: invalid vertex buffer");

    m_vertexBuffers[buffer] = VertexBuffer{}; // releases the byte storage
    m_freeVertexBuffers.push_back(buffer);
}


inline void Renderer::destroyIndexBuffer(const IndexBufferID buffer) {
    assert(buffer < m_indexBuffers.size() && "Renderer: invalid index buffer");

    m_indexBuffers[buffer] = IndexBuffer{}; // releases the index storage
    m_freeIndexBuffers.push_back(buffer);
}


inline void Renderer::destroyProgram(const ProgramID program) {
    assert(program < m_programs.size() && "Renderer: invalid program");

    m_programs[program] = Program{}; // null out the function pointers
    m_freePrograms.push_back(program);
}

} // namespace valo