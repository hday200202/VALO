#include "Renderer.hpp"
#include <SDL3/SDL.h>

// the table lambdas call through these, so both need to be complete here
#include "software/SoftwareRenderer.hpp"
#ifdef VALO_HAS_VULKAN
    #include "vulkan/VulkanRenderer.hpp"
#endif

namespace valo {

/* ========================================================================= //
    Lifecycle
// ========================================================================= */

bool Renderer::init(const RenderConfig& cfg) {
    if (m_backendFuncs.shutdown) shutdown();
    m_cfg = cfg;

    // init is what chooses the table, so it is never in the table itself
    m_backendFuncs = fetchBackendFuncs();
    if (!m_backendFuncs.initBackend) {
        std::cout << "Renderer: no backend for the configured Backend enum" << std::endl;
        return false;
    }

    return m_backendFuncs.initBackend(*this);
}


void Renderer::shutdown() {
    if (!m_backendFuncs.shutdown) return;

    if (m_backendFuncs.waitIdle) m_backendFuncs.waitIdle(*this);
    destroyAllQueued(true);
    m_backendFuncs.shutdown(*this);
    m_backendFuncs = {};

    // the IDs belong to the backend that just went away
    m_freeTargets.clear();
    m_freeVertexBuffers.clear();
    m_freeIndexBuffers.clear();
    m_freePrograms.clear();
    m_freeUniformBuffers.clear();
    m_freeStorageBuffers.clear();
}


uint32_t Renderer::width()  const { return m_backendFuncs.width(*this); }
uint32_t Renderer::height() const { return m_backendFuncs.height(*this); }



/* ========================================================================= //
    Frame
// ========================================================================= */

void Renderer::flush() { m_backendFuncs.flush(*this); }


void Renderer::clear(const Color& clearColor, const RenderTargetID target, const float clearDepth) {
    m_backendFuncs.clear(*this, clearColor, target, clearDepth);
}


void Renderer::draw(const RenderCommand& cmd, const RenderTargetID target) {
    m_backendFuncs.draw(*this, cmd, target);
}


void Renderer::show(const RenderTargetID target) {
    m_backendFuncs.show(*this, target);
}



/* ========================================================================= //
    Render targets
// ========================================================================= */

RenderTargetID Renderer::createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb) {
    return m_backendFuncs.createTarget(*this, w, h, tracksWindow, srgb);
}


void Renderer::destroyTarget(const RenderTargetID target) {
    assert(target != BACKBUFFER && "Renderer: cannot destroy the backbuffer");
    assert(validTarget(target) && "Renderer: invalid or already destroyed target");
    if (target == BACKBUFFER || !validTarget(target)) return;
    m_destroyedTargets.push_back({target, m_frame});
}



/* ========================================================================= //
    Vertex buffers
// ========================================================================= */

VertexBufferID Renderer::createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout) {
    return m_backendFuncs.createVertexBuffer(*this, data, count, layout);
}


void Renderer::updateVertexBuffer(
    const VertexBufferID vertices,
    const void* data,
    const size_t count,
    const VtxLayout& newLayout
) {
    m_backendFuncs.updateVertexBuffer(*this, vertices, data, count, newLayout);
}


void Renderer::destroyVertexBuffer(const VertexBufferID buffer) {
    assert(validVerts(buffer) && "Renderer: invalid vertex buffer");
    if (!validVerts(buffer)) return;
    m_destroyedVertexBuffers.push_back({buffer, m_frame});
}



/* ========================================================================= //
    Index buffers
// ========================================================================= */

IndexBufferID Renderer::createIndexBuffer(const uint32_t* data, const size_t count) {
    return m_backendFuncs.createIndexBuffer(*this, data, count);
}


void Renderer::updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    m_backendFuncs.updateIndexBuffer(*this, indices, data, count);
}


void Renderer::destroyIndexBuffer(const IndexBufferID buffer) {
    assert(validIndices(buffer) && "Renderer: invalid index buffer");
    if (!validIndices(buffer)) return;
    m_destroyedIndexBuffers.push_back({buffer, m_frame});
}



/* ========================================================================= //
    Uniform buffers
// ========================================================================= */

UniformBufferID Renderer::createUniformBuffer(const void* data, const size_t bytes) {
    return m_backendFuncs.createUniformBuffer(*this, data, bytes);
}


void Renderer::updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes) {
    m_backendFuncs.updateUniformBuffer(*this, buffer, data, bytes);
}


void Renderer::destroyUniformBuffer(const UniformBufferID buffer) {
    assert(validUniform(buffer) && "destroyUniformBuffer(): Uniform buffer ID out of range");
    if (!validUniform(buffer)) return;
    m_destroyedUniformBuffers.push_back({buffer, m_frame});
}



