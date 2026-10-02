#include "VulkanRenderer.hpp"
#include "../Renderer.hpp"   // m_owner: config, free lists, destroy queues

#ifdef VALO_HAS_VULKAN

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <stb_image.h>

namespace valo {


bool VulkanRenderer::initDevice(Window& window, const uint32_t framesInFlight, const bool validation) {
    m_window = &window;

    if (!m_ctx.init(window, validation)) return false;
    if (!m_swapchain.init(m_ctx, window.getSize(), m_owner->m_cfg.vsync)) return false;
    if (!m_frames.init(m_ctx, framesInFlight, m_swapchain.imageCount())) return false;

    return true;
}


bool VulkanRenderer::beginFrameIfNeeded(
    const VkAttachmentLoadOp loadOp,
    const Color& clearColor,
    const float clearDepth
) {
    if (m_frameOpen) return true;

    const VkDevice device = m_ctx.device();
    const uint32_t frame = m_frameIndex;

    if (m_needsRecreate) {
        if (!recreateSwapchain()) return false;
        if (m_needsRecreate) return false;
    }

    const VkFence fence = m_frames.inFlight(frame);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(
        device, m_swapchain.handle(), UINT64_MAX,
        m_frames.imageAvailable(frame), VK_NULL_HANDLE, &m_imageIndex
    );

    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m_needsRecreate = true;
        recreateSwapchain();
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;
    else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkAcquireNextImageKHR failed (%d)\n", r);
        return false;
    }

    vkResetFences(device, 1, &fence);

    const VkCommandBuffer cmd = m_frames.commandBuffer(frame);
    vkResetCommandBuffer(cmd, 0);

    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(cmd, &bi);

    const VkImageMemoryBarrier2 toAttachment{
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask           = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask          = 0,
        .dstStageMask           = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask          = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout              = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout              = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .image                  = m_swapchain.images()[m_imageIndex],
        .subresourceRange       = {
            .aspectMask         = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel       = 0,
            .levelCount         = 1,
            .baseArrayLayer     = 0,
            .layerCount         = 1
        }
    };

    const VkImageMemoryBarrier2 toDepth{
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask           = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask          = 0,
        .dstStageMask           = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask          = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout              = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout              = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .image                  = m_depth[frame].image,
        .subresourceRange       = {
            .aspectMask         = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            .baseMipLevel       = 0,
            .levelCount         = 1,
            .baseArrayLayer     = 0,
            .layerCount         = 1
        }
    };

    VkImageMemoryBarrier2 barriers[2] = { toAttachment, toDepth };

    const VkDependencyInfo depIn{
        .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount    = 2,
        .pImageMemoryBarriers       = barriers
    };
    vkCmdPipelineBarrier2(cmd, &depIn);

    const VkRenderingAttachmentInfo color{
        .sType          = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView      = m_swapchain.views()[m_imageIndex],
        .imageLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp         = loadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue     = { .color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}} }
    };

    const VkRenderingAttachmentInfo depth{
        .sType          = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView      = m_depth[frame].view,
        .imageLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp         = loadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue     = { .depthStencil = { .depth = clearDepth, .stencil = 0 } }
    };

    const VkRenderingInfo ri{
        .sType                  = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea             = { {0, 0}, m_swapchain.extent() },
        .layerCount             = 1,
        .colorAttachmentCount   = 1,
        .pColorAttachments      = &color,
        .pDepthAttachment       = &depth
    };

    vkCmdBeginRendering(cmd, &ri);

    m_frameOpen = true;
    m_renderingOpen = true;
    return true;
}


bool VulkanRenderer::uploadBuffer(
    const void* data,
    const VkDeviceSize bytes,
    const VkBufferUsageFlags usage,
    GPUBuffer& out
) {
    if (bytes == 0) return false;
    const VmaAllocator alloc = m_ctx.allocator();

    const VkBufferCreateInfo stagingCi{
        .sType          = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size           = bytes,
        .usage          = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode    = VK_SHARING_MODE_EXCLUSIVE
    };

    const VmaAllocationCreateInfo stagingAi{
        .flags  = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage  = VMA_MEMORY_USAGE_AUTO
    };

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};

    if (vmaCreateBuffer(alloc, &stagingCi, &stagingAi, &staging, &stagingAlloc, &stagingInfo) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] staging buffer allocation failed\n");
        return false;
    }

    std::memcpy(stagingInfo.pMappedData, data, size_t(bytes));

    const VkBufferCreateInfo ci{
        .sType          = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size           = bytes,
        .usage          = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode    = VK_SHARING_MODE_EXCLUSIVE
    };

    const VmaAllocationCreateInfo ai{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
    };

    if (vmaCreateBuffer(alloc, &ci, &ai, &out.buffer, &out.allocation, nullptr) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] device buffer allocation failed\n");
        vmaDestroyBuffer(alloc, staging, stagingAlloc);
        return false;
    }

    out.size = bytes;

    const VkCommandBuffer cmd = m_ctx.beginOneShot();
    if (!cmd) { vmaDestroyBuffer(alloc, staging, stagingAlloc); return false; }

    const VkBufferCopy region{ .srcOffset = 0, .dstOffset = 0, .size = bytes };
    vkCmdCopyBuffer(cmd, staging, out.buffer, 1, &region);

    const bool ok = m_ctx.endOneShot(cmd);

    vmaDestroyBuffer(alloc, staging, stagingAlloc);
    return ok;
}


