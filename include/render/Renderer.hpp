#pragma once

#include <vector>
#include <cassert>
#include <iostream>
#include <memory>
#include <cstring>
#include <deque>
#include <utility>

#include "RenderCommand.hpp"
#include "software/RenderTarget.hpp"
#include "buffers/VertexBuffer.hpp"
#include "buffers/IndexBuffer.hpp"
#include "ShaderProgram.hpp"
#include "buffers/UniformBuffer.hpp"
#include "software/DrawFuncs.hpp"
#include "software/Sampling.hpp"
#include "software/ThreadPool.hpp"
#include "software/SoftwareRenderer.hpp"
#include "common/Math.hpp"

struct SDL_Texture;
struct SDL_Renderer;
struct SDL_Window;

namespace valo {

class Window;
class VulkanRenderer;

enum class Backend { Software, Vulkan };

struct RenderConfig {
    SDL_Window*     sdlWindow       = nullptr;
    Window*         window          = nullptr;
    bool            vsync           = false;
    bool            srgbBackbuffer  = false;

    /*
        Vulkan validation layers. Off by default because they are a development
        tool that costs far more than the driver they wrap: measured on a 7900
        XT, the showcase example runs at 26 fps with them and 120 (vsync capped)
        without, with ~87% of the frame inside the layer and 5.9% in the actual
        driver. Turn it on while working on the backend, never for a timed run.
    */
    bool            validation      = false;

    uint16_t        maxFps          = 10000;
    uint8_t         framesInFlight  = 2;
    float           renderScale     = 1.f;

    Backend         backend         = Backend::Software;
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
        // the template does the type erasure; the backend stores the result
        return m_backendFuncs.createProgramFromCallbacks(*this, makeProgram(vs, fs));
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

        return m_backendFuncs.createProgramFromCallbacks(*this, p);
    }

    /*
        The GPU half of the shader contract: GLSL or SPIR-V on disk. The two
        overloads above are the software half - a backend implements one and
        rejects the other, since a C++ function pointer cannot become SPIR-V.
    */
    ProgramID createProgram(const std::string& vsPath, const std::string& fsPath);

    void destroyProgram(const ProgramID program);

    /* ---- state ---------------------------------------------------------- */
    void resize(const uint32_t w, const uint32_t h);

    void setVsync(const bool vsync);
    bool getVsync() const { return m_cfg.vsync; }

    void setRenderScale(const float scale) { m_cfg.renderScale = scale; }
    float getRenderScale() const { return m_cfg.renderScale; }

    /* ---- queries -------------------------------------------------------- */
    uint32_t width() const;
    uint32_t height() const;

protected:
    RenderConfig m_cfg;
    uint64_t     m_frame = 0;

    SoftwareRenderer* m_software = nullptr;
    VulkanRenderer* m_vulkan = nullptr;

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

    // takes ownership of a target/texture, reusing a freed slot if there is one
    // (straddles: software storage, Renderer's free list)
    RenderTargetID pushTarget(std::unique_ptr<RenderTarget> target);

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
    /*
        These straddle: the bounds come from whichever backend is installed,
        via its slotCount entry, and the rest from Renderer's own queues and
        free lists. A released slot is always in one of those two - the destroy
        queue until it retires, the free list afterwards - so the bounds check
        plus those two is the whole test, with no backend-specific liveness
        probe needed.
    */
    uint32_t slots(const ResourceKind kind) const {
        return m_backendFuncs.slotCount ? m_backendFuncs.slotCount(*this, kind) : 0;
    }

    bool validTarget (const RenderTargetID id)  const { return id < slots(ResourceKind::Target)        && !destroyed(m_destroyedTargets, id)        && !inFreeList(m_freeTargets, id); }
    bool validVerts  (const VertexBufferID id)  const { return id < slots(ResourceKind::VertexBuffer)  && !destroyed(m_destroyedVertexBuffers, id)  && !inFreeList(m_freeVertexBuffers, id); }
    bool validIndices(const IndexBufferID id)   const { return id < slots(ResourceKind::IndexBuffer)   && !destroyed(m_destroyedIndexBuffers, id)   && !inFreeList(m_freeIndexBuffers, id); }
    bool validProgram(const ProgramID id)       const { return id < slots(ResourceKind::Program)       && !destroyed(m_destroyedPrograms, id)       && !inFreeList(m_freePrograms, id); }
    bool validUniform(const UniformBufferID id) const { return id < slots(ResourceKind::UniformBuffer) && !destroyed(m_destroyedUniformBuffers, id) && !inFreeList(m_freeUniformBuffers, id); }

