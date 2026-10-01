/*
    The Vulkan half of Renderer's backend table. Every entry in
    Renderer::RenderBackend has a definition here, so the table is complete the
    moment it is installed - an unimplemented entry announces itself instead of
    being null or silently returning garbage.

    These are Renderer members rather than free functions, matching
    SoftwareRenderer.cpp: it lets each body reach m_targets, m_vulkan and
    friends exactly as the software versions do.

    Backend state lives on Renderer::m_vulkan, a VulkanRenderer* declared in
    Renderer.hpp. Allocate it in vulkanInitBackend(), delete it in
    vulkanShutdown(); every other function just uses it. It is typed rather than
    void*, so no casting - but that means the delete has to happen in here,
    where VulkanRenderer is a complete type.
*/

#include "../Renderer.hpp"

#ifdef VALO_HAS_VULKAN

// pulls in VulkanContext / VulkanSwapchain / VulkanFrames
#include "../vulkan/VulkanRenderer.hpp"

#include <cstdio>

namespace valo {

namespace {

// Says so once per call site rather than once per call, so a frame loop hitting
// an unimplemented entry does not bury everything else in output.
void todo(const char* fn) {
    static std::vector<const char*> seen;
    for (const char* s : seen) if (s == fn) return;
    seen.push_back(fn);
    std::fprintf(stderr, "[vulkan] TODO: %s is not implemented\n", fn);
}

} // namespace


/* ========================================================================= //
    Lifecycle
// ========================================================================= */

bool Renderer::vulkanInitBackend() {
    m_vulkan = new VulkanRenderer();
    return m_vulkan->init(*m_cfg.window, m_cfg.framesInFlight);
}


void Renderer::vulkanShutdown() {
    delete m_vulkan;
    m_vulkan = nullptr;
}


/* ========================================================================= //
    Frame
// ========================================================================= */

void Renderer::vulkanClear(const Color& clearColor, const RenderTargetID target, const float clearDepth) {
    (void)target;
    m_vulkan->clear(clearColor, clearDepth);
}


void Renderer::vulkanDraw(const RenderCommand& cmd, const RenderTargetID target) {
    (void)cmd; (void)target;
    todo(__func__);
}


void Renderer::vulkanShow(const RenderTargetID target) {
    (void)target;
    m_vulkan->present();
}


void Renderer::vulkanFlush() { /* no flush for vulkan */}


/* ========================================================================= //
    Render targets
// ========================================================================= */

RenderTargetID Renderer::vulkanCreateTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb) {
    (void)w; (void)h; (void)tracksWindow; (void)srgb;
    todo(__func__);
    return EMPTY_TEXTURE;
}


/* ========================================================================= //
    Vertex buffers
// ========================================================================= */

VertexBufferID Renderer::vulkanCreateVertexBuffer(const void* data, const size_t count, const VtxLayout& layout) {
    (void)data; (void)count; (void)layout;
    todo(__func__);
    return EMPTY_BUFFER;
}


void Renderer::vulkanUpdateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout) {
    (void)vertices; (void)data; (void)count; (void)newLayout;
    todo(__func__);
}


/* ========================================================================= //
    Index buffers
// ========================================================================= */

IndexBufferID Renderer::vulkanCreateIndexBuffer(const uint32_t* data, const size_t count) {
    (void)data; (void)count;
    todo(__func__);
    return EMPTY_BUFFER;
}


void Renderer::vulkanUpdateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    (void)indices; (void)data; (void)count;
    todo(__func__);
}


/* ========================================================================= //
    Uniform buffers
// ========================================================================= */

UniformBufferID Renderer::vulkanCreateUniformBuffer(const void* data, const size_t bytes) {
    (void)data; (void)bytes;
    todo(__func__);
    return EMPTY_BUFFER;
}


void Renderer::vulkanUpdateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes) {
    (void)buffer; (void)data; (void)bytes;
    todo(__func__);
}


/* ========================================================================= //
    Textures
// ========================================================================= */

TextureID Renderer::vulkanCreateTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips) {
    (void)w; (void)h; (void)data; (void)srgb; (void)mips;
    todo(__func__);
    return EMPTY_TEXTURE;
}


// The stb decode is backend-agnostic; only the upload differs. Either repeat the
// RenderTarget::loadImageFromPath call here, or lift the decode into Renderer and
// drop this entry - see the open A/B question in PLAN.md.
TextureID Renderer::vulkanCreateTextureFromFile(const std::string& path, const bool srgb, const bool mips) {
    (void)path; (void)srgb; (void)mips;
    todo(__func__);
    return EMPTY_TEXTURE;
}


void Renderer::vulkanUpdateTexture(const TextureID texture, const uint32_t* data) {
    (void)texture; (void)data;
    todo(__func__);
}


void Renderer::vulkanUpdateTextureRect(
    const TextureID texture,
    const uint32_t x, const uint32_t y,
    const uint32_t w, const uint32_t h,
    const uint32_t* data
) {
    (void)texture; (void)x; (void)y; (void)w; (void)h; (void)data;
    todo(__func__);
}


/* ========================================================================= //
    Programs
// ========================================================================= */

// A Program full of C++ function pointers cannot become SPIR-V. Per PLAN.md the
// two shader implementations are separate by design, so this is a hard no rather
// than something to fall back from - the GPU path is createProgramFromFiles.
ProgramID Renderer::vulkanCreateProgramFromCallbacks(const Program& program) {
    (void)program;
    todo(__func__);
    return DEFAULT_PROGRAM;
}


ProgramID Renderer::vulkanCreateProgramFromFiles(const std::string& vsPath, const std::string& fsPath) {
    (void)vsPath; (void)fsPath;
    todo(__func__);
    return DEFAULT_PROGRAM;
}


/* ========================================================================= //
    Releases
// ========================================================================= */
// Reached from destroyAllQueued() once a resource has aged framesInFlight
// frames, so the GPU is guaranteed to be done with it. This is where the actual
// vkDestroy* calls belong; leaving them empty leaks everything, silently.

void Renderer::vulkanReleaseTarget(const RenderTargetID id)         { (void)id; todo(__func__); }
void Renderer::vulkanReleaseVertexBuffer(const VertexBufferID id)   { (void)id; todo(__func__); }
void Renderer::vulkanReleaseIndexBuffer(const IndexBufferID id)     { (void)id; todo(__func__); }
void Renderer::vulkanReleaseUniformBuffer(const UniformBufferID id) { (void)id; todo(__func__); }
void Renderer::vulkanReleaseProgram(const ProgramID id)             { (void)id; todo(__func__); }


/* ========================================================================= //
    State
// ========================================================================= */

void Renderer::vulkanResize(const uint32_t w, const uint32_t h) {
    (void)w; (void)h;
    todo(__func__);
}


// Present mode is baked into the swapchain, so this cannot just set a flag the
// way the software path does - it has to rebuild.
void Renderer::vulkanSetVsync(const bool vsync) {
    (void)vsync;
    todo(__func__);
}


/* ========================================================================= //
    Queries
// ========================================================================= */

uint32_t Renderer::vulkanWidth() const {
    // the swapchain extent, not a RenderTarget
    return 0;
}


uint32_t Renderer::vulkanHeight() const {
    return 0;
}

} // namespace valo

#endif // VALO_HAS_VULKAN