bool VulkanRenderer::uploadImage(GPUTexture& tex, const uint32_t* data) {
    const VmaAllocator alloc = m_ctx.allocator();
    const VkDeviceSize bytes = VkDeviceSize(tex.width) * tex.height * 4;

    const VkBufferCreateInfo sci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    };

    const VmaAllocationCreateInfo sai{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO
    };

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};

    if (vmaCreateBuffer(alloc, &sci, &sai, &staging, &stagingAlloc, &stagingInfo) != VK_SUCCESS)
        return false;

    std::memcpy(stagingInfo.pMappedData, data, size_t(bytes));

    const VkCommandBuffer cmd = m_ctx.beginOneShot();
    if (!cmd) { vmaDestroyBuffer(alloc, staging, stagingAlloc); }

    transitionImage(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, tex.mipLevels);

    const VkBufferImageCopy region{
        .imageSubresource   = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent        = { tex.width, tex.height, 1 }
    };
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    int32_t mw = int32_t(tex.width), mh = int32_t(tex.height);

    for (uint32_t i = 1; i < tex.mipLevels; ++i) {
        transitionImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i - 1, 1);

        const int32_t nw = mw > 1 ? mw / 2 : 1;
        const int32_t nh = mh > 1 ? mh / 2 : 1;

        const VkImageBlit blit{
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 },
            .srcOffsets     = { {0, 0, 0}, {mw, mh, 1} },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i,     0, 1 },
            .dstOffsets     = { {0, 0, 0}, {nw, nh, 1} }
        };

        vkCmdBlitImage(
            cmd,
            tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR
        );

        transitionImage(
            cmd, tex.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 
            i - 1, 1
        );

        mw = nw;
        mh = nh;
    }

    transitionImage(
        cmd, tex.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        tex.mipLevels - 1, 1
    );

    const bool ok = m_ctx.endOneShot(cmd);
    vmaDestroyBuffer(alloc, staging, stagingAlloc);
    return ok;
}


void VulkanRenderer::writeTextureDescriptor(const TextureID id) {
    if (id >= m_textures.size() || !m_textures[id].view) return;

    const VkDescriptorImageInfo info{
        .imageView   = m_textures[id].view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    const VkWriteDescriptorSet write{
        .sType              = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet             = m_bindlessSet,
        .dstBinding         = 0,
        .dstArrayElement    = id,
        .descriptorCount    = 1,
        .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo         = &info
    };
    vkUpdateDescriptorSets(m_ctx.device(), 1,  &write, 0, nullptr);
}


// immediate, not deferred - only safe once the GPU is known to be idle, which
// is why the callers are the create failure path and shutdown
void VulkanRenderer::destroyTextureNow(GPUTexture& tex) {
    if (tex.view)  vkDestroyImageView(m_ctx.device(), tex.view, nullptr);
    if (tex.image) vmaDestroyImage(m_ctx.allocator(), tex.image, tex.allocation);

    tex = GPUTexture{};
}


bool VulkanRenderer::createImage(
    const uint32_t w, const uint32_t h,
    const VkFormat format,
    const uint32_t mipLevels,
    const VkImageUsageFlags usage,
    GPUTexture& out
) {
    const VkImageCreateInfo ici{
        .sType          = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType      = VK_IMAGE_TYPE_2D,
        .format         = format,
        .extent         = { w, h, 1 },
        .mipLevels      = mipLevels,
        .arrayLayers    = 1,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .tiling         = VK_IMAGE_TILING_OPTIMAL,
        .usage          = usage,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED
    };

    const VmaAllocationCreateInfo ai{ .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };

    if (vmaCreateImage(m_ctx.allocator(), &ici, &ai, &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vmaCreateImage failed\n");
        return false;
    }

    const VkImageViewCreateInfo vci{
        .sType              = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image              = out.image,
        .viewType           = VK_IMAGE_VIEW_TYPE_2D,
        .format             = format,
        .subresourceRange   = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = mipLevels,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };

    if (vkCreateImageView(m_ctx.device(), &vci, nullptr, &out.view) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateImageView failed\n");
        vmaDestroyImage(m_ctx.allocator(), out.image, out.allocation);
        out.image = VK_NULL_HANDLE;
        return false;
    }

    out.width       = w;
    out.height      = h;
    out.mipLevels   = mipLevels;
    out.format      = format;
    return true;
}


void VulkanRenderer::transitionImage(
    const VkCommandBuffer cmd,
    const VkImage image,
    const VkImageLayout from, const VkImageLayout to,
    const uint32_t baseMip, const uint32_t levelCount
) {
    const VkImageMemoryBarrier2 barrier{
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask           = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask          = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask           = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask          = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout              = from,
        .newLayout              = to,
        .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .image                  = image,
        .subresourceRange       = {
            .aspectMask         = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel       = baseMip,
            .levelCount         = levelCount,
            .baseArrayLayer     = 0,
            .layerCount         = 1
        }
    };

    const VkDependencyInfo dep{
        .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount    = 1,
        .pImageMemoryBarriers       = &barrier
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}


bool VulkanRenderer::recreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_ctx.physicalDevice(), m_ctx.surface(), &caps);

    const bool zeroSized =
        (caps.currentExtent.width == 0 || caps.currentExtent.height == 0);

    if (zeroSized) return true;


    vkDeviceWaitIdle(m_ctx.device());

    const uint32_t oldImageCount = m_swapchain.imageCount();

    m_swapchain.shutdown();
    if (!m_swapchain.init(m_ctx, m_window->getSize(), m_owner->m_cfg.vsync)) {
        std::fprintf(stderr, "[vulkan] swapchain recreate failed\n");
        return false;
    }

    if (m_swapchain.imageCount() != oldImageCount) {
        std::fprintf(
            stderr, "[vulkan] swapchain image count changed %u -> %u; VulkanFrames needs rebuilding\n",
            oldImageCount, m_swapchain.imageCount()
        );
        return false;
    }

    if (!createDepthBuffers()) return false;

    m_needsRecreate = false;
    return true;
}


void VulkanRenderer::retireBuffer(GPUBuffer& buf) {
    if (!buf.buffer) return;

    m_retiredBuffers.push_back({buf, m_owner->m_frame});
    buf = GPUBuffer{};
}


void VulkanRenderer::drainRetiredBuffers(const bool force) {
    const VmaAllocator alloc = m_ctx.allocator();

    size_t keep = 0;
    for (size_t i = 0; i < m_retiredBuffers.size(); ++i) {
        auto& [buf, frame] = m_retiredBuffers[i];

        if (force || m_owner->m_frame - frame >= m_owner->m_cfg.framesInFlight) {
            vmaDestroyBuffer(alloc, buf.buffer, buf.allocation);
        } else m_retiredBuffers[keep++] = m_retiredBuffers[i];
    }

    m_retiredBuffers.resize(keep);
}


std::vector<uint32_t> VulkanRenderer::loadSpirv(const std::string& path) {
    namespace fs = std::filesystem;

    std::string spvPath = path;

    if (fs::path(path).extension() != ".spv") {
        spvPath = path + ".spv";

        const bool stale = 
            !fs::exists(spvPath) || fs::last_write_time(spvPath) < fs::last_write_time(path);

        if (stale) {
            const std::string cmd = "glslc -I examples/shaders \"" + path + "\" -o \"" + spvPath + "\" 2>&1";

            std::fprintf(stderr, "[vulkan] compiling %s\n", path.c_str());
            if (std::system(cmd.c_str()) != 0) {
                std::fprintf(
                    stderr,
                    "[vulkan] glslc failed on %s - is ext/vulkan/env.sh sourced? "
                    "(precompiled .spv always works)\n",
                    path.c_str()
                );
                return {};
            }
        }
    }

    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "[vulkan] cannot open %s\n", spvPath.c_str());
        return {};
    }

    const std::streamsize bytes = file.tellg();
    file.seekg(0);

    if (bytes <= 0 || bytes % 4 != 0) {
        std::fprintf(stderr, "[vulkan] %s is not a whole number of 32-bit words\n", spvPath.c_str());
        return {};
    }

    std::vector<uint32_t> spirv(size_t(bytes) / 4);
    file.read(reinterpret_cast<char*>(spirv.data()), bytes);

    if (spirv[0] != 0x07230203) {
        std::fprintf(stderr, "[vulkan] %s is not SPIR-V (bad magic)\n", spvPath.c_str());
        return {};
    }

    return spirv;
}


