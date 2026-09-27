#include "Renderer.hpp"
#include <SDL3/SDL.h>

namespace valo {

/* ========================================================================= //
    Lifecycle
// ========================================================================= */

bool Renderer::init(const RenderConfig& cfg) {
    m_cfg = cfg;
    m_sdlRenderer = SDL_CreateRenderer(cfg.sdlWindow, NULL);

    int w, h;
    SDL_GetWindowSizeInPixels(cfg.sdlWindow, &w, &h);
    m_targets.push_back(std::make_unique<RenderTarget>(w * m_cfg.renderScale, h * m_cfg.renderScale));
    m_targets.back()->setSrgb(m_cfg.srgbBackbuffer);

    if (!m_sdlRenderer) {
        std::cout << "Renderer: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!recreateTexture(w * m_cfg.renderScale, h * m_cfg.renderScale)) return false;

    SDL_SetRenderVSync(m_sdlRenderer, m_cfg.vsync);

    return true;
}


void Renderer::shutdown() { 
    // the batch points into command queues and programs; none of it outlives this
    m_batch.tris.clear();
    m_batch.wires.clear();
    m_batch.states.clear();
    m_batch.target = nullptr;

    SDL_DestroyTexture(m_sdlTexture); 
    SDL_DestroyRenderer(m_sdlRenderer);
    m_sdlTexture = nullptr;
    m_sdlRenderer = nullptr;
}



/* ========================================================================= //
    Frame
// ========================================================================= */

void Renderer::flush() {
    const DrawContext ctx{&m_targets, nullptr};
    flushTriBatch(m_batch, m_pool, ctx);
}


void Renderer::clear(const Color& clearColor, const RenderTargetID target, const float clearDepth) { 
    assert(validTarget(target) && "Renderer: clear on invalid or destroyed target");
    if (!validTarget(target)) return;

    // anything queued was meant to land on the old contents
    flush();

    RenderTarget* tgt = m_targets[target].get();
    forEachBand(m_pool, tgt->height(), {&m_targets, nullptr}, [&](int y0, int y1) {
        tgt->clearRows(clearColor, clearDepth, uint32_t(y0), uint32_t(y1));
    });
    // m_targets[target]->clear(clearColor);
}


void Renderer::draw(const RenderCommand& cmd, const RenderTargetID target) {
    // every draw path indexes all three unconditionally, so a missing or stale
    // ID has to be rejected here rather than deeper in
    assert(validTarget(target)        && "draw(): invalid or destroyed render target");
    assert(validVerts(cmd.verts)      && "draw(): no vertex buffer bound");
    assert(validIndices(cmd.indices)  && "draw(): no index buffer bound");
    assert(validProgram(cmd.program)  && "draw(): no program bound");
    if (!validTarget(target) || !validVerts(cmd.verts)
     || !validIndices(cmd.indices) || !validProgram(cmd.program)) return;

    const void* resolved[MAX_UNIFORM_SLOTS];
    for (uint32_t i = 0; i < MAX_UNIFORM_SLOTS; ++i) {
        const UniformBufferID id = cmd.uniformBuffers[i];
        assert((id == EMPTY_BUFFER || validUniform(id)) && "draw(): stale uniform buffer bound");
        resolved[i] = validUniform(id) ? m_uniformBuffers[id].data.data() : nullptr;
    }
    
    const DrawContext ctx{&m_targets, resolved};
    detail::boundTargets = ctx.targets;
    detail::boundUniforms = ctx.uniforms;

    switch (cmd.topology) {
        case Topology::Triangles:
            drawTriangles(
                m_targets[target].get(), 
                m_vertexBuffers[cmd.verts], 
                m_indexBuffers[cmd.indices], 
                m_programs[cmd.program], 
                cmd,
                m_pool,
                ctx,
                m_batch
            );
            break;
        
        case Topology::Lines:
            flush();
            drawLines(
                m_targets[target].get(), 
                m_vertexBuffers[cmd.verts], 
                m_indexBuffers[cmd.indices], 
                m_programs[cmd.program], 
                cmd,
                m_pool,
                ctx
            );
            break;
        
        case Topology::Points:
            flush();
            drawPoints(
                m_targets[target].get(), 
                m_vertexBuffers[cmd.verts], 
                m_indexBuffers[cmd.indices], 
                m_programs[cmd.program], 
                cmd,
                m_pool,
                ctx
            );
            break;

        default: break;
    }
}


void Renderer::show(const RenderTargetID target) {
    flush();

    assert(validTarget(target) && "Renderer: show on invalid or destroyed target");
    if (!validTarget(target)) return;

    RenderTarget* tgt = m_targets[target].get();

    if ((tgt->width() != m_texW) || (tgt->height() != m_texH)) 
        recreateTexture(tgt->width(), tgt->height());

    SDL_UpdateTexture(m_sdlTexture, NULL, tgt->data(), tgt->width() * 4);
    SDL_RenderClear(m_sdlRenderer);
    SDL_RenderTexture(m_sdlRenderer, m_sdlTexture, NULL, NULL);
    SDL_RenderPresent(m_sdlRenderer);
}



/* ========================================================================= //
    Render targets
// ========================================================================= */

RenderTargetID Renderer::createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb) {
    auto target = std::make_unique<RenderTarget>(w, h, tracksWindow);
    target->setSrgb(srgb);
    return pushTarget(std::move(target));
}


void Renderer::destroyTarget(const RenderTargetID target) {
    assert(target != BACKBUFFER && "Renderer: cannot destroy the backbuffer");
    assert(validTarget(target) && "Renderer: invalid or already destroyed target");

    m_targets[target].reset();
    m_freeTargets.push_back(target);
}



/* ========================================================================= //
    Vertex buffers
// ========================================================================= */

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


void Renderer::destroyVertexBuffer(const VertexBufferID buffer) {
    assert(buffer < m_vertexBuffers.size() && "Renderer: invalid vertex buffer");

    m_vertexBuffers[buffer] = VertexBuffer{}; // releases the byte storage
    m_freeVertexBuffers.push_back(buffer);
}



/* ========================================================================= //
    Index buffers
// ========================================================================= */

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


void Renderer::updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    assert(indices < m_indexBuffers.size() && "updateIndexBuffer(): Index buffer ID out of range");
    m_indexBuffers[indices].indices.assign(data, data + count);
}