/* ========================================================================= //
    Storage buffers
// ========================================================================= */

StorageBufferID Renderer::createStorageBuffer(const void* data, const size_t bytes) {
    return m_backendFuncs.createStorageBuffer(*this, data, bytes);
}


void Renderer::updateStorageBuffer(const StorageBufferID buffer, const void* data, const size_t bytes) {
    m_backendFuncs.updateStorageBuffer(*this, buffer, data, bytes);
}


void Renderer::destroyStorageBuffer(const StorageBufferID buffer) {
    assert(validStorage(buffer) && "destroyStorageBuffer(): Storage buffer ID out of range");
    if (!validStorage(buffer)) return;
    m_destroyedStorageBuffers.push_back({buffer, m_frame});
}



/* ========================================================================= //
    Textures
// ========================================================================= */

TextureID Renderer::createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips) {
    return m_backendFuncs.createTexture(*this, w, h, data, srgb, mips);
}


TextureID Renderer::createTexture(const std::string& path, const bool srgb, const bool mips) {
    return m_backendFuncs.createTextureFromFile(*this, path, srgb, mips);
}


void Renderer::updateTexture(const TextureID texture, const uint32_t* data) {
    m_backendFuncs.updateTexture(*this, texture, data);
}


void Renderer::updateTexture(
    const TextureID texture,
    const uint32_t x, const uint32_t y,
    const uint32_t w, const uint32_t h,
    const uint32_t* data
) {
    m_backendFuncs.updateTextureRect(*this, texture, x, y, w, h, data);
}


void Renderer::destroyTexture(const TextureID texture) {
    assert(texture != BACKBUFFER && "Renderer: cannot destroy the backbuffer");
    assert(validTarget(texture) && "Renderer: invalid or already destroyed texture");
    if (texture == BACKBUFFER || !validTarget(texture)) return;
    m_destroyedTargets.push_back({texture, m_frame});
}



/* ========================================================================= //
    Programs
// ========================================================================= */

ProgramID Renderer::createProgram(const std::string& vsPath, const std::string& fsPath) {
    /*
        The two halves are found by path, so nothing at compile time ties them
        together. A pair that disagrees on Uniforms or Varyings reads garbage on
        both backends, so it is refused here, before either backend sees it.
        Pure GLSL files have no registry entry and skip the check.
    */
    const ShaderEntry* vs = findShader(vsPath);
    const ShaderEntry* fs = findShader(fsPath);

    if (vs && fs && vs->vsThunk && fs->fsThunk) {
        if (vs->uniformsTag != fs->uniformsTag) {
            std::fprintf(stderr,
                "createProgram(): %s and %s take different Uniforms types (%zu and %zu bytes) - "
                "both halves must take the same struct\n",
                vsPath.c_str(), fsPath.c_str(), vs->uniformsSize, fs->uniformsSize);
            return DEFAULT_PROGRAM;
        }
        if (vs->varyingsTag != fs->varyingsTag) {
            std::fprintf(stderr,
                "createProgram(): %s writes different Varyings than %s reads (%zu and %zu bytes) - "
                "both halves must use the same struct\n",
                vsPath.c_str(), fsPath.c_str(), vs->varyingsSize, fs->varyingsSize);
            return DEFAULT_PROGRAM;
        }
    }

    return m_backendFuncs.createProgramFromFiles(*this, vsPath, fsPath);
}


void Renderer::destroyProgram(const ProgramID program) {
    assert(validProgram(program) && "Renderer: invalid program");
    if (!validProgram(program)) return;
    m_destroyedPrograms.push_back({program, m_frame});
}



/* ========================================================================= //
    State
// ========================================================================= */

void Renderer::resize(const uint32_t w, const uint32_t h) {
    m_backendFuncs.resize(*this, w, h);
}


void Renderer::setVsync(const bool vsync) {
    m_backendFuncs.setVsync(*this, vsync);
}



/* ========================================================================= //
    Internal
// ========================================================================= */

RenderTargetID Renderer::pushTarget(std::unique_ptr<RenderTarget> target) {
    return allocSlot(m_software->m_targets, m_freeTargets, std::move(target));
}