VkShaderModule VulkanRenderer::createShaderModule(const std::vector<uint32_t>& spirv) {
    const VkShaderModuleCreateInfo ci{
        .sType      = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize   = spirv.size() * sizeof(uint32_t),
        .pCode      = spirv.data()
    };

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx.device(), &ci, nullptr, &module) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateShaderModule failed\n");
        return VK_NULL_HANDLE;
    }

    return module;
}


VkPipeline VulkanRenderer::createPipeline(
    const RenderCommand& cmd, 
    const GPUProgram& prog, 
    const VtxLayout& layout
) {
    const VkPipelineShaderStageCreateInfo stages[2]{
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = prog.vs,
            .pName  = "main"
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = prog.fs,
            .pName  = "main"
        }
    };

    const VkVertexInputBindingDescription binding{
        .binding    = 0,
        .stride     = uint32_t(layout.stride()),
        .inputRate  = VK_VERTEX_INPUT_RATE_VERTEX
    };

    std::vector<VkVertexInputAttributeDescription> attribs;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < layout.getAttribs().size(); ++i) {
        const VtxAttrib a = layout.getAttribs()[i];
        attribs.push_back({
            .location   = i,
            .binding    = 0,
            .format     = vkFormatOf(a),
            .offset     = offset
        });
        offset += uint32_t(attribSize(a));
    }

    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType                              = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount      = layout.stride() ? 1u : 0u,
        .pVertexBindingDescriptions         = &binding,
        .vertexAttributeDescriptionCount    = uint32_t(attribs.size()),
        .pVertexAttributeDescriptions       = attribs.data()
    };

    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType      = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology   = cmd.topology == Topology::Points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                    : cmd.topology == Topology::Lines  ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                    : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };

    const VkPipelineViewportStateCreateInfo viewport{
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount  = 1,
        .scissorCount   = 1
    };

    const VkPipelineRasterizationStateCreateInfo raster{
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode    = cmd.fillmode == FillMode::Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .cullMode       = vkCullOf(cmd.cullmode),
        .frontFace      = cmd.winding == VtxWinding::CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                         : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth      = 1.f
    };

    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples   = VK_SAMPLE_COUNT_1_BIT
    };

    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType              = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable    = cmd.depthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable   = cmd.depthWrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp     = vkCompareOf(cmd.depthCompare)
    };

    const VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable                = cmd.blendmode != BlendMode::Opaque ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor        = cmd.blendmode == BlendMode::Additive ? VK_BLEND_FACTOR_ONE
                                                                           : VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor        = cmd.blendmode == BlendMode::Additive ? VK_BLEND_FACTOR_ONE
                                                                           : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp               = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor        = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor        = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp               = VK_BLEND_OP_ADD,

        .colorWriteMask             = cmd.colorWrite ? (
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        ) : 0u
    };

    const VkPipelineColorBlendStateCreateInfo blend{
        .sType              = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount    = 1,
        .pAttachments       = &blendAttachment
    };

    const VkDynamicState    dynamics[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType              = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount  = 2,
        .pDynamicStates     = dynamics
    };

    const VkFormat colorFormat = m_swapchain.format();

    const VkPipelineRenderingCreateInfo rendering{
        .sType                      = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount       = 1,
        .pColorAttachmentFormats    = &colorFormat,
        .depthAttachmentFormat      = m_depthFormat
    };

    const VkGraphicsPipelineCreateInfo ci{
        .sType                  = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext                  = &rendering,
        .stageCount             = 2,
        .pStages                = stages,
        .pVertexInputState      = &vertexInput,
        .pInputAssemblyState    = &inputAssembly,
        .pViewportState         = &viewport,
        .pRasterizationState    = &raster,
        .pMultisampleState      = &multisample,
        .pDepthStencilState     = &depthStencil,
        .pColorBlendState       = &blend,
        .pDynamicState          = &dynamic,
        .layout                 = m_pipelineLayout,
        .renderPass             = VK_NULL_HANDLE
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateGraphicsPipelines failed\n");
        return VK_NULL_HANDLE;
    }

    return pipeline;
}


