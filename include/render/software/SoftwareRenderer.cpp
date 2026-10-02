#include "SoftwareRenderer.hpp"
#include "../Renderer.hpp"   // m_owner: free lists, destroy queues, predicates, config

#include <SDL3/SDL.h>
#include <iostream>

namespace valo {

bool SoftwareRenderer::recreateTexture(const uint32_t w, const uint32_t h) {
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



void SoftwareRenderer::flush() {
    const DrawContext ctx{&m_targets, nullptr};
    flushTriBatch(m_batch, m_pool, ctx);
}

void SoftwareRenderer::clear(const Color& clearColor, const RenderTargetID target, const float clearDepth) { 
    assert(m_owner->validTarget(target) && "Renderer: clear on invalid or destroyed target");
    if (!m_owner->validTarget(target)) return;

    flush();

    RenderTarget* tgt = m_targets[target].get();
    forEachBand(m_pool, tgt->height(), {&m_targets, nullptr}, [&](int y0, int y1) {
        tgt->clearRows(clearColor, clearDepth, uint32_t(y0), uint32_t(y1));
    });
}

void SoftwareRenderer::draw(const RenderCommand& cmd, const RenderTargetID target) {
    assert(m_owner->validTarget(target)        && "draw(): invalid or destroyed render target");
    assert(m_owner->validVerts(cmd.verts)      && "draw(): no vertex buffer bound");
    assert(m_owner->validIndices(cmd.indices)  && "draw(): no index buffer bound");
    assert(m_owner->validProgram(cmd.program)  && "draw(): no program bound");
    if (!m_owner->validTarget(target) || !m_owner->validVerts(cmd.verts)
     || !m_owner->validIndices(cmd.indices) || !m_owner->validProgram(cmd.program)) return;

    for (uint32_t i = 0; i < MAX_UNIFORM_SLOTS; ++i) {
        const UniformBufferID id = cmd.uniformBuffers[i];
        assert((id == EMPTY_BUFFER || m_owner->validUniform(id)) && "draw(): stale uniform buffer bound");
        m_resolvedUniforms[i] = m_owner->validUniform(id) ? m_uniformBuffers[id].data.data() : nullptr;
    }
    
    const DrawContext ctx{&m_targets, m_resolvedUniforms};
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

void SoftwareRenderer::show(const RenderTargetID target) {
    flush();

    ++m_owner->m_frame;

    assert(m_owner->validTarget(target) && "Renderer: show on invalid or destroyed target");
    if (!m_owner->validTarget(target)) return;

    RenderTarget* tgt = m_targets[target].get();

    if ((tgt->width() != m_texW) || (tgt->height() != m_texH)) 
        recreateTexture(tgt->width(), tgt->height());

    SDL_UpdateTexture(m_sdlTexture, NULL, tgt->data(), tgt->width() * 4);
    SDL_RenderClear(m_sdlRenderer);
    SDL_RenderTexture(m_sdlRenderer, m_sdlTexture, NULL, NULL);
    SDL_RenderPresent(m_sdlRenderer);
}


/* ---- lifecycle ---------------------------------------------------------- */

bool SoftwareRenderer::init() {
    m_sdlRenderer = SDL_CreateRenderer(m_owner->m_cfg.sdlWindow, NULL);

    int w, h;
    SDL_GetWindowSizeInPixels(m_owner->m_cfg.sdlWindow, &w, &h);
    m_targets.push_back(std::make_unique<RenderTarget>(w * m_owner->m_cfg.renderScale, h * m_owner->m_cfg.renderScale));
    m_targets.back()->setSrgb(m_owner->m_cfg.srgbBackbuffer);

    if (!m_sdlRenderer) {
        std::cout << "Renderer: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!recreateTexture(w * m_owner->m_cfg.renderScale, h * m_owner->m_cfg.renderScale)) return false;

    SDL_SetRenderVSync(m_sdlRenderer, m_owner->m_cfg.vsync);

    return true;
}


void SoftwareRenderer::shutdown() {
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


/* ---- releases ----------------------------------------------------------- */
// Reached from destroyAllQueued() once a resource has aged framesInFlight
// frames. The vectors drop their storage; there is nothing else to free.

void SoftwareRenderer::releaseTarget(const RenderTargetID id)        { m_targets[id].reset(); }
void SoftwareRenderer::releaseVertexBuffer(const VertexBufferID id)  { m_vertexBuffers[id] = VertexBuffer{}; }
void SoftwareRenderer::releaseIndexBuffer(const IndexBufferID id)    { m_indexBuffers[id] = IndexBuffer{}; }
void SoftwareRenderer::releaseUniformBuffer(const UniformBufferID id){ m_uniformBuffers[id] = UniformBuffer{}; }
void SoftwareRenderer::releaseProgram(const ProgramID id)            { m_programs[id] = Program{}; }


/* ---- queries ------------------------------------------------------------ */

uint32_t SoftwareRenderer::width()  const { return m_targets[Renderer::BACKBUFFER]->width(); }
uint32_t SoftwareRenderer::height() const { return m_targets[Renderer::BACKBUFFER]->height(); }



/* ---- resources and state ------------------------------------------------ */

RenderTargetID SoftwareRenderer::createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb) {
    auto target = std::make_unique<RenderTarget>(w, h, tracksWindow);
    target->setSrgb(srgb);
    return m_owner->pushTarget(std::move(target));
}

VertexBufferID SoftwareRenderer::createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout) {
    size_t sizeInBytes = count * layout.stride();
    VertexBuffer newBuffer;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    newBuffer.layout = layout;
    newBuffer.verts.assign(bytes, bytes + sizeInBytes);

    return m_owner->allocSlot(m_vertexBuffers, m_owner->m_freeVertexBuffers, std::move(newBuffer));
}

void SoftwareRenderer::updateVertexBuffer(
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

IndexBufferID SoftwareRenderer::createIndexBuffer(const uint32_t* data, const size_t count) {
    IndexBuffer newBuffer;
    newBuffer.indices.assign(data, data + count);

    return m_owner->allocSlot(m_indexBuffers, m_owner->m_freeIndexBuffers, std::move(newBuffer));
}

void SoftwareRenderer::updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    assert(indices < m_indexBuffers.size() && "updateIndexBuffer(): Index buffer ID out of range");
    m_indexBuffers[indices].indices.assign(data, data + count);
}

UniformBufferID SoftwareRenderer::createUniformBuffer(const void* data, const size_t bytes) {
    UniformBuffer newBuffer;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    newBuffer.data.assign(src, src + bytes);

    return m_owner->allocSlot(m_uniformBuffers, m_owner->m_freeUniformBuffers, std::move(newBuffer));
}

void SoftwareRenderer::updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes) {
    assert(buffer < m_uniformBuffers.size() && "updateUniformBuffer(): Uniform buffer ID out of range");
    
    const uint8_t* src = static_cast<const uint8_t*>(data);
    m_uniformBuffers[buffer].data.assign(src, src + bytes);
}

TextureID SoftwareRenderer::createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips) {
    auto texture = std::make_unique<RenderTarget>(RenderTarget::loadFromMemory(w, h, data));
    texture->setSrgb(srgb);
    if (mips) texture->generateMips();
    return m_owner->pushTarget(std::move(texture));
}

TextureID SoftwareRenderer::createTextureFromFile(const std::string& path, const bool srgb, const bool mips) {
    auto texture = std::make_unique<RenderTarget>(RenderTarget::loadImageFromPath(path));
    texture->setSrgb(srgb);
    if (mips) texture->generateMips();
    return m_owner->pushTarget(std::move(texture));
}

void SoftwareRenderer::updateTexture(const TextureID texture, const uint32_t* data) {
    assert(m_owner->validTarget(texture) && "updateTexture(): invalid or destroyed texture");
    if (!m_owner->validTarget(texture)) return;
    m_targets[texture]->update(data);
}

void SoftwareRenderer::updateTextureRect(
    const TextureID texture, 
    const uint32_t x, const uint32_t y, 
    const uint32_t w, const uint32_t h, 
    const uint32_t* data
) { 
    assert(m_owner->validTarget(texture) && "updateTexture(): invalid or destroyed texture");
    if (!m_owner->validTarget(texture)) return;
    m_targets[texture]->update(x, y, w, h, data); 
}

void SoftwareRenderer::resize(const uint32_t w, const uint32_t h) {
    for (auto& tgt : m_targets)
        if (tgt && tgt->tracksWindow()) // destroyed slots are null
            tgt->resize(w * m_owner->m_cfg.renderScale, h * m_owner->m_cfg.renderScale);

    recreateTexture(w * m_owner->m_cfg.renderScale, h * m_owner->m_cfg.renderScale);
}

void SoftwareRenderer::setVsync(const bool vsync) { 
    m_owner->m_cfg.vsync = vsync; 
    SDL_SetRenderVSync(m_sdlRenderer, m_owner->m_cfg.vsync); 
}


/* ---- programs ----------------------------------------------------------- */

ProgramID SoftwareRenderer::createProgramFromCallbacks(const Program& program) {
    return m_owner->allocSlot(m_programs, m_owner->m_freePrograms, program);
}


// The software backend runs C++ callbacks, not SPIR-V. See PLAN.md: the two
// shader implementations are separate by design, so this is a hard no rather
// than something to fall back from.
ProgramID SoftwareRenderer::createProgramFromFiles(const std::string& vsPath, const std::string& fsPath) {
    const ShaderEntry* vs = findShader(vsPath);
    const ShaderEntry* fs = findShader(fsPath);

    if (!vs || !vs->vsThunk) {
        std::fprintf(stderr, "[software] no registered vertex shader for %s\n", vsPath.c_str());
        return DEFAULT_PROGRAM;
    }

    if (!fs || !fs->fsThunk) {
        std::fprintf(stderr, "[software] no registered fragment shader for %s\n", fsPath.c_str());
        return DEFAULT_PROGRAM;
    }

    Program p{
        .vsThunk        = vs->vsThunk,
        .vsFn           = vs->fn,
        .fsThunk        = fs->fsThunk,
        .fsFn           = fs->fn,
        .varyingCount   = vs->varyingCount
    };

    return m_owner->allocSlot(m_programs, m_owner->m_freePrograms, std::move(p));
}


} // namespace valo
