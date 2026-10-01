#include "../Renderer.hpp"
#include <SDL3/SDL.h>

namespace valo {

void Renderer::softwareFlush() {
    const DrawContext ctx{&m_targets, nullptr};
    flushTriBatch(m_batch, m_pool, ctx);
}

void Renderer::softwareClear(const Color& clearColor, const RenderTargetID target, const float clearDepth) { 
    assert(validTarget(target) && "Renderer: clear on invalid or destroyed target");
    if (!validTarget(target)) return;

    flush();

    RenderTarget* tgt = m_targets[target].get();
    forEachBand(m_pool, tgt->height(), {&m_targets, nullptr}, [&](int y0, int y1) {
        tgt->clearRows(clearColor, clearDepth, uint32_t(y0), uint32_t(y1));
    });
}

void Renderer::softwareDraw(const RenderCommand& cmd, const RenderTargetID target) {
    assert(validTarget(target)        && "draw(): invalid or destroyed render target");
    assert(validVerts(cmd.verts)      && "draw(): no vertex buffer bound");
    assert(validIndices(cmd.indices)  && "draw(): no index buffer bound");
    assert(validProgram(cmd.program)  && "draw(): no program bound");
    if (!validTarget(target) || !validVerts(cmd.verts)
     || !validIndices(cmd.indices) || !validProgram(cmd.program)) return;

    for (uint32_t i = 0; i < MAX_UNIFORM_SLOTS; ++i) {
        const UniformBufferID id = cmd.uniformBuffers[i];
        assert((id == EMPTY_BUFFER || validUniform(id)) && "draw(): stale uniform buffer bound");
        m_resolvedUniforms[i] = validUniform(id) ? m_uniformBuffers[id].data.data() : nullptr;
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

void Renderer::softwareShow(const RenderTargetID target) {
    flush();

    ++m_frame;

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


/* ---- lifecycle ---------------------------------------------------------- */

bool Renderer::softwareInitBackend() {
    m_sdlRenderer = SDL_CreateRenderer(m_cfg.sdlWindow, NULL);

    int w, h;
    SDL_GetWindowSizeInPixels(m_cfg.sdlWindow, &w, &h);
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


void Renderer::softwareShutdown() {
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

void Renderer::softwareReleaseTarget(const RenderTargetID id)        { m_targets[id].reset(); }
void Renderer::softwareReleaseVertexBuffer(const VertexBufferID id)  { m_vertexBuffers[id] = VertexBuffer{}; }
void Renderer::softwareReleaseIndexBuffer(const IndexBufferID id)    { m_indexBuffers[id] = IndexBuffer{}; }
void Renderer::softwareReleaseUniformBuffer(const UniformBufferID id){ m_uniformBuffers[id] = UniformBuffer{}; }
void Renderer::softwareReleaseProgram(const ProgramID id)            { m_programs[id] = Program{}; }


/* ---- queries ------------------------------------------------------------ */

uint32_t Renderer::softwareWidth()  const { return m_targets[BACKBUFFER]->width(); }
uint32_t Renderer::softwareHeight() const { return m_targets[BACKBUFFER]->height(); }



/* ---- resources and state ------------------------------------------------ */

RenderTargetID Renderer::softwareCreateTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb) {
    auto target = std::make_unique<RenderTarget>(w, h, tracksWindow);
    target->setSrgb(srgb);
    return pushTarget(std::move(target));
}

VertexBufferID Renderer::softwareCreateVertexBuffer(const void* data, const size_t count, const VtxLayout& layout) {
    size_t sizeInBytes = count * layout.stride();
    VertexBuffer newBuffer;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    newBuffer.layout = layout;
    newBuffer.verts.assign(bytes, bytes + sizeInBytes);

    return allocSlot(m_vertexBuffers, m_freeVertexBuffers, std::move(newBuffer));
}

void Renderer::softwareUpdateVertexBuffer(
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

IndexBufferID Renderer::softwareCreateIndexBuffer(const uint32_t* data, const size_t count) {
    IndexBuffer newBuffer;
    newBuffer.indices.assign(data, data + count);

    return allocSlot(m_indexBuffers, m_freeIndexBuffers, std::move(newBuffer));
}

void Renderer::softwareUpdateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    assert(indices < m_indexBuffers.size() && "updateIndexBuffer(): Index buffer ID out of range");
    m_indexBuffers[indices].indices.assign(data, data + count);
}

UniformBufferID Renderer::softwareCreateUniformBuffer(const void* data, const size_t bytes) {
    UniformBuffer newBuffer;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    newBuffer.data.assign(src, src + bytes);

    return allocSlot(m_uniformBuffers, m_freeUniformBuffers, std::move(newBuffer));
}

void Renderer::softwareUpdateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes) {
    assert(buffer < m_uniformBuffers.size() && "updateUniformBuffer(): Uniform buffer ID out of range");
    
    const uint8_t* src = static_cast<const uint8_t*>(data);
    m_uniformBuffers[buffer].data.assign(src, src + bytes);
}

TextureID Renderer::softwareCreateTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips) {
    auto texture = std::make_unique<RenderTarget>(RenderTarget::loadFromMemory(w, h, data));
    texture->setSrgb(srgb);
    if (mips) texture->generateMips();
    return pushTarget(std::move(texture));
}

TextureID Renderer::softwareCreateTextureFromFile(const std::string& path, const bool srgb, const bool mips) {
    auto texture = std::make_unique<RenderTarget>(RenderTarget::loadImageFromPath(path));
    texture->setSrgb(srgb);
    if (mips) texture->generateMips();
    return pushTarget(std::move(texture));
}

void Renderer::softwareUpdateTexture(const TextureID texture, const uint32_t* data) {
    assert(validTarget(texture) && "updateTexture(): invalid or destroyed texture");
    if (!validTarget(texture)) return;
    m_targets[texture]->update(data);
}

void Renderer::softwareUpdateTextureRect(
    const TextureID texture, 
    const uint32_t x, const uint32_t y, 
    const uint32_t w, const uint32_t h, 
    const uint32_t* data
) { 
    assert(validTarget(texture) && "updateTexture(): invalid or destroyed texture");
    if (!validTarget(texture)) return;
    m_targets[texture]->update(x, y, w, h, data); 
}

void Renderer::softwareResize(const uint32_t w, const uint32_t h) {
    for (auto& tgt : m_targets)
        if (tgt && tgt->tracksWindow()) // destroyed slots are null
            tgt->resize(w * m_cfg.renderScale, h * m_cfg.renderScale);

    recreateTexture(w * m_cfg.renderScale, h * m_cfg.renderScale);
}

void Renderer::softwareSetVsync(const bool vsync) { 
    m_cfg.vsync = vsync; 
    SDL_SetRenderVSync(m_sdlRenderer, m_cfg.vsync); 
}


/* ---- programs ----------------------------------------------------------- */

ProgramID Renderer::softwareCreateProgramFromCallbacks(const Program& program) {
    return allocSlot(m_programs, m_freePrograms, program);
}


// The software backend runs C++ callbacks, not SPIR-V. See PLAN.md: the two
// shader implementations are separate by design, so this is a hard no rather
// than something to fall back from.
ProgramID Renderer::softwareCreateProgramFromFiles(const std::string&, const std::string&) {
    std::cout << "Renderer: the software backend has no SPIR-V path; "
                 "use createProgram() with C++ callbacks" << std::endl;
    return DEFAULT_PROGRAM;
}

} // namespace valo