VkPipeline VulkanRenderer::pipelineFor(const RenderCommand& cmd, const VtxLayout& layout) {
    const PipelineKey key{ cmd.program, packPipelineState(cmd), layout };

    for (const auto& [k, pipeline] : m_pipelines)
        if (k == key) return pipeline;

    if (cmd.program >= m_programs.size()) return VK_NULL_HANDLE;

    const GPUProgram& prog = m_programs[cmd.program];
    if (!prog.vs || !prog.fs) return VK_NULL_HANDLE;

    const VkPipeline pipeline = createPipeline(cmd, prog, layout);
    if (pipeline) m_pipelines.push_back({key, pipeline});

    return pipeline;
}


void VulkanRenderer::destroyDepthBuffers() {
    if (!m_ctx.device()) return;

    for (auto& d : m_depth) {
        if (d.view)  vkDestroyImageView(m_ctx.device(), d.view, nullptr);
        if (d.image) vmaDestroyImage(m_ctx.allocator(), d.image, d.allocation);
    }

    m_depth.clear();
}


bool VulkanRenderer::createBindlessSet() {
    const VkDescriptorSetLayoutBinding bindings[2]{
        {
            .binding            = 0,
            .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount    = MAX_TEXTURES,
            .stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT
        },
        {
            .binding            = 1,
            .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount    = 18,
            .stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT
        }
    };

    const VkDescriptorBindingFlags flags[2]{
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        0u
    };

    const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{
        .sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount   = 2,
        .pBindingFlags  = flags
    };

    const VkDescriptorSetLayoutCreateInfo dsl{
        .sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext          = &bindingFlags,
        .flags          = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount   = 2,
        .pBindings      = bindings
    };

    if (vkCreateDescriptorSetLayout(m_ctx.device(), &dsl, nullptr, &m_bindlessLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] bindless set layout failed\n");
        return false;
    }

    const VkDescriptorPoolSize sizes[2]{
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 18}
    };

    const VkDescriptorPoolCreateInfo pci{
        .sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags          = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets        = 1,
        .poolSizeCount  = 2,
        .pPoolSizes     = sizes
    };

    if (vkCreateDescriptorPool(m_ctx.device(), &pci, nullptr, &m_bindlessPool) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] bindless pool failed\n");
        return false;
    }

    const VkDescriptorSetAllocateInfo ai{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = m_bindlessPool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &m_bindlessLayout
    };

    if (vkAllocateDescriptorSets(m_ctx.device(), &ai, &m_bindlessSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] bindless set allocation failed\n");
        return false;
    }

    return true;
}


bool VulkanRenderer::createSamplers() {
    auto vkWrap = [](const Wrap w) {
        switch (w) {
            case Wrap::Repeat:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case Wrap::Clamp:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case Wrap::Mirror:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }

        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };

    for (uint32_t u = 0; u < 3; ++u) {
        for (uint32_t v = 0; v < 3; ++v) {
            for (uint32_t f = 0; f < 2; ++f) {
                const VkFilter filter = f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                const VkSamplerCreateInfo sci{
                    .sType          = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                    .magFilter      = filter,
                    .minFilter      = filter,
                    .mipmapMode     = f ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                    .addressModeU   = vkWrap(Wrap(u)),
                    .addressModeV   = vkWrap(Wrap(v)),
                    .addressModeW   = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                    .maxLod         = VK_LOD_CLAMP_NONE
                };

                if (
                    vkCreateSampler(
                        m_ctx.device(), &sci, nullptr, 
                        &m_samplers[samplerIndex(
                            Wrap(u), Wrap(v), filter ? Filter::Linear : Filter::Nearest
                        )]
                    ) != VK_SUCCESS
                ) {
                    std::fprintf(stderr, "[vulkan] vkCreateSampler failed\n");
                    return false;
                }
            }
        }
    }

    VkDescriptorImageInfo infos[18]{};
    for (uint32_t i = 0; i < 18; ++i) infos[i].sampler = m_samplers[i];

    const VkWriteDescriptorSet write{
        .sType              = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet             = m_bindlessSet,
        .dstBinding         = 1,
        .descriptorCount    = 18,
        .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo         = infos
    };
    vkUpdateDescriptorSets(m_ctx.device(), 1, &write, 0, nullptr);


    return true;
}


VkFormat VulkanRenderer::pickDepthFormat() const {
    const VkFormat candidates[]{ VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };

    for (const auto f : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_ctx.physicalDevice(), f, &props);

        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return f;
    }

    std::fprintf(stderr, "[vulkan] no supported depth/stencil format\n");
    return VK_FORMAT_UNDEFINED;
}