/*
    Captureless lambdas convert to plain function pointers, so each entry is a
    one-line adaptor from the table's (Renderer&, ...) shape to the member call.
    Type-checked at compile time: a signature drift stops the build here.
*/
Renderer::RenderBackend Renderer::fetchBackendFuncs() {
    switch (m_cfg.backend) {
        case Backend::Software: return RenderBackend{
            .clear = [](Renderer& r, const Color& c, RenderTargetID t, float d) { r.m_software->clear(c, t, d); },
            .draw  = [](Renderer& r, const RenderCommand& cmd, RenderTargetID t) { r.m_software->draw(cmd, t); },
            .show  = [](Renderer& r, RenderTargetID t) { r.m_software->show(t); },
            .flush = [](Renderer& r) { r.m_software->flush(); },

            .createTarget = [](Renderer& r, uint32_t w, uint32_t h, bool tracks, bool srgb) { return r.m_software->createTarget(w, h, tracks, srgb); },
            .createVertexBuffer = [](Renderer& r, const void* d, const size_t n, const VtxLayout& l) { return r.m_software->createVertexBuffer(d, n, l); },
            .updateVertexBuffer = [](Renderer& r, VertexBufferID id, const void* d, const size_t n, const VtxLayout& l) { r.m_software->updateVertexBuffer(id, d, n, l); },
            .createIndexBuffer = [](Renderer& r, const uint32_t* d, const size_t n) { return r.m_software->createIndexBuffer(d, n); },
            .updateIndexBuffer = [](Renderer& r, IndexBufferID id, const uint32_t* d, size_t n) { r.m_software->updateIndexBuffer(id, d, n); },
            .createUniformBuffer = [](Renderer& r, const void* d, const size_t n) { return r.m_software->createUniformBuffer(d, n); },
            .updateUniformBuffer = [](Renderer& r, UniformBufferID id, const void* d, const size_t n) { r.m_software->updateUniformBuffer(id, d, n); },
            .createStorageBuffer = [](Renderer& r, const void* d, const size_t n) { return r.m_software->createStorageBuffer(d, n); },
            .updateStorageBuffer = [](Renderer& r, StorageBufferID id, const void* d, const size_t n) { r.m_software->updateStorageBuffer(id, d, n); },
            .createTexture = [](Renderer& r, uint32_t w, uint32_t h, const uint32_t* d, bool srgb, bool mips) { return r.m_software->createTexture(w, h, d, srgb, mips); },
            .createTextureFromFile = [](Renderer& r, const std::string& p, bool srgb, bool mips) { return r.m_software->createTextureFromFile(p, srgb, mips); },
            .updateTexture = [](Renderer& r, TextureID id, const uint32_t* d) { r.m_software->updateTexture(id, d); },
            .updateTextureRect = [](Renderer& r, TextureID id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* d) { r.m_software->updateTextureRect(id, x, y, w, h, d); },
            .createProgramFromCallbacks = [](Renderer& r, const Program& p) { return r.m_software->createProgramFromCallbacks(p); },
            .createProgramFromFiles = [](Renderer& r, const std::string& vs, const std::string& fs) { return r.m_software->createProgramFromFiles(vs, fs); },

            .releaseTarget        = [](Renderer& r, RenderTargetID id)    { r.m_software->releaseTarget(id); },
            .releaseVertexBuffer  = [](Renderer& r, VertexBufferID id)    { r.m_software->releaseVertexBuffer(id); },
            .releaseIndexBuffer   = [](Renderer& r, IndexBufferID id)     { r.m_software->releaseIndexBuffer(id); },
            .releaseUniformBuffer = [](Renderer& r, UniformBufferID id)   { r.m_software->releaseUniformBuffer(id); },
            .releaseProgram       = [](Renderer& r, ProgramID id)         { r.m_software->releaseProgram(id); },
            .releaseStorageBuffer = [](Renderer& r, StorageBufferID id)   { r.m_software->releaseStorageBuffer(id); },

            .resize   = [](Renderer& r, uint32_t w, uint32_t h) { r.m_software->resize(w, h); },
            .setVsync = [](Renderer& r, bool v) { r.m_software->setVsync(v); },

            .width  = [](const Renderer& r) { return r.m_software->width(); },
            .height = [](const Renderer& r) { return r.m_software->height(); },

            .initBackend = [](Renderer& r) { return (r.m_software = new SoftwareRenderer(r), r.m_software->init()); },
            .shutdown    = [](Renderer& r) { r.m_software->shutdown(); delete r.m_software; r.m_software = nullptr; },
            .waitIdle    = [](Renderer&) {},
            .slotCount   = [](const Renderer& r, const ResourceKind k) { return r.m_software->slotCount(k); }
        };

        case Backend::Vulkan:
#ifdef VALO_HAS_VULKAN
            return RenderBackend{
            .clear = [](Renderer& r, const Color& c, RenderTargetID t, float d) { r.m_vulkan->clear(c, t, d); },
            .draw  = [](Renderer& r, const RenderCommand& cmd, RenderTargetID t) { r.m_vulkan->draw(cmd, t); },
            .show  = [](Renderer& r, RenderTargetID t) { r.m_vulkan->show(t); },
            .flush = [](Renderer& r) { r.m_vulkan->flush(); },

            .createTarget = [](Renderer& r, uint32_t w, uint32_t h, bool tracks, bool srgb) { return r.m_vulkan->createTarget(w, h, tracks, srgb); },
            .createVertexBuffer = [](Renderer& r, const void* d, const size_t n, const VtxLayout& l) { return r.m_vulkan->createVertexBuffer(d, n, l); },
            .updateVertexBuffer = [](Renderer& r, VertexBufferID id, const void* d, const size_t n, const VtxLayout& l) { r.m_vulkan->updateVertexBuffer(id, d, n, l); },
            .createIndexBuffer = [](Renderer& r, const uint32_t* d, const size_t n) { return r.m_vulkan->createIndexBuffer(d, n); },
            .updateIndexBuffer = [](Renderer& r, IndexBufferID id, const uint32_t* d, size_t n) { r.m_vulkan->updateIndexBuffer(id, d, n); },
            .createUniformBuffer = [](Renderer& r, const void* d, const size_t n) { return r.m_vulkan->createUniformBuffer(d, n); },
            .updateUniformBuffer = [](Renderer& r, UniformBufferID id, const void* d, const size_t n) { r.m_vulkan->updateUniformBuffer(id, d, n); },
            .createStorageBuffer = [](Renderer& r, const void* d, const size_t n) { return r.m_vulkan->createStorageBuffer(d, n); },
            .updateStorageBuffer = [](Renderer& r, StorageBufferID id, const void* d, const size_t n) { r.m_vulkan->updateStorageBuffer(id, d, n); },
            .createTexture = [](Renderer& r, uint32_t w, uint32_t h, const uint32_t* d, bool srgb, bool mips) { return r.m_vulkan->createTexture(w, h, d, srgb, mips); },
            .createTextureFromFile = [](Renderer& r, const std::string& p, bool srgb, bool mips) { return r.m_vulkan->createTextureFromFile(p, srgb, mips); },
            .updateTexture = [](Renderer& r, TextureID id, const uint32_t* d) { r.m_vulkan->updateTexture(id, d); },
            .updateTextureRect = [](Renderer& r, TextureID id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* d) { r.m_vulkan->updateTextureRect(id, x, y, w, h, d); },
            .createProgramFromCallbacks = [](Renderer& r, const Program& p) { return r.m_vulkan->createProgramFromCallbacks(p); },
            .createProgramFromFiles = [](Renderer& r, const std::string& vs, const std::string& fs) { return r.m_vulkan->createProgramFromFiles(vs, fs); },

            .releaseTarget        = [](Renderer& r, RenderTargetID id)    { r.m_vulkan->releaseTarget(id); },
            .releaseVertexBuffer  = [](Renderer& r, VertexBufferID id)    { r.m_vulkan->releaseVertexBuffer(id); },
            .releaseIndexBuffer   = [](Renderer& r, IndexBufferID id)     { r.m_vulkan->releaseIndexBuffer(id); },
            .releaseUniformBuffer = [](Renderer& r, UniformBufferID id)   { r.m_vulkan->releaseUniformBuffer(id); },
            .releaseProgram       = [](Renderer& r, ProgramID id)         { r.m_vulkan->releaseProgram(id); },
            .releaseStorageBuffer = [](Renderer& r, StorageBufferID id)   { r.m_vulkan->releaseStorageBuffer(id); },

            .resize   = [](Renderer& r, uint32_t w, uint32_t h) { r.m_vulkan->resize(w, h); },
            .setVsync = [](Renderer& r, bool v) { r.m_vulkan->setVsync(v); },

            .width  = [](const Renderer& r) { return r.m_vulkan->width(); },
            .height = [](const Renderer& r) { return r.m_vulkan->height(); },

            .initBackend = [](Renderer& r) { return (r.m_vulkan = new VulkanRenderer(r), r.m_vulkan->init()); },
            .shutdown    = [](Renderer& r) { r.m_vulkan->shutdown(); delete r.m_vulkan; r.m_vulkan = nullptr; },
            .waitIdle    = [](Renderer& r) { r.m_vulkan->waitIdle(); },
            .slotCount   = [](const Renderer& r, const ResourceKind k) { return r.m_vulkan->slotCount(k); }
        };
#else
            std::cout << "Renderer: Backend::Vulkan requested but this build has no "
                         "Vulkan (./build.sh without no-vulkan)" << std::endl;
            return RenderBackend{};
#endif
    }

    // unreachable for a valid enum, but a bad cast must not fall off the end
    return RenderBackend{};
}

} // namespace valo