    template <typename Storage, typename FreeList, typename T>
    static uint32_t allocSlot(Storage& storage, FreeList& freeList, T&& value) {
        if (!freeList.empty()) {
            const uint32_t id = freeList.back();
            freeList.pop_back();
            storage[id] = std::forward<T>(value);
            return id;
        }

        storage.push_back(std::forward<T>(value));
        return uint32_t(storage.size() - 1);
    }

    template <typename Queue, typename FreeList, typename Release>
    void retireQueue(Queue& queue, FreeList& freeList, Release release, const bool force) {
        size_t keep = 0;
        for (size_t i = 0; i < queue.size(); ++i) {
            const auto [id, frame] = queue[i];
            if (force || m_frame - frame > m_cfg.framesInFlight) {
                release(id);
                freeList.push_back(id);
            } else queue[keep++] = queue[i];
        }
        queue.resize(keep);
    }

    void destroyAllQueued(const bool force = false) {
        if (!m_backendFuncs.releaseTarget) return;

        retireQueue(m_destroyedTargets, m_freeTargets,
            [this](RenderTargetID id)  { m_backendFuncs.releaseTarget(*this, id); }, force);

        retireQueue(m_destroyedVertexBuffers, m_freeVertexBuffers,
            [this](VertexBufferID id)  { m_backendFuncs.releaseVertexBuffer(*this, id); }, force);

        retireQueue(m_destroyedIndexBuffers, m_freeIndexBuffers,
            [this](IndexBufferID id)   { m_backendFuncs.releaseIndexBuffer(*this, id); }, force);

        retireQueue(m_destroyedPrograms, m_freePrograms,
            [this](ProgramID id)       { m_backendFuncs.releaseProgram(*this, id); }, force);

        retireQueue(m_destroyedUniformBuffers, m_freeUniformBuffers,
            [this](UniformBufferID id) { m_backendFuncs.releaseUniformBuffer(*this, id); }, force);
    }

    struct RenderBackend {
        void (*clear)(Renderer&, const Color&, RenderTargetID, float);
        void (*draw)(Renderer&, const RenderCommand&, RenderTargetID);
        void (*show)(Renderer&, RenderTargetID);
        void (*flush)(Renderer&);

        RenderTargetID (*createTarget)(Renderer&, uint32_t, uint32_t, bool, bool);

        VertexBufferID (*createVertexBuffer)(Renderer&, const void*, const size_t, const VtxLayout&);
        void (*updateVertexBuffer)(Renderer&, VertexBufferID, const void*, const size_t, const VtxLayout&);

        IndexBufferID (*createIndexBuffer)(Renderer&, const uint32_t*, const size_t);
        void (*updateIndexBuffer)(Renderer&, IndexBufferID, const uint32_t*, size_t);

        UniformBufferID (*createUniformBuffer)(Renderer&, const void*, const size_t);
        void (*updateUniformBuffer)(Renderer&, UniformBufferID, const void*, const size_t);

        TextureID (*createTexture)(Renderer&, uint32_t, uint32_t, const uint32_t*, bool, bool);
        TextureID (*createTextureFromFile)(Renderer&, const std::string&, bool, bool);
        void (*updateTexture)(Renderer&, TextureID, const uint32_t*);
        void (*updateTextureRect)(Renderer&, TextureID, uint32_t, uint32_t, uint32_t, uint32_t, const uint32_t*);

        ProgramID (*createProgramFromCallbacks)(Renderer&, const Program&);
        ProgramID (*createProgramFromFiles)(Renderer&, const std::string&, const std::string&);

        void (*releaseTarget)(Renderer&, RenderTargetID);
        void (*releaseVertexBuffer)(Renderer&, VertexBufferID);
        void (*releaseIndexBuffer)(Renderer&, IndexBufferID);
        void (*releaseUniformBuffer)(Renderer&, UniformBufferID);
        void (*releaseProgram)(Renderer&, ProgramID);

        void (*resize)(Renderer&, uint32_t, uint32_t);
        void (*setVsync)(Renderer&, bool);

        uint32_t (*width)(const Renderer&);
        uint32_t (*height)(const Renderer&);

        bool (*initBackend)(Renderer&);
        void (*shutdown)(Renderer&);
        void (*waitIdle)(Renderer&);
        uint32_t (*slotCount)(const Renderer&, const ResourceKind);
    } m_backendFuncs{};

    RenderBackend fetchBackendFuncs();

    friend class VALO;
    friend class SoftwareRenderer;
    friend class VulkanRenderer;
};

} // namespace valo