bool VulkanRenderer::createDepthBuffers() {
    destroyDepthBuffers();

    const VkExtent2D extent = m_swapchain.extent();
    m_depth.resize(m_frames.framesInFlight());

    for (auto& d : m_depth) {
        const VkImageCreateInfo ici{
            .sType          = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType      = VK_IMAGE_TYPE_2D,
            .format         = m_depthFormat,
            .extent         = { extent.width, extent.height, 1 },
            .mipLevels      = 1,
            .arrayLayers    = 1,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .tiling         = VK_IMAGE_TILING_OPTIMAL,
            .usage          = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED
        };

        const VmaAllocationCreateInfo aci{ .usage = VMA_MEMORY_USAGE_AUTO };

        if (vmaCreateImage(m_ctx.allocator(), &ici, &aci, &d.image, &d.allocation, nullptr) != VK_SUCCESS) {
            std::fprintf(stderr, "[vulkan] vmaCreateImage failed for the depth buffer\n");
            return false;
        }

        const VkImageViewCreateInfo vci{
            .sType              = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image              = d.image,
            .viewType           = VK_IMAGE_VIEW_TYPE_2D,
            .format             = m_depthFormat,
            .subresourceRange   = {
                .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1
            }
        };

        if (vkCreateImageView(m_ctx.device(), &vci, nullptr, &d.view) != VK_SUCCESS) {
            std::fprintf(stderr, "[vulkan] vkCreateImageView failed for the depth buffer\n");
            return false;
        }
    }

    return true;
}


void VulkanRenderer::shutdown() {
    if (m_ctx.device()) vkDeviceWaitIdle(m_ctx.device());

    drainRetiredBuffers(true);

    destroyDepthBuffers();

    for (auto& [key, pipeline] : m_pipelines)
        vkDestroyPipeline(m_ctx.device(), pipeline, nullptr);
    m_pipelines.clear();

    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_ctx.device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    if (m_uniformSetLayout) {
        vkDestroyDescriptorSetLayout(m_ctx.device(), m_uniformSetLayout, nullptr);
        m_uniformSetLayout = VK_NULL_HANDLE;
    }

    for (VkSampler& s : m_samplers) {
        if (s) vkDestroySampler(m_ctx.device(), s, nullptr);
        s = VK_NULL_HANDLE;
    }

    // m_bindlessSet is not destroyed; it dies with the pool
    if (m_bindlessPool) {
        vkDestroyDescriptorPool(m_ctx.device(), m_bindlessPool, nullptr);
        m_bindlessPool = VK_NULL_HANDLE;
    }

    if (m_bindlessLayout) {
        vkDestroyDescriptorSetLayout(m_ctx.device(), m_bindlessLayout, nullptr);
        m_bindlessLayout = VK_NULL_HANDLE;
    }

    m_bindlessSet = VK_NULL_HANDLE;

    const VmaAllocator alloc = m_ctx.allocator();
    for (auto& b : m_vertexBuffers)
        if (b.buffer) vmaDestroyBuffer(alloc, b.buffer, b.allocation);
    for (auto& b : m_indexBuffers)
        if (b.buffer) vmaDestroyBuffer(alloc, b.buffer, b.allocation);
    for (auto& b : m_uniformBuffers)
        if (b.buffer) vmaDestroyBuffer(alloc, b.buffer, b.allocation);

    for (auto& p : m_programs) {
        if (p.vs) vkDestroyShaderModule(m_ctx.device(), p.vs, nullptr);
        if (p.fs) vkDestroyShaderModule(m_ctx.device(), p.fs, nullptr);
    }

    for (auto& t : m_textures) destroyTextureNow(t);

    m_vertexBuffers.clear();
    m_indexBuffers.clear();
    m_uniformBuffers.clear();
    m_programs.clear();
    m_textures.clear();

    m_frames.shutdown();
    m_swapchain.shutdown();
    m_ctx.shutdown();
}



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

bool VulkanRenderer::init() {
    if (!m_owner->m_cfg.window) {
        std::fprintf(stderr, "[vulkan] RenderConfig::window is null\n");
        return false;
    }

    if (!initDevice(*m_owner->m_cfg.window, m_owner->m_cfg.framesInFlight)) return false;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_ctx.physicalDevice(), &props);

    m_pushConstantSize = std::min(
        uint32_t(RenderCommand::MAX_UNIFORM_BYTES),
        props.limits.maxPushConstantsSize
    );

    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = m_pushConstantSize
    };

    VkDescriptorSetLayoutBinding bindings[MAX_UNIFORM_SLOTS]{};
    for (uint32_t i = 0; i < MAX_UNIFORM_SLOTS; ++i) {
        bindings[i] = {
            .binding            = i,
            .descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount    = 1,
            .stageFlags         = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        };
    }

    const VkDescriptorSetLayoutCreateInfo dsl{
        .sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags          = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount   = MAX_UNIFORM_SLOTS,
        .pBindings      = bindings
    };

    if (vkCreateDescriptorSetLayout(m_ctx.device(), &dsl, nullptr, &m_uniformSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateDescriptorSetLayout failed\n");
        return false;
    }

    m_pushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR)
        vkGetDeviceProcAddr(m_ctx.device(), "vkCmdPushDescriptorSetKHR");

    if (!m_pushDescriptorSet) {
        std::fprintf(stderr, "[vulkan] vkCmdPushDescriptorSetKHR not available\n");
        return false;
    }

    // before the pipeline layout, which names both sets
    if (!createBindlessSet()) return false;

    const VkDescriptorSetLayout setLayouts[2]{ m_uniformSetLayout, m_bindlessLayout };

    const VkPipelineLayoutCreateInfo li{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 2,
        .pSetLayouts            = setLayouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range
    };

    if (vkCreatePipelineLayout(m_ctx.device(), &li, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreatePipelineLayout failed\n");
        return false;
    }

    if (!createSamplers()) return false;

    m_depthFormat = pickDepthFormat();
    if (m_depthFormat == VK_FORMAT_UNDEFINED) return false;

    return createDepthBuffers();
}


