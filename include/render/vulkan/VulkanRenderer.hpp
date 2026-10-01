#pragma once

#ifdef VALO_HAS_VULKAN

#include "VulkanContext.hpp"
#include "VulkanSwapchain.hpp"
#include "VulkanFrames.hpp"
#include "../RenderCommand.hpp"
#include "../buffers/VertexBuffer.hpp"
#include "../ShaderProgram.hpp"
#include "../common/Math.hpp"

namespace valo {

class Renderer;


struct GPUBuffer {
    VkBuffer        buffer      = VK_NULL_HANDLE;
    VmaAllocation   allocation  = VK_NULL_HANDLE;
    VkDeviceSize    size        = 0;
    VtxLayout       layout;
};

struct GPUProgram {
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
};

struct DepthBuffer {
    VkImage         image       = VK_NULL_HANDLE;
    VmaAllocation   allocation  = VK_NULL_HANDLE;
    VkImageView     view        = VK_NULL_HANDLE;
};

inline constexpr uint64_t packPipelineState(const RenderCommand& cmd) {
    return  (uint64_t(cmd.topology)     <<  0)
         |  (uint64_t(cmd.cullmode)     <<  2)
         |  (uint64_t(cmd.winding)      <<  4)
         |  (uint64_t(cmd.blendmode)    <<  5)
         |  (uint64_t(cmd.depthCompare) <<  7)
         |  (uint64_t(cmd.fillmode)     << 10)
         |  (uint64_t(cmd.depthTest)    << 11)
         |  (uint64_t(cmd.depthWrite)   << 12)
         |  (uint64_t(cmd.colorWrite)   << 13);
}


struct PipelineKey {
    ProgramID   program;
    uint64_t    state;

    VtxLayout   layout;

    bool operator==(const PipelineKey& o) const {
        return program == o.program
            && state   == o.state
            && layout.getAttribs() == o.layout.getAttribs();
    }
};

class VulkanRenderer {
public:
    ~VulkanRenderer() { shutdown(); }

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool initDevice(Window& window, const uint32_t framesInFlight = 2, const bool validation = true);
    void shutdown();

    bool beginFrameIfNeeded(const VkAttachmentLoadOp loadOp, const Color& clearColor, const float clearDepth = 0.f);
    bool uploadBuffer(
        const void* data,
        const VkDeviceSize bytes,
        const VkBufferUsageFlags usage,
        GPUBuffer& out
    );

    explicit VulkanRenderer(Renderer& owner) : m_owner(&owner) {}

    bool            init();

    void            clear(const Color& clearColor, const RenderTargetID target, const float clearDepth);
    void            draw(const RenderCommand& cmd, const RenderTargetID target);
    void            show(const RenderTargetID target);
    void            flush();

    RenderTargetID  createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb);

    VertexBufferID  createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    void            updateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout);

    IndexBufferID   createIndexBuffer(const uint32_t* data, const size_t count);
    void            updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count);

    UniformBufferID createUniformBuffer(const void* data, const size_t bytes);
    void            updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes);

    TextureID       createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips);
    TextureID       createTextureFromFile(const std::string& path, const bool srgb, const bool mips);
    void            updateTexture(const TextureID texture, const uint32_t* data);
    void            updateTextureRect(const TextureID texture, const uint32_t x, const uint32_t y, const uint32_t w, const uint32_t h, const uint32_t* data);

    ProgramID       createProgramFromCallbacks(const Program& program);
    ProgramID       createProgramFromFiles(const std::string& vsPath, const std::string& fsPath);

    void            releaseTarget(const RenderTargetID id);
    void            releaseVertexBuffer(const VertexBufferID id);
    void            releaseIndexBuffer(const IndexBufferID id);
    void            releaseUniformBuffer(const UniformBufferID id);
    void            releaseProgram(const ProgramID id);

    void            resize(const uint32_t w, const uint32_t h);
    void            setVsync(const bool vsync);

    uint32_t        width() const;
    uint32_t        height() const;

private:
    bool recreateSwapchain();
    void retireBuffer(GPUBuffer& buf);
    void drainRetiredBuffers(const bool force = false);
    std::vector<uint32_t> loadSpirv(const std::string& path);
    VkShaderModule createShaderModule(const std::vector<uint32_t>& spirv);
    VkPipeline createPipeline(const RenderCommand& cmd, const GPUProgram& prog, const VtxLayout& layout);
    VkPipeline pipelineFor(const RenderCommand& cmd, const VtxLayout& layout);
    VkFormat pickDepthFormat() const;
    bool createDepthBuffers();
    void destroyDepthBuffers();
    
    Window*         m_window        = nullptr;
    VulkanContext   m_ctx;
    VulkanSwapchain m_swapchain;
    VulkanFrames    m_frames;
    Renderer*       m_owner         = nullptr;
    uint32_t        m_frameIndex    = 0;
    uint32_t        m_imageIndex    = 0;
    bool            m_frameOpen     = false;
    bool            m_renderingOpen = false;
    bool            m_needsRecreate = false;

    uint32_t        m_pushConstantSize = 0;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_uniformSetLayout = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR m_pushDescriptorSet = nullptr;

    std::vector<std::pair<GPUBuffer, uint64_t>> m_retiredBuffers;
    std::vector<GPUBuffer> m_vertexBuffers;
    std::vector<GPUBuffer> m_indexBuffers;
    std::vector<GPUBuffer> m_uniformBuffers;
    std::vector<GPUProgram> m_programs;
    std::vector<std::pair<PipelineKey, VkPipeline>> m_pipelines;
    std::vector<DepthBuffer> m_depth;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
};



inline constexpr VkFormat vkFormatOf(const VtxAttrib a) {
    switch (a) {
        case VtxAttrib::Float:  return VK_FORMAT_R32_SFLOAT;
        case VtxAttrib::Int32:  return VK_FORMAT_R32_SINT;
        case VtxAttrib::Uint32: return VK_FORMAT_R32_UINT;
        case VtxAttrib::Vec2f:  return VK_FORMAT_R32G32_SFLOAT;
        case VtxAttrib::Vec2i:  return VK_FORMAT_R32G32_SINT;
        case VtxAttrib::Vec2u:  return VK_FORMAT_R32G32_UINT;
        case VtxAttrib::Vec3f:  return VK_FORMAT_R32G32B32_SFLOAT;
        case VtxAttrib::Vec3i:  return VK_FORMAT_R32G32B32_SINT;
        case VtxAttrib::Vec3u:  return VK_FORMAT_R32G32B32_UINT;
        case VtxAttrib::Vec4f:  return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VtxAttrib::Vec4i:  return VK_FORMAT_R32G32B32A32_SINT;
        case VtxAttrib::Vec4u:  return VK_FORMAT_R32G32B32A32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}


inline constexpr VkCullModeFlags vkCullOf(const CullMode m) {
    switch (m) {
        case CullMode::None:  return VK_CULL_MODE_NONE;
        case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}


inline constexpr VkCompareOp vkCompareOf(const DepthCompare c) {
    switch (c) {
        case DepthCompare::Less:         return VK_COMPARE_OP_LESS;
        case DepthCompare::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case DepthCompare::Greater:      return VK_COMPARE_OP_GREATER;
        case DepthCompare::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case DepthCompare::Equal:        return VK_COMPARE_OP_EQUAL;
        case DepthCompare::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case DepthCompare::Always:       return VK_COMPARE_OP_ALWAYS;
        case DepthCompare::Never:        return VK_COMPARE_OP_NEVER;
    }
    return VK_COMPARE_OP_GREATER;
}

} // namespace valo

#endif // VALO_HAS_VULKAN