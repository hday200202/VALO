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
    VkImageLayout   layout      = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct GPUTexture {
    VkImage         image           = VK_NULL_HANDLE;
    VmaAllocation   allocation      = VK_NULL_HANDLE;
    VkImageView     view            = VK_NULL_HANDLE;
    uint32_t        width           = 0;
    uint32_t        height          = 0;
    uint32_t        mipLevels       = 1;
    VkFormat        format          = VK_FORMAT_UNDEFINED;
    bool            isTarget        = false;
    bool            tracksWindow    = false;
    DepthBuffer     depth{};
    VkImageLayout   layout          = VK_IMAGE_LAYOUT_UNDEFINED;
};

inline constexpr uint32_t MAX_TEXTURES  = 4096;
inline constexpr uint32_t SAMPLER_COUNT = 18;   // Wrap x Wrap x Filter

inline constexpr uint64_t packPipelineState(const RenderCommand& cmd) {
    return  (uint64_t(cmd.topology)     <<  0)
         |  (uint64_t(cmd.cullmode)     <<  2)
         |  (uint64_t(cmd.winding)      <<  4)
         |  (uint64_t(cmd.blendmode)    <<  5)
         |  (uint64_t(cmd.depthCompare) <<  7)
         |  (uint64_t(cmd.fillmode)     << 10)
         |  (uint64_t(cmd.depthTest)    << 11)
         |  (uint64_t(cmd.depthWrite)   << 12)
         |  (uint64_t(cmd.colorWrite)   << 13)
         |  (uint64_t(cmd.stencilTest)  << 14)
         |  (uint64_t(cmd.stencilOp)    << 17)
         |  (uint64_t(cmd.stencilRef)   << 20);
}


struct PipelineKey {
    ProgramID   program;
    uint64_t    state;
    VkFormat    colorFormat;

    VtxLayout   layout;

    bool operator==(const PipelineKey& o) const {
        return program == o.program
            && state   == o.state
            && colorFormat == o.colorFormat
            && layout.getAttribs() == o.layout.getAttribs();
    }
};

class VulkanRenderer {
public:
    ~VulkanRenderer() { shutdown(); }

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool initDevice(Window& window, const uint32_t framesInFlight = 2, const bool validation = false);
    void shutdown();
    void waitIdle();

    uint32_t slotCount(const ResourceKind kind) const;

    bool beginFrameIfNeeded();
    bool uploadBuffer(
        const void* data,
        const VkDeviceSize bytes,
        const VkBufferUsageFlags usage,
        GPUBuffer& out
    );

    bool bindTarget(
        const RenderTargetID target,
        const VkAttachmentLoadOp loadOp,
        const Color& clearColor,
        const float clearDepth = 1.0f
    );

    void endRenderingIfOpen();

    bool uploadImage(GPUTexture& tex, const uint32_t* data);

    bool uploadImageRect(
        GPUTexture& tex,
        const uint32_t x, const uint32_t y,
        const uint32_t w, const uint32_t h,
        const uint32_t* data
    );

    bool createImage(
        const uint32_t w, const uint32_t h,
        const VkFormat format,
        const uint32_t mipLevels,
        const VkImageUsageFlags usage,
        GPUTexture& out
    );

    bool createTargetDepth(GPUTexture& tex);
    bool resizeTrackingTargets();

    VkExtent2D     scaledExtent() const;
    bool           applyRenderScale();
    RenderTargetID resolveTarget(const RenderTargetID target) const;

    void transitionImage(
        const VkCommandBuffer cmd,
        const VkImage image,
        const VkImageLayout from, const VkImageLayout to,
        const uint32_t baseMip, const uint32_t levelCount
    );

    void writeTextureDescriptor(const TextureID id);
    void clearTextureDescriptor(const TextureID id);

    void destroyTextureNow(GPUTexture& tex);

    explicit                VulkanRenderer(Renderer& owner) : m_owner(&owner) {}

    bool                    init();

    void                    clear(const Color& clearColor, const RenderTargetID target, const float clearDepth);
    void                    draw(const RenderCommand& cmd, const RenderTargetID target);
    void                    blitToSwapchain(const VkCommandBuffer cmd, GPUTexture& tex);
    void                    show(const RenderTargetID target);
    void                    flush();

    RenderTargetID          createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb);

    VertexBufferID          createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout);
    void                    updateVertexBuffer(const VertexBufferID vertices, const void* data, const size_t count, const VtxLayout& newLayout);

    IndexBufferID           createIndexBuffer(const uint32_t* data, const size_t count);
    void                    updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count);

    UniformBufferID         createUniformBuffer(const void* data, const size_t bytes);
    void                    updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes);

    StorageBufferID         createStorageBuffer(const void*, const size_t);
    void                    updateStorageBuffer(const StorageBufferID, const void*, const size_t);
    void                    releaseStorageBuffer(const StorageBufferID);

    TextureID               createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips);
    TextureID               createTextureFromFile(const std::string& path, const bool srgb, const bool mips);
    void                    updateTexture(const TextureID texture, const uint32_t* data);
    void                    updateTextureRect(const TextureID texture, const uint32_t x, const uint32_t y, const uint32_t w, const uint32_t h, const uint32_t* data);

    ProgramID               createProgramFromCallbacks(const Program& program);
    ProgramID               createProgramFromFiles(const std::string& vsPath, const std::string& fsPath);

    void                    releaseTarget(const RenderTargetID id);
    void                    releaseVertexBuffer(const VertexBufferID id);
    void                    releaseIndexBuffer(const IndexBufferID id);
    void                    releaseUniformBuffer(const UniformBufferID id);
    void                    releaseProgram(const ProgramID id);

    void                    resize(const uint32_t w, const uint32_t h);
    void                    setVsync(const bool vsync);

    uint32_t                width() const;
    uint32_t                height() const;