/* ========================================================================= //
    Frame
// ========================================================================= */

void VulkanRenderer::clear(const Color& clearColor, const RenderTargetID target, const float clearDepth) {
    (void)target;       // render targets are not implemented yet

    beginFrameIfNeeded(VK_ATTACHMENT_LOAD_OP_CLEAR, clearColor, clearDepth);
}


void VulkanRenderer::draw(const RenderCommand& cmd, const RenderTargetID target) {
    (void)target;

    if (!beginFrameIfNeeded(VK_ATTACHMENT_LOAD_OP_LOAD, {})) return;

    static const VtxLayout noVerts;

    const bool hasVerts = 
        cmd.verts != EMPTY_BUFFER &&
        cmd.verts < m_vertexBuffers.size() &&
        m_vertexBuffers[cmd.verts].buffer;

    const VtxLayout& layout = 
        hasVerts ? m_vertexBuffers[cmd.verts].layout : noVerts;

    const VkPipeline pipeline = pipelineFor(cmd, layout);
    if (!pipeline) return;

    const VkCommandBuffer buf = m_frames.commandBuffer(m_frameIndex);
    vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    if (cmd.uniformSize) {
        if (cmd.uniformSize > m_pushConstantSize) {
            static bool warned = false;
            if (!warned) {
                std::fprintf(
                    stderr,
                    "[vulkan] uniforms are %zu bytes, this device allows %u - truncating\n",
                    cmd.uniformSize, m_pushConstantSize
                );
                warned = true;
            }
        }

        vkCmdPushConstants(
            buf, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, std::min(uint32_t(cmd.uniformSize), m_pushConstantSize),
            cmd.uniforms
        );
    }

    VkDescriptorBufferInfo infos[MAX_UNIFORM_SLOTS]{};
    VkWriteDescriptorSet writes[MAX_UNIFORM_SLOTS]{};
    uint32_t writeCount = 0;

    for (uint32_t slot = 0; slot < MAX_UNIFORM_SLOTS; ++slot) {
        const UniformBufferID id = cmd.uniformBuffers[slot];
        if (id == EMPTY_BUFFER || id >= m_uniformBuffers.size()) continue;
        if (!m_uniformBuffers[id].buffer) continue;

        infos[writeCount] = {
            .buffer = m_uniformBuffers[id].buffer,
            .offset = 0,
            .range  = VK_WHOLE_SIZE
        };

        writes[writeCount] = {
            .sType              = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding         = slot,
            .descriptorCount    = 1,
            .descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo        = &infos[writeCount]
        };
        ++writeCount;
    }

    if (writeCount)
        m_pushDescriptorSet(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, writeCount, writes);

    // set 1 holds every texture and every sampler, so it is bound once per draw
    // and never rebuilt - the TextureID in the uniform blob is the array index
    if (m_bindlessSet) {
        vkCmdBindDescriptorSets(
            buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelineLayout, 1, 1, &m_bindlessSet, 0, nullptr
        );
    }

    const VkExtent2D extent = m_swapchain.extent();

    const float vpX = cmd.viewport.width  > 0.f ? cmd.viewport.x : 0.f;
    const float vpY = cmd.viewport.height > 0.f ? cmd.viewport.y : 0.f;
    const float vpW = cmd.viewport.width  > 0.f ? cmd.viewport.width  : float(extent.width);
    const float vpH = cmd.viewport.height > 0.f ? cmd.viewport.height : float(extent.height);

    /*
        Negative height, so the viewport transform flips Y. VALO's clip space is
        Y-up: Mat4f::perspective does not negate Y, and the software backend
        flips at DrawFuncs.cpp:79 with (1 - ndc.y). Vulkan's is natively Y-down
        and would not flip, which renders mirrored AND reverses the sign of the
        signed area - so the same mesh culls the opposite way on each backend.

        Flipping here rather than in the projection keeps one matrix meaning one
        thing on both backends. VK_KHR_maintenance1, core since 1.1.
    */
    const VkViewport viewport{
        .x          = vpX,
        .y          = vpY + vpH,
        .width      = vpW,
        .height     = -vpH,
        .minDepth   = cmd.viewport.minDepth,
        .maxDepth   = cmd.viewport.maxDepth
    };
    vkCmdSetViewport(buf, 0, 1, &viewport);

    const VkRect2D scissor{
        .offset = {
            cmd.scissor.width ? cmd.scissor.x : 0,
            cmd.scissor.height ? cmd.scissor.y : 0
        },
        .extent = {
            cmd.scissor.width ? cmd.scissor.width : extent.width,
            cmd.scissor.height ? cmd.scissor.height : extent.height
        }
    };
    vkCmdSetScissor(buf, 0, 1, &scissor);

    if (hasVerts) {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(buf, 0, 1, &m_vertexBuffers[cmd.verts].buffer, &offset);
    }

    const bool hasIndices = 
        cmd.indices != EMPTY_BUFFER &&
        cmd.indices < m_indexBuffers.size() &&
        m_indexBuffers[cmd.indices].buffer;

    if (hasIndices) {
        const GPUBuffer& ib = m_indexBuffers[cmd.indices];
        const uint32_t total = uint32_t(ib.size / sizeof(uint32_t));

        const uint32_t count = 
            cmd.indexCount == ALL_INDICES ?
            (cmd.firstIndex < total ? total - cmd.firstIndex : 0) :
            cmd.indexCount;

        if (!count) return;

        vkCmdBindIndexBuffer(buf, ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(buf, count, cmd.instanceCount, cmd.firstIndex, 0, 0);
        return;
    }

    const uint32_t count =
        cmd.indexCount != ALL_INDICES ?
        cmd.indexCount : hasVerts ?
        uint32_t(m_vertexBuffers[cmd.verts].size / layout.stride()) :
        0;

    if (count) vkCmdDraw(buf, count, cmd.instanceCount, cmd.firstIndex, 0);
}


void VulkanRenderer::show(const RenderTargetID target) {
    (void)target;       // render targets are not implemented yet

    if (!m_frameOpen) return;

    drainRetiredBuffers();

    const uint32_t frame = m_frameIndex;
    const VkCommandBuffer cmd = m_frames.commandBuffer(frame);

    if (m_renderingOpen) {
        vkCmdEndRendering(cmd);
        m_renderingOpen = false;
    }

    const VkImageMemoryBarrier2 toPresent{
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask           = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask          = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask           = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask          = 0,
        .oldLayout              = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout              = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .image                  = m_swapchain.images()[m_imageIndex],
        .subresourceRange       = {
            .aspectMask         = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel       = 0,
            .levelCount         = 1,
            .baseArrayLayer     = 0,
            .layerCount         = 1
        }
    };

    const VkDependencyInfo depOut{
        .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount    = 1,
        .pImageMemoryBarriers       = &toPresent
    };
    vkCmdPipelineBarrier2(cmd, &depOut);

    vkEndCommandBuffer(cmd);

    const VkSemaphoreSubmitInfo waitSem{
        .sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore              = m_frames.imageAvailable(frame),
        .stageMask              = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
    };

    const VkSemaphoreSubmitInfo signalSem{
        .sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore              = m_frames.renderFinished(m_imageIndex),
        .stageMask              = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT
    };

    const VkCommandBufferSubmitInfo cmdInfo{
        .sType                  = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer          = cmd
    };

    const VkSubmitInfo2 submit{
        .sType                      = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount     = 1,
        .pWaitSemaphoreInfos        = &waitSem,
        .commandBufferInfoCount     = 1,
        .pCommandBufferInfos        = &cmdInfo,
        .signalSemaphoreInfoCount   = 1,
        .pSignalSemaphoreInfos      = &signalSem
    };

    m_frameOpen = false;

    if (vkQueueSubmit2(m_ctx.graphicsQueue(), 1, &submit, m_frames.inFlight(frame)) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkQueueSubmit2 failed\n");
        return;
    }

    const VkSemaphore presentWait = m_frames.renderFinished(m_imageIndex);
    const VkSwapchainKHR chain = m_swapchain.handle();

    const VkPresentInfoKHR pi{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &presentWait,
        .swapchainCount     = 1,
        .pSwapchains        = &chain,
        .pImageIndices      = &m_imageIndex 
    };

    const VkResult r = vkQueuePresentKHR(m_ctx.presentQueue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;
    else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkQueuePresentKHR failed (%d)\n", r);
        return;
    }

    m_frameIndex = (frame + 1) % m_frames.framesInFlight();
}


void VulkanRenderer::flush() { /* no flush for vulkan */}


/* ========================================================================= //
    Render targets
// ========================================================================= */

RenderTargetID VulkanRenderer::createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb) {
    (void)w; (void)h; (void)tracksWindow; (void)srgb;
    todo(__func__);
    return EMPTY_TEXTURE;
}


