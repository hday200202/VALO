#include "Renderer.hpp"
#include <SDL3/SDL.h>

namespace valo {

bool Renderer::init(const RenderConfig& cfg) {
    m_cfg = cfg;
    m_sdlRenderer = SDL_CreateRenderer(cfg.sdlWindow, NULL);

    int w, h;
    SDL_GetWindowSizeInPixels(cfg.sdlWindow, &w, &h);
    m_targets.push_back(std::make_unique<RenderTarget>(w * m_cfg.renderScale, h * m_cfg.renderScale));

    if (!m_sdlRenderer) {
        std::cout << "Renderer: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!recreateTexture(w * m_cfg.renderScale, h * m_cfg.renderScale)) return false;

    SDL_SetRenderVSync(m_sdlRenderer, m_cfg.vsync);

    return true;
}

void Renderer::clear(const Color& clearColor, const RenderTargetID target) { 
    assert(m_targets[target] && "Renderer: clear on destroyed target");
    RenderTarget* tgt = m_targets[target].get();
    forEachBand(m_pool, tgt->height(), &m_targets, [&](int y0, int y1) {
        tgt->clearRows(clearColor, uint32_t(y0), uint32_t(y1));
    });
    // m_targets[target]->clear(clearColor);
}

void Renderer::draw(const RenderCommand& cmd, const RenderTargetID target) {
    detail::boundTargets = &m_targets;

    switch (cmd.topology) {
        case Topology::Triangles:
            drawTriangles(
                m_targets[target].get(), 
                m_vertexBuffers[cmd.verts], 
                m_indexBuffers[cmd.indices], 
                m_programs[cmd.program], 
                cmd,
                m_pool,
                &m_targets
            );
            break;
        
        case Topology::Lines:
            drawLines(
                m_targets[target].get(), 
                m_vertexBuffers[cmd.verts], 
                m_indexBuffers[cmd.indices], 
                m_programs[cmd.program], 
                cmd,
                m_pool,
                &m_targets
            );
            break;
        
        case Topology::Points:
            drawPoints(
                m_targets[target].get(), 
                m_vertexBuffers[cmd.verts], 
                m_indexBuffers[cmd.indices], 
                m_programs[cmd.program], 
                cmd,
                m_pool,
                &m_targets
            );
            break;

        default: break;
    }
}

void Renderer::show(const RenderTargetID target) {
    RenderTarget* tgt = m_targets[target].get();
    assert(tgt && "Renderer: show on destroyed target");

    if ((tgt->width() != m_texW) || (tgt->height() != m_texH)) 
        recreateTexture(tgt->width(), tgt->height());

    SDL_UpdateTexture(m_sdlTexture, NULL, tgt->data(), tgt->width() * 4);
    SDL_RenderClear(m_sdlRenderer);
    SDL_RenderTexture(m_sdlRenderer, m_sdlTexture, NULL, NULL);
    SDL_RenderPresent(m_sdlRenderer);
}


void Renderer::resize(const uint32_t w, const uint32_t h) {
    for (auto& tgt : m_targets)
        if (tgt && tgt->tracksWindow()) // destroyed slots are null
            tgt->resize(w * m_cfg.renderScale, h * m_cfg.renderScale);

    recreateTexture(w * m_cfg.renderScale, h * m_cfg.renderScale);
}


RenderTargetID Renderer::pushTarget(std::unique_ptr<RenderTarget> target) {
    if (!m_freeTargets.empty()) {
        RenderTargetID id = m_freeTargets.back();
        m_freeTargets.pop_back();
        m_targets[id] = std::move(target);
        return id;
    }

    m_targets.push_back(std::move(target));
    return m_targets.size() - 1;
}


RenderTargetID Renderer::createTarget(const uint32_t w, const uint32_t h,const bool tracksWindow) 
{ return pushTarget(std::make_unique<RenderTarget>(w, h, tracksWindow)); }


VertexBufferID Renderer::createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout) {
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


IndexBufferID Renderer::createIndexBuffer(const uint32_t* data, const size_t count) {
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


TextureID Renderer::createTexture(const uint32_t w, const uint32_t h, uint32_t* data) {
    return pushTarget(std::make_unique<RenderTarget>(
        RenderTarget::loadFromMemory(w, h, data)
    ));
}


void Renderer::updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    assert(indices < m_indexBuffers.size() && "updateIndexBuffer(): Index buffer ID out of range");
    m_indexBuffers[indices].indices.assign(data, data + count);
}


void Renderer::updateVertexBuffer(
    const VertexBufferID vertices, 
    const void* data, 
    const size_t count,
    const VtxLayout& newLayout
) {
    assert(vertices < m_vertexBuffers.size() && "updateVertexBuffer(): Vertex buffer ID out of range");
    auto& vb = m_vertexBuffers[vertices];
    vb.layout = newLayout.stride() == 0 ? vb.layout : newLayout;

    size_t sizeInBytes = count * vb.layout.stride();
    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    vb.verts.assign(bytes, bytes + sizeInBytes);
}


TextureID Renderer::createTexture(const std::string& path) {
    return pushTarget(std::make_unique<RenderTarget>(
        RenderTarget::loadImageFromPath(path)
    ));
}


void Renderer::destroyTarget(const RenderTargetID target) {
    assert(target != BACKBUFFER && "Renderer: cannot destroy the backbuffer");
    assert(target < m_targets.size() && m_targets[target] && "Renderer: invalid or already destroyed target");

    m_targets[target].reset();
    m_freeTargets.push_back(target);
}


void Renderer::destroyVertexBuffer(const VertexBufferID buffer) {
    assert(buffer < m_vertexBuffers.size() && "Renderer: invalid vertex buffer");

    m_vertexBuffers[buffer] = VertexBuffer{}; // releases the byte storage
    m_freeVertexBuffers.push_back(buffer);
}


void Renderer::destroyIndexBuffer(const IndexBufferID buffer) {
    assert(buffer < m_indexBuffers.size() && "Renderer: invalid index buffer");

    m_indexBuffers[buffer] = IndexBuffer{}; // releases the index storage
    m_freeIndexBuffers.push_back(buffer);
}


void Renderer::destroyProgram(const ProgramID program) {
    assert(program < m_programs.size() && "Renderer: invalid program");

    m_programs[program] = Program{}; // null out the function pointers
    m_freePrograms.push_back(program);
}


void Renderer::destroyTexture(const TextureID texture) {
    assert(texture != BACKBUFFER && "Renderer: cannot destroy the backbuffer");
    assert(texture < m_targets.size() && m_targets[texture] && "Renderer: invalid or already destroyed texture");

    m_targets[texture].reset();
    m_freeTargets.push_back(texture);
}

bool Renderer::recreateTexture(const uint32_t w, const uint32_t h) {
    SDL_DestroyTexture(m_sdlTexture);
    m_sdlTexture = SDL_CreateTexture(
        m_sdlRenderer, 
        SDL_PIXELFORMAT_ABGR8888, 
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

void Renderer::setVsync(const bool vsync) { 
    m_cfg.vsync = vsync; 
    SDL_SetRenderVSync(m_sdlRenderer, m_cfg.vsync); 
}

void Renderer::shutdown() { 
    SDL_DestroyTexture(m_sdlTexture); 
    SDL_DestroyRenderer(m_sdlRenderer);
    m_sdlTexture = nullptr;
    m_sdlRenderer = nullptr;
}

} // namespace valo