private:
    bool                    recreateSwapchain();
    void                    retireBuffer(GPUBuffer& buf);
    void                    drainRetiredBuffers(const bool force = false);
    std::vector<uint32_t>   loadSpirv(const std::string& path);
    VkShaderModule          createShaderModule(const std::vector<uint32_t>& spirv);
    VkPipeline              createPipeline(const RenderCommand& cmd, const GPUProgram& prog, const VtxLayout& layout, const VkFormat colorFormat);
    VkPipeline              pipelineFor(const RenderCommand& cmd, const VtxLayout& layout, const VkFormat colorFormat);
    VkFormat                pickDepthFormat() const;
    bool                    createDepthBuffers();
    void                    destroyDepthBuffers();
    bool                    createBindlessSet();
    bool                    createSamplers();
    
    Window*                         m_window                = nullptr;
    VulkanContext                   m_ctx;
    VulkanSwapchain                 m_swapchain;
    VulkanFrames                    m_frames;
    Renderer*                       m_owner                 = nullptr;
    uint32_t                        m_frameIndex            = 0;
    uint32_t                        m_imageIndex            = 0;
    bool                            m_frameOpen             = false;
    bool                            m_renderingOpen         = false;
    RenderTargetID                  m_currentTarget         = 0;    // Renderer::BACKBUFFER

    // the backbuffer when renderScale != 1: drawn at the scaled size and
    // blitted up to the swapchain on show, the way software stretches its own
    RenderTargetID                  m_scaledBackbuffer      = 0;
    float                           m_appliedScale          = 1.f;
    bool                            m_needsRecreate         = false;

    uint32_t                        m_pushConstantSize      = 0;

    VkPipelineLayout                m_pipelineLayout        = VK_NULL_HANDLE;
    VkDescriptorSetLayout           m_uniformSetLayout      = VK_NULL_HANDLE;
    VkDescriptorSetLayout           m_storageSetLayout      = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR   m_pushDescriptorSet     = nullptr;

    std::vector<std::pair<GPUBuffer, uint64_t>> 
                                    m_retiredBuffers;

    std::vector<GPUBuffer>          m_vertexBuffers;
    std::vector<GPUBuffer>          m_indexBuffers;
    std::vector<GPUBuffer>          m_uniformBuffers;
    std::vector<GPUBuffer>          m_storageBuffers;
    
    /*
        MAX_TEXTURES is the ceiling the shader is written against; the device may
        allow far fewer. MoltenVK reports maxPerSetDescriptors 1212, so a 4096
        array fails layout creation outright.
    */
    uint32_t                        m_maxTextures           = MAX_TEXTURES;

    std::vector<GPUTexture>         m_textures;
    std::vector<GPUProgram>         m_programs;
    std::vector<std::pair<PipelineKey, VkPipeline>> 
                                    m_pipelines;
    std::vector<DepthBuffer>        m_depth;
    std::vector<VkImageLayout>      m_swapLayouts;
    VkDescriptorSetLayout           m_bindlessLayout        = VK_NULL_HANDLE;
    VkDescriptorPool                m_bindlessPool          = VK_NULL_HANDLE;
    VkDescriptorSet                 m_bindlessSet           = VK_NULL_HANDLE;
    VkSampler                       m_samplers[SAMPLER_COUNT] = {};
    GPUTexture                      m_nullTexture{};
    VkFormat                        m_depthFormat           = VK_FORMAT_UNDEFINED;
    VkFormat                        m_currentColorFormat    = VK_FORMAT_UNDEFINED;
    VkExtent2D                      m_currentExtent         = {0, 0};
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


inline constexpr VkCompareOp vkStencilCompareOf(const StencilTest t) {
    switch (t) {
        case StencilTest::Always:   return VK_COMPARE_OP_ALWAYS;
        case StencilTest::Equal:    return VK_COMPARE_OP_EQUAL;
        case StencilTest::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case StencilTest::Less:     return VK_COMPARE_OP_LESS;
        case StencilTest::Greater:  return VK_COMPARE_OP_GREATER;
    }
    return VK_COMPARE_OP_ALWAYS;
}


inline constexpr VkStencilOp vkStencilOpOf(const StencilOp op) {
    switch (op) {
        case StencilOp::Keep:    return VK_STENCIL_OP_KEEP;
        case StencilOp::Zero:    return VK_STENCIL_OP_ZERO;
        case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
        case StencilOp::Incr:    return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case StencilOp::Decr:    return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
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

inline constexpr uint32_t samplerIndex(const Wrap u, const Wrap v, const Filter f) {
    return (uint32_t(u) * 3 + uint32_t(v)) * 2 + uint32_t(f);
}

} // namespace valo

#endif // VALO_HAS_VULKAN