void Renderer::destroyIndexBuffer(const IndexBufferID buffer) {
    assert(buffer < m_indexBuffers.size() && "Renderer: invalid index buffer");

    m_indexBuffers[buffer] = IndexBuffer{}; // releases the index storage
    m_freeIndexBuffers.push_back(buffer);
}



/* ========================================================================= //
    Uniform buffers
// ========================================================================= */

UniformBufferID Renderer::createUniformBuffer(const void* data, const size_t bytes) {
    UniformBuffer newBuffer;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    newBuffer.data.assign(src, src + bytes);

    if (!m_freeUniformBuffers.empty()) {
        UniformBufferID id = m_freeUniformBuffers.back();
        m_freeUniformBuffers.pop_back();
        m_uniformBuffers[id] = std::move(newBuffer);
        return id;
    }

    m_uniformBuffers.push_back(std::move(newBuffer));
    return m_uniformBuffers.size() - 1;
}


void Renderer::updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes) {
    assert(buffer < m_uniformBuffers.size() && "updateUniformBuffer(): Uniform buffer ID out of range");
    
    const uint8_t* src = static_cast<const uint8_t*>(data);
    m_uniformBuffers[buffer].data.assign(src, src + bytes);
}


void Renderer::destroyUniformBuffer(const UniformBufferID buffer) {
    assert(buffer < m_uniformBuffers.size() && "destroyUniformBuffer(): Uniform buffer ID out of range");

    m_uniformBuffers[buffer] = UniformBuffer{};
    m_freeUniformBuffers.push_back(buffer);
}



/* ========================================================================= //
    Textures
// ========================================================================= */

TextureID Renderer::createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips) {
    auto texture = std::make_unique<RenderTarget>(RenderTarget::loadFromMemory(w, h, data));
    texture->setSrgb(srgb);
    if (mips) texture->generateMips();
    return pushTarget(std::move(texture));
}


TextureID Renderer::createTexture(const std::string& path, const bool srgb, const bool mips) {
    auto texture = std::make_unique<RenderTarget>(RenderTarget::loadImageFromPath(path));
    texture->setSrgb(srgb);
    if (mips) texture->generateMips();
    return pushTarget(std::move(texture));
}


void Renderer::updateTexture(const TextureID texture, const uint32_t* data) {
    assert(validTarget(texture) && "updateTexture(): invalid or destroyed texture");
    if (!validTarget(texture)) return;
    m_targets[texture]->update(data);
}


void Renderer::updateTexture(
    const TextureID texture, 
    const uint32_t x, const uint32_t y, 
    const uint32_t w, const uint32_t h, 
    const uint32_t* data
) { 
    assert(validTarget(texture) && "updateTexture(): invalid or destroyed texture");
    if (!validTarget(texture)) return;
    m_targets[texture]->update(x, y, w, h, data); 
}


void Renderer::destroyTexture(const TextureID texture) {
    assert(texture != BACKBUFFER && "Renderer: cannot destroy the backbuffer");
    assert(validTarget(texture) && "Renderer: invalid or already destroyed texture");

    m_targets[texture].reset();
    m_freeTargets.push_back(texture);
}



/* ========================================================================= //
    Programs
// ========================================================================= */

void Renderer::destroyProgram(const ProgramID program) {
    assert(program < m_programs.size() && "Renderer: invalid program");

    m_programs[program] = Program{}; // null out the function pointers
    m_freePrograms.push_back(program);
}



/* ========================================================================= //
    State
// ========================================================================= */

void Renderer::resize(const uint32_t w, const uint32_t h) {
    for (auto& tgt : m_targets)
        if (tgt && tgt->tracksWindow()) // destroyed slots are null
            tgt->resize(w * m_cfg.renderScale, h * m_cfg.renderScale);

    recreateTexture(w * m_cfg.renderScale, h * m_cfg.renderScale);
}


void Renderer::setVsync(const bool vsync) { 
    m_cfg.vsync = vsync; 
    SDL_SetRenderVSync(m_sdlRenderer, m_cfg.vsync); 
}



/* ========================================================================= //
    Internal
// ========================================================================= */

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


// the SDL streaming texture that show() blits the backbuffer through
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

} // namespace valo
