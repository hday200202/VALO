#pragma once

#include <vector>
#include <cassert>
#include <iostream>
#include <memory>
#include <cstring>
#include <deque>
#include <utility>

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

class Window;
class VulkanRenderer;

enum class Backend { Software, Vulkan };

struct RenderConfig {
    SDL_Window*     sdlWindow       = nullptr;
    // The Vulkan backend needs more than the raw handle: the surface comes from
    // sdlWindow, but the swapchain extent comes from Window::getSize(). Set by
    // VALO::init(); a Renderer driven directly must set it to use Backend::Vulkan.
    Window*         window          = nullptr;
    bool            vsync           = false;
    // backbuffer stores sRGB-encoded bytes; shader output is encoded on write
    bool            srgbBackbuffer  = false;

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
            if (force || m_frame - frame >= m_cfg.framesInFlight) {
                release(id);
                freeList.push_back(id);
            } else queue[keep++] = queue[i];
        }
        queue.resize(keep);
    }

    // force: ignore the age check and release everything (shutdown)
    void destroyAllQueued(const bool force = false) {
        // reachable from ~Renderer() on an object that never init()ed
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

        /*
            The deferred-destroy *timing* is backend-agnostic and stays in
            destroyAllQueued(); only the freeing differs. Without these the
            Vulkan backend would leak every resource silently, because the ID
            bookkeeping would still look correct.
        */
        void (*releaseTarget)(Renderer&, RenderTargetID);
        void (*releaseVertexBuffer)(Renderer&, VertexBufferID);
        void (*releaseIndexBuffer)(Renderer&, IndexBufferID);
        void (*releaseUniformBuffer)(Renderer&, UniformBufferID);
        void (*releaseProgram)(Renderer&, ProgramID);

        void (*resize)(Renderer&, uint32_t, uint32_t);
        void (*setVsync)(Renderer&, bool);

        uint32_t (*width)(const Renderer&);
        uint32_t (*height)(const Renderer&);

        // init() picks the table, so it is not in the table - but the
        // backend-specific half of startup still has to be dispatched
        bool (*initBackend)(Renderer&);
        void (*shutdown)(Renderer&);
    } m_backendFuncs{};   // zeroed: shutdown() may run before init()

    /*
        Owned by vulkanInitBackend(), freed by vulkanShutdown(). Typed rather
        than void* because only one backend needs state: the software path keeps
        everything in the members above. Forward-declared, so this costs the
        no-vulkan build nothing - it is a null pointer nothing dereferences.
        Deleting it needs the complete type, so that must stay inside
        VulkanBackend.cpp's guard.
    */
    VulkanRenderer* m_vulkan = nullptr;

    RenderBackend fetchBackendFuncs();

    /*
        Software backend implementations. Members rather than free functions so
        the bodies could move into SoftwareRenderer.cpp untouched; the table
        reaches them through the lambdas in fetchBackendFuncs().
    */
    bool softwareInitBackend();

    RenderTargetID  softwareCreateTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb);

    VertexBufferID  softwareCreateVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    void            softwareUpdateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout);

    IndexBufferID   softwareCreateIndexBuffer(const uint32_t* data, const size_t count);
    void            softwareUpdateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count);

    UniformBufferID softwareCreateUniformBuffer(const void* data, const size_t bytes);
    void            softwareUpdateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes);

    TextureID       softwareCreateTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips);
    TextureID       softwareCreateTextureFromFile(const std::string& path, const bool srgb, const bool mips);
    void            softwareUpdateTexture(const TextureID texture, const uint32_t* data);
    void            softwareUpdateTextureRect(const TextureID texture, const uint32_t x, const uint32_t y, const uint32_t w, const uint32_t h, const uint32_t* data);

    ProgramID       softwareCreateProgramFromCallbacks(const Program& program);
    ProgramID       softwareCreateProgramFromFiles(const std::string& vsPath, const std::string& fsPath);

    void            softwareResize(const uint32_t w, const uint32_t h);
    void            softwareSetVsync(const bool vsync);
    void softwareShutdown();

    void softwareReleaseTarget(const RenderTargetID id);
    void softwareReleaseVertexBuffer(const VertexBufferID id);
    void softwareReleaseIndexBuffer(const IndexBufferID id);
    void softwareReleaseUniformBuffer(const UniformBufferID id);
    void softwareReleaseProgram(const ProgramID id);

    uint32_t softwareWidth() const;
    uint32_t softwareHeight() const;

#ifdef VALO_HAS_VULKAN
    /*
        Vulkan backend. Declarations only need VALO types, but the bodies need
        the Vulkan headers, so both these and VulkanBackend.cpp are guarded -
        ./build.sh no-vulkan then has no Vulkan symbols at all, and init()
        rejects Backend::Vulkan because initBackend comes back null.
    */
    bool vulkanInitBackend();
    void vulkanShutdown();

    void vulkanClear(const Color& clearColor, const RenderTargetID target, const float clearDepth);
    void vulkanDraw(const RenderCommand& cmd, const RenderTargetID target);
    void vulkanShow(const RenderTargetID target);
    void vulkanFlush();

    RenderTargetID  vulkanCreateTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb);

    VertexBufferID  vulkanCreateVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    void            vulkanUpdateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout);

    IndexBufferID   vulkanCreateIndexBuffer(const uint32_t* data, const size_t count);
    void            vulkanUpdateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count);

    UniformBufferID vulkanCreateUniformBuffer(const void* data, const size_t bytes);
    void            vulkanUpdateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes);

    TextureID       vulkanCreateTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips);
    TextureID       vulkanCreateTextureFromFile(const std::string& path, const bool srgb, const bool mips);
    void            vulkanUpdateTexture(const TextureID texture, const uint32_t* data);
    void            vulkanUpdateTextureRect(const TextureID texture, const uint32_t x, const uint32_t y, const uint32_t w, const uint32_t h, const uint32_t* data);

    ProgramID       vulkanCreateProgramFromCallbacks(const Program& program);
    ProgramID       vulkanCreateProgramFromFiles(const std::string& vsPath, const std::string& fsPath);

    void vulkanReleaseTarget(const RenderTargetID id);
    void vulkanReleaseVertexBuffer(const VertexBufferID id);
    void vulkanReleaseIndexBuffer(const IndexBufferID id);
    void vulkanReleaseUniformBuffer(const UniformBufferID id);
    void vulkanReleaseProgram(const ProgramID id);

    void vulkanResize(const uint32_t w, const uint32_t h);
    void vulkanSetVsync(const bool vsync);

    uint32_t vulkanWidth() const;
    uint32_t vulkanHeight() const;
#endif

    void softwareFlush();
    void softwareClear(const Color& clearColor, const RenderTargetID target, const float clearDepth);
    void softwareDraw(const RenderCommand& cmd, const RenderTargetID target);
    void softwareShow(const RenderTargetID target);

    friend class VALO;
};

} // namespace valo