/* ========================================================================= //
    Vertex buffers
// ========================================================================= */

VertexBufferID VulkanRenderer::createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout) {
    GPUBuffer buffer;
    buffer.layout = layout;

    if (!uploadBuffer(data, count * layout.stride(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, buffer))
        return EMPTY_BUFFER;

    return m_owner->allocSlot(m_vertexBuffers, m_owner->m_freeVertexBuffers, std::move(buffer));
}


void VulkanRenderer::updateVertexBuffer(
    const VertexBufferID vertices, 
    const void* data, 
    const size_t count, 
    const VtxLayout& newLayout
) {
    if (vertices >= m_vertexBuffers.size()) {
        std::fprintf(stderr, "[vulkan] updateVertexBuffer(): ID out of range\n");
        return;
    }

    GPUBuffer& vb = m_vertexBuffers[vertices];
    const VtxLayout layout = newLayout.stride() == 0 ? vb.layout : newLayout;

    GPUBuffer fresh;
    fresh.layout = layout;

    if (!uploadBuffer(data, count * layout.stride(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, fresh))
        return;

    retireBuffer(vb);
    vb = fresh;
}


/* ========================================================================= //
    Index buffers
// ========================================================================= */

IndexBufferID VulkanRenderer::createIndexBuffer(const uint32_t* data, const size_t count) {
    GPUBuffer buffer;

    if (!uploadBuffer(data, count * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, buffer))
        return EMPTY_BUFFER;

    return m_owner->allocSlot(m_indexBuffers, m_owner->m_freeIndexBuffers, std::move(buffer));
}


void VulkanRenderer::updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    if (indices >= m_indexBuffers.size()) {
        std::fprintf(stderr, "[vulkan] updateIndexBuffer(): ID out of range\n");
        return;
    }

    GPUBuffer fresh;
    if (!uploadBuffer(data, count * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, fresh))
        return;

    retireBuffer(m_indexBuffers[indices]);
    m_indexBuffers[indices] = fresh;
}


/* ========================================================================= //
    Uniform buffers
// ========================================================================= */

UniformBufferID VulkanRenderer::createUniformBuffer(const void* data, const size_t bytes) {
    GPUBuffer buffer;
    if (!uploadBuffer(data, bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, buffer))
        return EMPTY_BUFFER;

    return m_owner->allocSlot(m_uniformBuffers, m_owner->m_freeUniformBuffers, std::move(buffer));
}


void VulkanRenderer::updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes) {
    if (buffer >= m_uniformBuffers.size()) {
        std::fprintf(stderr, "[vulkan] updateUniformBuffer(): ID out of range\n");
        return;
    }

    GPUBuffer fresh;
    if (!uploadBuffer(data, bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, fresh)) return;

    retireBuffer(m_uniformBuffers[buffer]);
    m_uniformBuffers[buffer] = fresh;
}


/* ========================================================================= //
    Textures
// ========================================================================= */

TextureID VulkanRenderer::createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips) {
    if (!w || !h || !data) return EMPTY_TEXTURE;
    if (m_textures.size() >= MAX_TEXTURES) {
        std::fprintf(stderr, "[vulkan] texture limit (%u) reached\n", MAX_TEXTURES);
        return EMPTY_TEXTURE;
    }

    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    uint32_t levels = 1;
    if (mips) while ((w >> levels) > 0 || (h >> levels) > 0) ++levels;

    GPUTexture tex;
    const VkImageUsageFlags usage = 
        VK_IMAGE_USAGE_SAMPLED_BIT      |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (!createImage(w, h, format, levels, usage, tex)) return EMPTY_TEXTURE;

    if (!uploadImage(tex, data)) {
        destroyTextureNow(tex);
        return EMPTY_TEXTURE;
    }

    const TextureID id = m_owner->allocSlot(m_textures, m_owner->m_freeTargets, std::move(tex));
    writeTextureDescriptor(id);
    return id;
}


TextureID VulkanRenderer::createTextureFromFile(const std::string& path, const bool srgb, const bool mips) {
    int w = 0, h = 0, channels = 0;

    uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);

    if (!pixels) {
        std::fprintf(stderr, "[vulkan] %s: %s\n", path.c_str(), stbi_failure_reason());
        return EMPTY_TEXTURE;
    }

    const TextureID id = createTexture(
        uint32_t(w), uint32_t(h),
        reinterpret_cast<const uint32_t*>(pixels),
        srgb, mips
    );

    stbi_image_free(pixels);
    return id;
}


void VulkanRenderer::updateTexture(const TextureID texture, const uint32_t* data) {
    (void)texture; (void)data;
    todo(__func__);
}


void VulkanRenderer::updateTextureRect(
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

ProgramID VulkanRenderer::createProgramFromCallbacks(const Program& program) {
    (void)program;

    std::fprintf(
        stderr,
        "[vulkan] createProgram(vs, fs) takes C++ callbacks, which the Vulkan "
        "backend cannot use - pass shader paths instead\n"
    );

    return DEFAULT_PROGRAM;
}


ProgramID VulkanRenderer::createProgramFromFiles(const std::string& vsPath, const std::string& fsPath) {
    const std::vector<uint32_t> vsCode = loadSpirv(vsPath);
    const std::vector<uint32_t> fsCode = loadSpirv(fsPath);
    if (vsCode.empty() || fsCode.empty()) return DEFAULT_PROGRAM;

    GPUProgram prog;
    prog.vs = createShaderModule(vsCode);
    prog.fs = createShaderModule(fsCode);

    if (!prog.vs || !prog.fs) {
        if (prog.vs) vkDestroyShaderModule(m_ctx.device(), prog.vs, nullptr);
        if (prog.fs) vkDestroyShaderModule(m_ctx.device(), prog.fs, nullptr);
        return DEFAULT_PROGRAM;
    }

    return m_owner->allocSlot(m_programs, m_owner->m_freePrograms, std::move(prog));
}


/* ========================================================================= //
    Releases
// ========================================================================= */

void VulkanRenderer::releaseTarget(const RenderTargetID id)         { (void)id; todo(__func__); }


void VulkanRenderer::releaseVertexBuffer(const VertexBufferID id) { 
    vmaDestroyBuffer(m_ctx.allocator(), m_vertexBuffers[id].buffer, m_vertexBuffers[id].allocation);
    m_vertexBuffers[id] = GPUBuffer{};
}


void VulkanRenderer::releaseIndexBuffer(const IndexBufferID id) {
    vmaDestroyBuffer(m_ctx.allocator(), m_indexBuffers[id].buffer, m_indexBuffers[id].allocation);
    m_indexBuffers[id] = GPUBuffer{};
}


void VulkanRenderer::releaseUniformBuffer(const UniformBufferID id) { 
    if (id < m_uniformBuffers.size()) retireBuffer(m_uniformBuffers[id]); 
}

void VulkanRenderer::releaseProgram(const ProgramID id) { 
    if (m_programs[id].vs) vkDestroyShaderModule(m_ctx.device(), m_programs[id].vs, nullptr);
    if (m_programs[id].fs) vkDestroyShaderModule(m_ctx.device(), m_programs[id].fs, nullptr);
    m_programs[id] = GPUProgram{};
}


/* ========================================================================= //
    State
// ========================================================================= */

void VulkanRenderer::resize(const uint32_t w, const uint32_t h) {
    (void)w; (void)h;
    m_needsRecreate = true;
}


void VulkanRenderer::setVsync(const bool vsync) {
    if (m_owner->m_cfg.vsync == vsync) return;
    
    m_owner->m_cfg.vsync = vsync;
    m_needsRecreate = true;
}


/* ========================================================================= //
    Queries
// ========================================================================= */

uint32_t VulkanRenderer::width() const {
    return m_swapchain.extent().width;
}


uint32_t VulkanRenderer::height() const {
    return m_swapchain.extent().height;
}


} // namespace valo

#endif // VALO_HAS_VULKAN