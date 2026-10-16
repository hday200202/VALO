#include "VulkanRenderer.hpp"
#include "../Renderer.hpp"
#include "../SpirvRegistry.hpp"

#ifdef VALO_HAS_VULKAN

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <stb_image.h>

namespace valo {


bool VulkanRenderer::initDevice(Window& window, const uint32_t framesInFlight, const bool validation) {
    m_window = &window;

    if (!m_ctx.init(window, validation)) return false;
    if (!m_swapchain.init(m_ctx, window.getSize(), m_owner->m_cfg.vsync, m_owner->m_cfg.srgbBackbuffer)) return false;
    if (!m_frames.init(m_ctx, framesInFlight, m_swapchain.imageCount())) return false;

    return true;
}


bool VulkanRenderer::beginFrameIfNeeded() {
    if (m_frameOpen) return true;

    const VkDevice device = m_ctx.device();
    const uint32_t frame = m_frameIndex;

    if (m_needsRecreate) {
        if (!recreateSwapchain()) return false;
        if (m_needsRecreate) return false;
    }

    if (!applyRenderScale()) return false;

    if (!waitForSlot()) return false;
    const VkFence fence = m_frames.inFlight(frame);

    VkResult r = vkAcquireNextImageKHR(
        device, m_swapchain.handle(), UINT64_MAX,
        m_frames.imageAvailable(frame), VK_NULL_HANDLE, &m_imageIndex
    );

    // an out-of-date acquire signals nothing, so it can be retried on the new swapchain
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m_needsRecreate = true;
        if (!recreateSwapchain() || m_needsRecreate) return false;

        r = vkAcquireNextImageKHR(
            device, m_swapchain.handle(), UINT64_MAX,
            m_frames.imageAvailable(frame), VK_NULL_HANDLE, &m_imageIndex
        );
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { m_needsRecreate = true; return false; }
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

    m_frameOpen = true;
    return true;
}


bool VulkanRenderer::bindTarget(
    const RenderTargetID requested,
    const VkAttachmentLoadOp loadOp,
    const Color& clearColor,
    const float clearDepth
) {
    if (!beginFrameIfNeeded()) return false;

    const RenderTargetID target = resolveTarget(requested);

    if (m_renderingOpen && m_currentTarget == target && loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
        return true;

    endRenderingIfOpen();

    const uint32_t frame = m_frameIndex;
    const VkCommandBuffer cmd = m_frames.commandBuffer(frame);

    if (m_swapLayouts.size() != m_swapchain.imageCount())
        m_swapLayouts.assign(m_swapchain.imageCount(), VK_IMAGE_LAYOUT_UNDEFINED);

    const bool toBackbuffer = (target == 0);

    GPUTexture* tex = nullptr;
    if (!toBackbuffer) {
        if (target >= m_textures.size() || !m_textures[target].isTarget) return false;
        tex = &m_textures[target];
    }

    const VkImage     colorImage = toBackbuffer ? m_swapchain.images()[m_imageIndex] : tex->image;
    const VkImageView colorView  = toBackbuffer ? m_swapchain.views()[m_imageIndex]  : tex->view;
    const VkExtent2D  extent     = toBackbuffer ? m_swapchain.extent()
                                                : VkExtent2D{ tex->width, tex->height };
    const VkFormat colorFormat = toBackbuffer ? m_swapchain.format() : tex->format;
    DepthBuffer& db = toBackbuffer ? m_depth[frame] : tex->depth;
    const bool depthFresh = (db.layout == VK_IMAGE_LAYOUT_UNDEFINED);

    const VkImageMemoryBarrier2 toAttachment{
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask           = toBackbuffer ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                      : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask          = toBackbuffer ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask           = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask          = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
        // CLEAR overwrites the image, so UNDEFINED (which discards) is fine and
        // cheaper; LOAD has to name the layout the image is actually in
        .oldLayout              = !toBackbuffer           ? tex->layout
                                : loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR
                                                          ? VK_IMAGE_LAYOUT_UNDEFINED
                                                          : m_swapLayouts[m_imageIndex],
        .newLayout              = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .image                  = colorImage,
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
        .srcStageMask           = depthFresh ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                             : VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                             | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask          = depthFresh ? 0
                                             : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask           = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask          = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .oldLayout              = db.layout,
        .newLayout              = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .image                  = db.image,
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
        .imageView      = colorView,
        .imageLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp         = loadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue     = { .color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}} }
    };

    const VkRenderingAttachmentInfo depth{
        .sType          = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView      = db.view,
        .imageLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp         = loadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue     = { .depthStencil = { .depth = clearDepth, .stencil = 0 } }
    };

    const VkRenderingAttachmentInfo stencil{
        .sType          = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView      = db.view,
        .imageLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp         = loadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue     = { .depthStencil = { .depth = clearDepth, .stencil = 0 } }
    };

    const VkRenderingInfo ri{
        .sType                  = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea             = { {0, 0}, extent },
        .layerCount             = 1,
        .colorAttachmentCount   = 1,
        .pColorAttachments      = &color,
        .pDepthAttachment       = &depth,
        .pStencilAttachment     = &stencil
    };

    vkCmdBeginRendering(cmd, &ri);

    if (toBackbuffer) m_swapLayouts[m_imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    m_currentTarget = target;
    m_currentColorFormat = colorFormat;
    m_currentExtent = extent;
    db.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (tex) tex->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    m_renderingOpen = true;
    return true;
}


void VulkanRenderer::endRenderingIfOpen() {
    if (!m_renderingOpen) return;

    const VkCommandBuffer cmd = m_frames.commandBuffer(m_frameIndex);
    vkCmdEndRendering(cmd);
    m_renderingOpen = false;

    if (m_currentTarget == 0 || m_currentTarget >= m_textures.size()) return;

    GPUTexture& tex = m_textures[m_currentTarget];
    if (!tex.isTarget || tex.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return;

    const VkImageMemoryBarrier2 toRead{
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask           = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask          = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask           = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask          = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout              = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout              = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
        .image                  = tex.image,
        .subresourceRange       = {
            .aspectMask         = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel       = 0,
            .levelCount         = 1,
            .baseArrayLayer     = 0,
            .layerCount         = 1
        }
    };

    const VkDependencyInfo depIn{
        .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount    = 1,
        .pImageMemoryBarriers       = &toRead
    };
    vkCmdPipelineBarrier2(cmd, &depIn);

    tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}


bool VulkanRenderer::uploadBuffer(
    const void* data,
    const VkDeviceSize bytes,
    const VkBufferUsageFlags usage,
    GPUBuffer& out
) {
    out.size = bytes;
    if (bytes == 0) return true;

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
    vmaFlushAllocation(alloc, stagingAlloc, 0, bytes);

    const VkCommandBuffer cmd = m_ctx.beginOneShot();
    if (!cmd) { vmaDestroyBuffer(alloc, staging, stagingAlloc); return false; }

    recordImageUpload(cmd, tex, staging, 0, 0, 0, tex.width, tex.height, VK_IMAGE_LAYOUT_UNDEFINED);

    const bool ok = m_ctx.endOneShot(cmd);
    vmaDestroyBuffer(alloc, staging, stagingAlloc);
    if (!ok) tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ok;
}


void VulkanRenderer::recordImageUpload(
    const VkCommandBuffer cmd,
    GPUTexture& tex,
    const VkBuffer src, const VkDeviceSize offset,
    const uint32_t x, const uint32_t y,
    const uint32_t w, const uint32_t h,
    const VkImageLayout from
) {
    transitionImage(cmd, tex.image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, tex.mipLevels);

    const VkBufferImageCopy region{
        .bufferOffset       = offset,
        .imageSubresource   = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset        = { int32_t(x), int32_t(y), 0 },
        .imageExtent        = { w, h, 1 }
    };
    vkCmdCopyBufferToImage(cmd, src, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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

    tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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


void VulkanRenderer::clearTextureDescriptor(const TextureID id) {
    if (id >= m_maxTextures || !m_nullTexture.view) return;

    const VkDescriptorImageInfo info{
        .imageView      = m_nullTexture.view,
        .imageLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
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
    vkUpdateDescriptorSets(m_ctx.device(), 1, &write, 0, nullptr);
}


// immediate, not deferred - only safe once the GPU is known to be idle, which
// is why the callers are the create failure path and shutdown
void VulkanRenderer::destroyTextureNow(GPUTexture& tex) {
    if (tex.depth.view)  vkDestroyImageView(m_ctx.device(), tex.depth.view, nullptr);
    if (tex.depth.image) vmaDestroyImage(m_ctx.allocator(), tex.depth.image, tex.depth.allocation);
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
    if (!m_swapchain.init(m_ctx, m_window->getSize(), m_owner->m_cfg.vsync, m_owner->m_cfg.srgbBackbuffer)) {
        std::fprintf(stderr, "[vulkan] swapchain recreate failed\n");
        return false;
    }

    // renderFinished is one semaphore per swapchain image, so a driver that
    // varies the count needs VulkanFrames rebuilt rather than an error
    if (m_swapchain.imageCount() != oldImageCount) {
        flushUploads();
        m_frames.shutdown();

        if (!m_frames.init(m_ctx, m_owner->m_cfg.framesInFlight, m_swapchain.imageCount())) {
            std::fprintf(stderr, "[vulkan] VulkanFrames rebuild failed after an image-count change\n");
            return false;
        }

        m_frameIndex = 0;
        m_slotReady  = false;
    }

    m_swapLayouts.assign(m_swapchain.imageCount(), VK_IMAGE_LAYOUT_UNDEFINED);

    if (!createDepthBuffers()) return false;
    if (!resizeTrackingTargets()) return false;

    m_needsRecreate = false;
    return true;
}


void VulkanRenderer::retireBuffer(GPUBuffer& buf) {
    if (!buf.buffer && buf.slots.empty()) { buf = GPUBuffer{}; return; }

    m_retiredBuffers.push_back({buf, m_owner->m_frame});
    buf = GPUBuffer{};
}


void VulkanRenderer::drainRetiredBuffers(const bool force) {
    size_t keep = 0;
    for (size_t i = 0; i < m_retiredBuffers.size(); ++i) {
        auto& [buf, frame] = m_retiredBuffers[i];

        if (force || m_owner->m_frame - frame >= m_owner->m_cfg.framesInFlight) {
            destroyBufferNow(buf);
        } else {
            if (keep != i) m_retiredBuffers[keep] = std::move(m_retiredBuffers[i]);
            ++keep;
        }
    }

    m_retiredBuffers.resize(keep);
}


void VulkanRenderer::destroyBufferNow(GPUBuffer& buf) {
    const VmaAllocator alloc = m_ctx.allocator();

    if (buf.buffer) vmaDestroyBuffer(alloc, buf.buffer, buf.allocation);
    for (BufferSlot& slot : buf.slots)
        if (slot.buffer) vmaDestroyBuffer(alloc, slot.buffer, slot.allocation);

    buf = GPUBuffer{};
}



/* ---- per-frame updates -------------------------------------------------- */

// idempotent per slot: the fence is only reset once beginFrameIfNeeded has acquired
bool VulkanRenderer::waitForSlot() {
    if (m_slotReady) return true;

    const VkFence fence = m_frames.inFlight(m_frameIndex);
    if (vkWaitForFences(m_ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

    m_slotReady = true;
    return true;
}


bool VulkanRenderer::allocSlotBuffer(BufferSlot& slot, const VkDeviceSize bytes, const VkBufferUsageFlags usage) {
    const VkBufferCreateInfo ci{
        .sType          = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size           = bytes,
        .usage          = usage,
        .sharingMode    = VK_SHARING_MODE_EXCLUSIVE
    };

    const VmaAllocationCreateInfo ai{
        .flags  = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage  = VMA_MEMORY_USAGE_AUTO
    };

    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_ctx.allocator(), &ci, &ai, &slot.buffer, &slot.allocation, &info) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] dynamic buffer allocation failed\n");
        slot = BufferSlot{};
        return false;
    }

    slot.mapped   = info.pMappedData;
    slot.capacity = bytes;
    return true;
}


// a copy recorded earlier this frame may still read it, so it goes through the retire queue
void VulkanRenderer::retireSlotBuffer(BufferSlot& slot) {
    if (!slot.buffer) return;

    GPUBuffer old;
    old.buffer     = slot.buffer;
    old.allocation = slot.allocation;
    retireBuffer(old);

    slot = BufferSlot{};
}


bool VulkanRenderer::writeSlot(GPUBuffer& buf, BufferSlot& slot) {
    const VkDeviceSize bytes = buf.latest.size();

    if (bytes > slot.capacity) {
        const VkDeviceSize grown = std::max(bytes, slot.capacity * 2);
        retireSlotBuffer(slot);
        if (!allocSlotBuffer(slot, grown, buf.usage)) return false;
    }

    if (bytes) {
        std::memcpy(slot.mapped, buf.latest.data(), size_t(bytes));
        vmaFlushAllocation(m_ctx.allocator(), slot.allocation, 0, bytes);
    }

    slot.size    = bytes;
    slot.version = buf.version;
    return true;
}


bool VulkanRenderer::updateDynamic(
    GPUBuffer& buf,
    const void* header, const size_t headerBytes,
    const void* data, const size_t bytes
) {
    if (!waitForSlot()) return false;

    if (buf.slots.empty()) {
        GPUBuffer old;
        old.buffer     = buf.buffer;
        old.allocation = buf.allocation;
        retireBuffer(old);

        buf.buffer     = VK_NULL_HANDLE;
        buf.allocation = VK_NULL_HANDLE;
        buf.slots.resize(m_frames.framesInFlight());
    }

    buf.latest.resize(headerBytes + bytes);
    if (headerBytes)   std::memcpy(buf.latest.data(), header, headerBytes);
    if (bytes && data) std::memcpy(buf.latest.data() + headerBytes, data, bytes);

    buf.size = buf.latest.size();
    ++buf.version;

    return writeSlot(buf, buf.slots[m_frameIndex]);
}


// a slot that missed the last update is caught up from the CPU copy before it is bound
bool VulkanRenderer::currentBuffer(GPUBuffer& buf, VkBuffer& out, VkDeviceSize& bytes) {
    if (!buf.live) return false;

    if (buf.slots.empty()) {
        out   = buf.buffer;
        bytes = buf.size;
        return true;
    }

    BufferSlot& slot = buf.slots[m_frameIndex];
    if (slot.version != buf.version && !writeSlot(buf, slot)) return false;

    out   = slot.buffer;
    bytes = slot.size;
    return true;
}


bool VulkanRenderer::createUploadSlots() {
    const VkCommandPoolCreateInfo pi{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags              = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex   = m_ctx.graphicsFamily()
    };

    if (vkCreateCommandPool(m_ctx.device(), &pi, nullptr, &m_uploadPool) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] upload command pool creation failed\n");
        return false;
    }

    m_uploads.resize(m_frames.framesInFlight());

    for (UploadSlot& u : m_uploads) {
        const VkCommandBufferAllocateInfo ai{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = m_uploadPool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1
        };

        if (vkAllocateCommandBuffers(m_ctx.device(), &ai, &u.cmd) != VK_SUCCESS) return false;
    }

    return true;
}


void VulkanRenderer::destroyUploadSlots() {
    const VmaAllocator alloc = m_ctx.allocator();

    for (UploadSlot& u : m_uploads)
        if (u.staging.buffer) vmaDestroyBuffer(alloc, u.staging.buffer, u.staging.allocation);
    m_uploads.clear();

    if (m_uploadPool) {
        vkDestroyCommandPool(m_ctx.device(), m_uploadPool, nullptr);
        m_uploadPool = VK_NULL_HANDLE;
    }
}


// for uploads recorded against a slot that will not be shown again
void VulkanRenderer::flushUploads() {
    for (UploadSlot& u : m_uploads) {
        if (!u.open) continue;

        vkEndCommandBuffer(u.cmd);
        u.open = false;

        const VkCommandBufferSubmitInfo cmdInfo{
            .sType          = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer  = u.cmd
        };

        const VkSubmitInfo2 submit{
            .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos    = &cmdInfo
        };

        vkQueueSubmit2(m_ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    }

    vkQueueWaitIdle(m_ctx.graphicsQueue());
}


// no frame to ride on: the uploads go alone, fenced by the slot so the next update waits for them
void VulkanRenderer::submitPendingUploads() {
    if (m_uploads.empty()) return;

    UploadSlot& u = m_uploads[m_frameIndex];
    if (!u.open) return;

    vkEndCommandBuffer(u.cmd);
    u.open = false;

    const VkFence fence = m_frames.inFlight(m_frameIndex);
    vkResetFences(m_ctx.device(), 1, &fence);

    const VkCommandBufferSubmitInfo cmdInfo{
        .sType          = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer  = u.cmd
    };

    const VkSubmitInfo2 submit{
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos    = &cmdInfo
    };

    vkQueueSubmit2(m_ctx.graphicsQueue(), 1, &submit, fence);
    m_slotReady = false;
}


VkCommandBuffer VulkanRenderer::uploadCommands() {
    if (!waitForSlot()) return VK_NULL_HANDLE;

    UploadSlot& u = m_uploads[m_frameIndex];
    if (u.open) return u.cmd;

    vkResetCommandBuffer(u.cmd, 0);

    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (vkBeginCommandBuffer(u.cmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;

    u.open = true;
    u.used = 0;
    return u.cmd;
}


bool VulkanRenderer::stageUpload(const void* data, const VkDeviceSize bytes, VkBuffer& src, VkDeviceSize& offset) {
    UploadSlot& u = m_uploads[m_frameIndex];

    if (u.used + bytes > u.staging.capacity) {
        const VkDeviceSize grown = std::max(u.used + bytes, u.staging.capacity * 2);
        retireSlotBuffer(u.staging);
        if (!allocSlotBuffer(u.staging, grown, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
        u.used = 0;
    }

    std::memcpy(static_cast<uint8_t*>(u.staging.mapped) + u.used, data, size_t(bytes));
    vmaFlushAllocation(m_ctx.allocator(), u.staging.allocation, u.used, bytes);

    src    = u.staging.buffer;
    offset = u.used;
    u.used = (u.used + bytes + 15) & ~VkDeviceSize(15);
    return true;
}


namespace {

#ifndef VALO_SOURCE_DIR
#define VALO_SOURCE_DIR "."
#endif

#ifndef VALO_GLSLC_PATH
#define VALO_GLSLC_PATH "glslc"
#endif

const std::string SOURCE_DIR = VALO_SOURCE_DIR;

#ifdef _WIN32
const std::string VALOC_PATH  = SOURCE_DIR + "/bin/valoc.exe";
const char* const NULL_OUTPUT = " > NUL";
#else
const std::string VALOC_PATH  = SOURCE_DIR + "/bin/valoc";
const char* const NULL_OUTPUT = " > /dev/null";
#endif

// cmd.exe strips the first and last quote of a line that starts with one, so
// the whole line gets a pair of its own and the quoted executable survives
std::string shellLine(const std::string& line) {
#ifdef _WIN32
    return "\"" + line + "\"";
#else
    return line;
#endif
}

/*
    Whether the GLSL beside a C++ shader has to be (re)translated.

    Missing: yes. Hand-written - no valoc marker - never, so an override is
    never clobbered. Generated: when anything it was translated from is newer
    than it, or valoc itself is, since a translator fix changes the output
    without any shader changing. valoc records its inputs as `// valoc-dep:`
    lines, which covers headers a shader only borrows a struct from; a file
    translated before it recorded them is treated as stale.
*/
bool needsTranslation(const std::string& glslPath) {
    namespace fs = std::filesystem;

    if (!fs::exists(glslPath)) return true;

    std::ifstream in(glslPath);
    std::string line;
    bool generated = false;
    std::vector<std::string> deps;

    for (int n = 0; n < 256 && std::getline(in, line); ++n) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("// generated by valoc", 0) == 0)  generated = true;
        else if (line.rfind("// valoc-dep: ", 0) == 0)    deps.push_back(line.substr(14));
        else if (generated && !line.empty() && line.rfind("//", 0) != 0) break;
    }

    if (!generated)   return false;
    if (deps.empty()) return true;

    std::error_code ec;
    const auto built = fs::last_write_time(glslPath, ec);
    if (ec) return true;

    const auto translator = fs::last_write_time(VALOC_PATH, ec);
    if (!ec && translator > built) return true;

    const fs::path dir = fs::path(glslPath).parent_path();
    for (const std::string& d : deps) {
        const auto t = fs::last_write_time(dir / d, ec);
        if (ec || t > built) return true;       // moved, deleted, or edited
    }

    return false;
}

} // namespace


std::vector<uint32_t> VulkanRenderer::loadSpirv(const std::string& path) {
    namespace fs = std::filesystem;

    if (const SpirvEntry* embedded = findSpirv(path))
        return std::vector<uint32_t>(embedded->words, embedded->words + embedded->count);

    std::string source = path;

    /*
        A C++ shader file: strip the .hpp to get the GLSL beside it, translating
        it first when it is missing or stale. If translation fails the old file
        is not used - running yesterday's shader quietly is worse than failing.
    */
    if (fs::path(path).extension() == ".hpp") {
        source = path.substr(0, path.size() - 4);

        if (needsTranslation(source)) {
            // stdout is valoc's struct summary; its errors arrive on stderr
            const std::string cmd = shellLine(
                "\"" + VALOC_PATH + "\" \"" + path + "\" -o \"" + source + "\""
                " -I \"" + SOURCE_DIR + "/include\""
                " -I \"" + SOURCE_DIR + "/examples\""
                " -I \"" + SOURCE_DIR + "/ext/stb\""
                " -I \"" + SOURCE_DIR + "/ext/tinyobjloader\""
                " -I \"" + SOURCE_DIR + "/ext/cgltf\""
                " -I \"" + SOURCE_DIR + "/ext/SDL3-install/include\"" + NULL_OUTPUT
            );

            std::fprintf(stderr, "[vulkan] translating %s\n", path.c_str());
            if (std::system(cmd.c_str()) != 0) {
                std::fprintf(
                    stderr,
                    "[vulkan] valoc failed on %s - see the errors above, or build "
                    "the valoc target if there are none\n",
                    path.c_str()
                );
                return {};
            }
        }
    }

    std::string spvPath = source;

    if (fs::path(source).extension() != ".spv") {
        spvPath = source + ".spv";

        const bool stale =
            !fs::exists(spvPath) || fs::last_write_time(spvPath) < fs::last_write_time(source);

        if (stale) {
            const std::string cmd = shellLine(
                "\"" + std::string(VALO_GLSLC_PATH) + "\""
                " -I \"" + SOURCE_DIR + "/include/render/vulkan\""
                " \"" + source + "\" -o \"" + spvPath + "\" 2>&1"
            );

            std::fprintf(stderr, "[vulkan] compiling %s\n", source.c_str());
            if (std::system(cmd.c_str()) != 0) {
                std::fprintf(
                    stderr,
                    "[vulkan] glslc failed on %s - is ext/vulkan/env.sh sourced? "
                    "(precompiled .spv always works)\n",
                    source.c_str()
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
    const VtxLayout& layout,
    const VkFormat colorFormat
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

    const VkStencilOpState stencilState{
        .failOp         = VK_STENCIL_OP_KEEP,
        .passOp         = vkStencilOpOf(cmd.stencilOp),
        .depthFailOp    = VK_STENCIL_OP_KEEP,
        .compareOp      = vkStencilCompareOf(cmd.stencilTest),
        .compareMask    = 0xFF,
        .writeMask      = 0xFF,
        .reference      = 0
    };

    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType              = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable    = cmd.depthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable   = cmd.depthWrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp     = vkCompareOf(cmd.depthCompare),
        .stencilTestEnable  = usesStencil(cmd) ? VK_TRUE : VK_FALSE,
        .front              = stencilState,
        .back               = stencilState
    };

    const VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable                = cmd.blendmode != BlendMode::Opaque ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor        = cmd.blendmode == BlendMode::Additive ? VK_BLEND_FACTOR_ONE
                                                                           : VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor        = cmd.blendmode == BlendMode::Additive ? VK_BLEND_FACTOR_ONE
                                                                           : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp               = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor        = cmd.blendmode == BlendMode::Additive ? VK_BLEND_FACTOR_ZERO
                                                                           : VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor        = cmd.blendmode == BlendMode::Additive ? VK_BLEND_FACTOR_ONE
                                                                           : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
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

    const VkDynamicState    dynamics[]{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE
    };

    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType              = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount  = 3,
        .pDynamicStates     = dynamics
    };

    const VkPipelineRenderingCreateInfo rendering{
        .sType                      = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount       = 1,
        .pColorAttachmentFormats    = &colorFormat,
        .depthAttachmentFormat      = m_depthFormat,
        .stencilAttachmentFormat    = m_depthFormat
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


VkPipeline VulkanRenderer::pipelineFor(const RenderCommand& cmd, const VtxLayout& layout, const VkFormat colorFormat) {
    const PipelineKey key{ cmd.program, packPipelineState(cmd), colorFormat, layout };

    for (const auto& [k, pipeline] : m_pipelines)
        if (k == key) return pipeline;

    if (cmd.program >= m_programs.size()) return VK_NULL_HANDLE;

    const GPUProgram& prog = m_programs[cmd.program];
    if (!prog.vs || !prog.fs) return VK_NULL_HANDLE;

    const VkPipeline pipeline = createPipeline(cmd, prog, layout, colorFormat);
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
    {
        VkPhysicalDeviceMaintenance3Properties m3{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
        };
        VkPhysicalDeviceProperties2 props2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &m3
        };
        vkGetPhysicalDeviceProperties2(m_ctx.physicalDevice(), &props2);

        const uint32_t perSet = m3.maxPerSetDescriptors > SAMPLER_COUNT
            ? m3.maxPerSetDescriptors - SAMPLER_COUNT : 1u;

        m_maxTextures = std::min({ 
            uint32_t(MAX_TEXTURES),
            props2.properties.limits.maxDescriptorSetSampledImages,
            perSet 
        });

        if (m_maxTextures != MAX_TEXTURES)
            std::fprintf(
                stderr, 
                "[vulkan] texture array capped at %u (device limit)\n",
                m_maxTextures
            );
    }

    const VkDescriptorSetLayoutBinding bindings[2]{
        {
            .binding            = 0,
            .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount    = m_maxTextures,
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
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_maxTextures },
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
                .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
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


bool VulkanRenderer::createTargetDepth(GPUTexture& tex) {
    const VkImageCreateInfo ici{
        .sType          = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType      = VK_IMAGE_TYPE_2D,
        .format         = m_depthFormat,
        .extent         = { tex.width, tex.height, 1 },
        .mipLevels      = 1,
        .arrayLayers    = 1,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .tiling         = VK_IMAGE_TILING_OPTIMAL,
        .usage          = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED
    };

    const VmaAllocationCreateInfo aci{ .usage = VMA_MEMORY_USAGE_AUTO };

    if (vmaCreateImage(
            m_ctx.allocator(), &ici, &aci,
            &tex.depth.image, &tex.depth.allocation, nullptr) != VK_SUCCESS
    ) {
        std::fprintf(stderr, "[vulkan] vmaCreateImage failed for a target's depth buffer\n");
        return false;
    }

    const VkImageViewCreateInfo vci{
        .sType              = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image              = tex.depth.image,
        .viewType           = VK_IMAGE_VIEW_TYPE_2D,
        .format             = m_depthFormat,
        .subresourceRange   = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };

    if (vkCreateImageView(m_ctx.device(), &vci, nullptr, &tex.depth.view) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateImageView failed for a target's depth buffer\n");
        return false;
    }

    tex.depth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}


bool VulkanRenderer::resizeTrackingTargets() {
    const VkExtent2D extent = scaledExtent();

    for (size_t id = 0; id < m_textures.size(); ++id) {
        GPUTexture& tex = m_textures[id];

        if (!tex.isTarget || !tex.tracksWindow) continue;
        if (tex.width == extent.width && tex.height == extent.height) continue;

        const VkFormat format = tex.format;

        const VkImageUsageFlags usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT          |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        destroyTextureNow(tex);

        if (!createImage(extent.width, extent.height, format, 1, usage, tex)) {
            std::fprintf(stderr, "[vulkan] failed to resize a window-tracking target\n");
            return false;
        }

        tex.isTarget     = true;
        tex.tracksWindow = true;

        if (!createTargetDepth(tex)) {
            std::fprintf(stderr, "[vulkan] failed to resize a tracking target's depth buffer\n");
            return false;
        }

        writeTextureDescriptor(uint32_t(id));
    }

    return true;
}


VkExtent2D VulkanRenderer::scaledExtent() const {
    const VkExtent2D e = m_swapchain.extent();
    const float scale  = m_owner->m_cfg.renderScale;
    return {
        std::max(1u, uint32_t(float(e.width)  * scale)),
        std::max(1u, uint32_t(float(e.height) * scale))
    };
}


/*
    Picked up at the start of a frame rather than inside setRenderScale, which
    lives on Renderer and doesn't know a backend exists. Waits for idle because
    the targets it resizes may still be read by a frame in flight.
*/
bool VulkanRenderer::applyRenderScale() {
    const float scale = m_owner->m_cfg.renderScale;
    if (scale == m_appliedScale) return true;

    vkDeviceWaitIdle(m_ctx.device());
    m_appliedScale = scale;

    if (scale != 1.f && m_scaledBackbuffer == 0) {
        const VkExtent2D e = scaledExtent();
        const RenderTargetID id = createTarget(e.width, e.height, true, m_owner->m_cfg.srgbBackbuffer);
        if (id == EMPTY_TEXTURE) {
            std::fprintf(stderr, "[vulkan] failed to create the scaled backbuffer\n");
            return false;
        }
        m_scaledBackbuffer = id;
    }

    return resizeTrackingTargets();
}


RenderTargetID VulkanRenderer::resolveTarget(const RenderTargetID target) const {
    if (target == 0 && m_appliedScale != 1.f && m_scaledBackbuffer != 0) return m_scaledBackbuffer;
    return target;
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

    for (auto& b : m_vertexBuffers)  destroyBufferNow(b);
    for (auto& b : m_indexBuffers)   destroyBufferNow(b);
    for (auto& b : m_uniformBuffers) destroyBufferNow(b);
    for (auto& b : m_storageBuffers) destroyBufferNow(b);
    destroyUploadSlots();
    for (auto& p : m_programs) {
        if (p.vs) vkDestroyShaderModule(m_ctx.device(), p.vs, nullptr);
        if (p.fs) vkDestroyShaderModule(m_ctx.device(), p.fs, nullptr);
    }

    for (auto& t : m_textures) destroyTextureNow(t);
    destroyTextureNow(m_nullTexture);

    m_vertexBuffers.clear();
    m_indexBuffers.clear();
    m_uniformBuffers.clear();
    m_storageBuffers.clear();
    m_programs.clear();
    m_textures.clear();

    m_frames.shutdown();
    m_swapchain.shutdown();
    m_ctx.shutdown();
}


uint32_t VulkanRenderer::slotCount(const ResourceKind kind) const {
    switch (kind) {
        case ResourceKind::Target:        return uint32_t(m_textures.size());
        case ResourceKind::VertexBuffer:  return uint32_t(m_vertexBuffers.size());
        case ResourceKind::IndexBuffer:   return uint32_t(m_indexBuffers.size());
        case ResourceKind::Program:       return uint32_t(m_programs.size());
        case ResourceKind::UniformBuffer: return uint32_t(m_uniformBuffers.size());
        case ResourceKind::StorageBuffer: return uint32_t(m_storageBuffers.size());
    }
    return 0;
}


void VulkanRenderer::waitIdle() {
    if (m_ctx.device()) vkDeviceWaitIdle(m_ctx.device());
}



/* ========================================================================= //
    Lifecycle
// ========================================================================= */

bool VulkanRenderer::init() {
    if (!m_owner->m_cfg.window) {
        std::fprintf(stderr, "[vulkan] RenderConfig::window is null\n");
        return false;
    }

    if (!initDevice(*m_owner->m_cfg.window, m_owner->m_cfg.framesInFlight, m_owner->m_cfg.validation)) return false;

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

    /*
        Set 0 holds both kinds of slot-bound buffer: uniform slots at bindings
        0..7 and storage slots after them. A pipeline layout may contain only
        one push-descriptor set, so they cannot each have their own - valoc
        emits storage blocks at set 0, binding MAX_UNIFORM_SLOTS + slot to match.
    */
    VkDescriptorSetLayoutBinding bindings[MAX_UNIFORM_SLOTS + MAX_STORAGE_SLOTS]{};
    for (uint32_t i = 0; i < MAX_UNIFORM_SLOTS; ++i) {
        bindings[i] = {
            .binding            = i,
            .descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount    = 1,
            .stageFlags         = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        };
    }
    for (uint32_t i = 0; i < MAX_STORAGE_SLOTS; ++i) {
        bindings[MAX_UNIFORM_SLOTS + i] = {
            .binding            = MAX_UNIFORM_SLOTS + i,
            .descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount    = 1,
            .stageFlags         = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        };
    }

    const VkDescriptorSetLayoutCreateInfo dsl{
        .sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags          = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount   = MAX_UNIFORM_SLOTS + MAX_STORAGE_SLOTS,
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

    const uint32_t white = (uint32_t)0xFFFFFFFFu;
    const VkImageUsageFlags usage = 
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1, usage, m_nullTexture)) return false;
    if (!uploadImage(m_nullTexture, &white)) return false;

    m_textures.push_back(GPUTexture{});

    m_depthFormat = pickDepthFormat();
    if (m_depthFormat == VK_FORMAT_UNDEFINED) return false;

    if (!createUploadSlots()) return false;
    return createDepthBuffers();
}


/* ========================================================================= //
    Frame
// ========================================================================= */

void VulkanRenderer::clear(const Color& clearColor, const RenderTargetID target, const float clearDepth) {
    bindTarget(target, VK_ATTACHMENT_LOAD_OP_CLEAR, clearColor, clearDepth);
}


void VulkanRenderer::draw(const RenderCommand& cmd, const RenderTargetID target) {
    if (!bindTarget(target, VK_ATTACHMENT_LOAD_OP_LOAD, {})) return;

    static const VtxLayout noVerts;

    VkBuffer     vertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexBytes  = 0;

    const bool hasVerts =
        cmd.verts != EMPTY_BUFFER &&
        cmd.verts < m_vertexBuffers.size() &&
        currentBuffer(m_vertexBuffers[cmd.verts], vertexBuffer, vertexBytes);

    const VtxLayout& layout = 
        hasVerts ? m_vertexBuffers[cmd.verts].layout : noVerts;

    const VkPipeline pipeline = pipelineFor(cmd, layout, m_currentColorFormat);
    if (!pipeline) return;

    const VkCommandBuffer buf = m_frames.commandBuffer(m_frameIndex);
    vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    if (usesStencil(cmd)) vkCmdSetStencilReference(buf, VK_STENCIL_FACE_FRONT_AND_BACK, cmd.stencilRef);

    if (cmd.uniformSize) {
        /*
            Pushing fewer bytes than the shader's block declares leaves the rest
            of it undefined, so the draw would render with garbage in its last
            fields - often close enough to look right. Skipped instead, once
            reported per program. Software has no such limit, which is why this
            only ever shows up here.
        */
        if (cmd.uniformSize > m_pushConstantSize) {
            bool reported = false;
            for (const ProgramID p : m_oversizedUniforms) if (p == cmd.program) reported = true;

            if (!reported) {
                m_oversizedUniforms.push_back(cmd.program);
                std::fprintf(
                    stderr,
                    "[vulkan] program %u: Uniforms are %zu bytes but this device takes %u "
                    "(Vulkan only guarantees 128) - its draws are skipped. Move frame-wide "
                    "data into a uniform buffer, or bulk data into a storage buffer\n",
                    cmd.program, cmd.uniformSize, m_pushConstantSize
                );
            }
            return;
        }

        vkCmdPushConstants(
            buf, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, uint32_t(cmd.uniformSize),
            cmd.uniforms
        );
    }

    // one push to set 0 for uniform and storage slots together
    constexpr uint32_t SLOT_COUNT = MAX_UNIFORM_SLOTS + MAX_STORAGE_SLOTS;
    VkDescriptorBufferInfo infos[SLOT_COUNT]{};
    VkWriteDescriptorSet writes[SLOT_COUNT]{};
    uint32_t writeCount = 0;

    for (uint32_t slot = 0; slot < MAX_UNIFORM_SLOTS; ++slot) {
        const UniformBufferID id = cmd.uniformBuffers[slot];
        if (id == EMPTY_BUFFER || id >= m_uniformBuffers.size()) continue;

        VkBuffer     ub      = VK_NULL_HANDLE;
        VkDeviceSize ubBytes = 0;
        if (!currentBuffer(m_uniformBuffers[id], ub, ubBytes) || !ub || !ubBytes) continue;

        infos[writeCount] = {
            .buffer = ub,
            .offset = 0,
            .range  = ubBytes
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

    for (uint32_t slot = 0; slot < MAX_STORAGE_SLOTS; ++slot) {
        const StorageBufferID id = cmd.storageBuffers[slot];
        if (id == EMPTY_BUFFER || id >= m_storageBuffers.size()) continue;

        VkBuffer     sb      = VK_NULL_HANDLE;
        VkDeviceSize sbBytes = 0;
        if (!currentBuffer(m_storageBuffers[id], sb, sbBytes) || !sb || !sbBytes) continue;

        infos[writeCount] = {
            .buffer = sb,
            .offset = 0,
            .range  = sbBytes
        };

        writes[writeCount] = {
            .sType              = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding         = MAX_UNIFORM_SLOTS + slot,
            .descriptorCount    = 1,
            .descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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

    const VkExtent2D extent = m_currentExtent;

    const float vpX = cmd.viewport.width  > 0.f ? cmd.viewport.x : 0.f;
    const float vpY = cmd.viewport.height > 0.f ? cmd.viewport.y : 0.f;
    const float vpW = cmd.viewport.width  > 0.f ? cmd.viewport.width  : float(extent.width);
    const float vpH = cmd.viewport.height > 0.f ? cmd.viewport.height : float(extent.height);

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
        if (!vertexBuffer) return;

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(buf, 0, 1, &vertexBuffer, &offset);
    }

    VkBuffer     indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexBytes  = 0;

    const bool hasIndices =
        cmd.indices != EMPTY_BUFFER &&
        cmd.indices < m_indexBuffers.size() &&
        currentBuffer(m_indexBuffers[cmd.indices], indexBuffer, indexBytes);

    if (hasIndices) {
        const uint32_t total = uint32_t(indexBytes / sizeof(uint32_t));

        const uint32_t available = cmd.firstIndex < total ? total - cmd.firstIndex : 0;
        const uint32_t count     = cmd.indexCount == ALL_INDICES ? available : std::min(cmd.indexCount, available);

        if (!count || !indexBuffer) return;

        vkCmdBindIndexBuffer(buf, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(buf, count, cmd.instanceCount, cmd.firstIndex, 0, 0);
        return;
    }

    // without vertices there is nothing to clamp against: a vertex-pulling shader draws exactly what it asks for
    const uint32_t total     = hasVerts ? uint32_t(vertexBytes / layout.stride()) : UINT32_MAX;
    const uint32_t available = cmd.firstIndex < total ? total - cmd.firstIndex : 0;
    const uint32_t count     = cmd.indexCount == ALL_INDICES ? (hasVerts ? available : 0)
                                                             : std::min(cmd.indexCount, available);

    if (count) vkCmdDraw(buf, count, cmd.instanceCount, cmd.firstIndex, 0);
}


void VulkanRenderer::blitToSwapchain(const VkCommandBuffer cmd, GPUTexture& tex) {
    const VkExtent2D extent = m_swapchain.extent();
    const VkImage    swap   = m_swapchain.images()[m_imageIndex];

    const VkImageSubresourceRange color{
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1
    };

    const VkImageMemoryBarrier2 pre[2]{
        {
            .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask           = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask          = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask           = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask          = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout              = tex.layout,
            .newLayout              = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .image                  = tex.image,
            .subresourceRange       = color
        },
        {
            .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask           = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .srcAccessMask          = 0,
            .dstStageMask           = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask          = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout              = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout              = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .image                  = swap,
            .subresourceRange       = color
        }
    };

    const VkDependencyInfo preDep{
        .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount    = 2,
        .pImageMemoryBarriers       = pre
    };
    vkCmdPipelineBarrier2(cmd, &preDep);

    const VkImageBlit2 region{
        .sType              = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource     = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1
        },
        .srcOffsets         = { {0, 0, 0}, {int32_t(tex.width), int32_t(tex.height), 1} },
        .dstSubresource     = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1
        },
        .dstOffsets         = { {0, 0, 0}, {int32_t(extent.width), int32_t(extent.height), 1} }
    };

    const VkBlitImageInfo2 blit{
        .sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage       = tex.image,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage       = swap,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount    = 1,
        .pRegions       = &region,
        .filter         = VK_FILTER_LINEAR
    };
    vkCmdBlitImage2(cmd, &blit);

    const VkImageMemoryBarrier2 post[2]{
        {
            .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask           = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .srcAccessMask          = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask           = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            .dstAccessMask          = 0,
            .oldLayout              = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout              = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .image                  = swap,
            .subresourceRange       = color
        },
        {
            .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask           = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .srcAccessMask          = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask           = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask          = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout              = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout              = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .image                  = tex.image,
            .subresourceRange       = color
        }
    };

    const VkDependencyInfo postDep{
        .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount    = 2,
        .pImageMemoryBarriers       = post
    };
    vkCmdPipelineBarrier2(cmd, &postDep);

    m_swapLayouts[m_imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}


void VulkanRenderer::show(const RenderTargetID target) {
    if (!m_frameOpen) {
        submitPendingUploads();
        return;
    }

    drainRetiredBuffers();

    const uint32_t frame = m_frameIndex;
    const VkCommandBuffer cmd = m_frames.commandBuffer(frame);

    endRenderingIfOpen();

    const RenderTargetID shown = resolveTarget(target);

    const bool fromTarget =
        shown != 0 && shown < m_textures.size() && m_textures[shown].isTarget;

    if (fromTarget) {
        blitToSwapchain(cmd, m_textures[shown]);
    }
    else {
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
        m_swapLayouts[m_imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    vkEndCommandBuffer(cmd);

    const VkSemaphoreSubmitInfo waitSem{
        .sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore              = m_frames.imageAvailable(frame),
        .stageMask              = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT
    };

    const VkSemaphoreSubmitInfo signalSem{
        .sType                  = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore              = m_frames.renderFinished(m_imageIndex),
        .stageMask              = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT
    };

    VkCommandBufferSubmitInfo cmdInfos[2]{};
    uint32_t cmdCount = 0;

    UploadSlot& upload = m_uploads[frame];
    if (upload.open) {
        vkEndCommandBuffer(upload.cmd);
        upload.open = false;

        cmdInfos[cmdCount++] = {
            .sType          = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer  = upload.cmd
        };
    }

    cmdInfos[cmdCount++] = {
        .sType          = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer  = cmd
    };

    const VkSubmitInfo2 submit{
        .sType                      = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount     = 1,
        .pWaitSemaphoreInfos        = &waitSem,
        .commandBufferInfoCount     = cmdCount,
        .pCommandBufferInfos        = cmdInfos,
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

    m_frameIndex = (frame + 1) % m_frames.framesInFlight();
    m_slotReady  = false;
    ++m_owner->m_frame;

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;
    else if (r != VK_SUCCESS) std::fprintf(stderr, "[vulkan] vkQueuePresentKHR failed (%d)\n", r);
}


void VulkanRenderer::flush() { /* no flush for vulkan */}


namespace {

// Says so once per call site rather than once per call, so a frame loop hitting
// an unimplemented entry does not bury everything else in output.
// void todo(const char* fn) {
//     static std::vector<const char*> seen;
//     for (const char* s : seen) if (s == fn) return;
//     seen.push_back(fn);
//     std::fprintf(stderr, "[vulkan] TODO: %s is not implemented\n", fn);
// }

} // namespace


/* ========================================================================= //
    Render targets
// ========================================================================= */

RenderTargetID VulkanRenderer::createTarget(const uint32_t w, const uint32_t h, const bool tracksWindow, const bool srgb) {    
    if (!w || !h) return EMPTY_TEXTURE;
    if (m_textures.size() >= m_maxTextures) {
        std::fprintf(stderr, "[vulkan] texture limit (%u) reached\n", m_maxTextures);
        return EMPTY_TEXTURE;
    }

    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    GPUTexture tex;

    const VkImageUsageFlags usage = 
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT          |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (!createImage(w, h, format, 1, usage, tex)) return EMPTY_TEXTURE;

    tex.isTarget = true;
    tex.tracksWindow = tracksWindow;

    if (!createTargetDepth(tex)) {
        destroyTextureNow(tex);
        return EMPTY_TEXTURE;
    }

    const RenderTargetID id = m_owner->allocSlot(m_textures, m_owner->m_freeTargets, std::move(tex));
    writeTextureDescriptor(id);
    return id;
}


/* ========================================================================= //
    Vertex buffers
// ========================================================================= */

VertexBufferID VulkanRenderer::createVertexBuffer(const void* data, const size_t count, const VtxLayout& layout) {
    GPUBuffer buffer;
    buffer.layout = layout;
    buffer.usage  = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    if (!uploadBuffer(data, count * layout.stride(), buffer.usage, buffer))
        return EMPTY_BUFFER;

    buffer.live = true;
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
    if (!vb.live) return;
    if (newLayout.stride() != 0) vb.layout = newLayout;

    updateDynamic(vb, nullptr, 0, data, count * vb.layout.stride());
}


/* ========================================================================= //
    Index buffers
// ========================================================================= */

IndexBufferID VulkanRenderer::createIndexBuffer(const uint32_t* data, const size_t count) {
    GPUBuffer buffer;
    buffer.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

    if (!uploadBuffer(data, count * sizeof(uint32_t), buffer.usage, buffer))
        return EMPTY_BUFFER;

    buffer.live = true;
    return m_owner->allocSlot(m_indexBuffers, m_owner->m_freeIndexBuffers, std::move(buffer));
}


void VulkanRenderer::updateIndexBuffer(const IndexBufferID indices, const uint32_t* data, size_t count) {
    if (indices >= m_indexBuffers.size()) {
        std::fprintf(stderr, "[vulkan] updateIndexBuffer(): ID out of range\n");
        return;
    }

    GPUBuffer& ib = m_indexBuffers[indices];
    if (!ib.live) return;

    updateDynamic(ib, nullptr, 0, data, count * sizeof(uint32_t));
}


/* ========================================================================= //
    Uniform buffers
// ========================================================================= */

UniformBufferID VulkanRenderer::createUniformBuffer(const void* data, const size_t bytes) {
    GPUBuffer buffer;
    buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    if (!uploadBuffer(data, bytes, buffer.usage, buffer))
        return EMPTY_BUFFER;

    buffer.live = true;
    return m_owner->allocSlot(m_uniformBuffers, m_owner->m_freeUniformBuffers, std::move(buffer));
}


void VulkanRenderer::updateUniformBuffer(const UniformBufferID buffer, const void* data, const size_t bytes) {
    if (buffer >= m_uniformBuffers.size()) {
        std::fprintf(stderr, "[vulkan] updateUniformBuffer(): ID out of range\n");
        return;
    }

    GPUBuffer& ub = m_uniformBuffers[buffer];
    if (!ub.live) return;

    updateDynamic(ub, nullptr, 0, data, bytes);
}



/* ========================================================================= //
    Storage buffers
// ========================================================================= */

/*
    A storage buffer starts with a 16-byte header holding its byte size, and the
    data follows it. valoc translates .size() to that size over the element
    stride rather than GLSL's .length(), which MoltenVK reports as 0 through a
    push descriptor - and trace.frag's BVH bails out on an empty node list, so
    every traced ray missed. valoc's block declares the same four uints.
*/
static constexpr size_t STORAGE_HEADER_BYTES = 16;

static std::vector<uint8_t> withStorageHeader(const void* data, const size_t bytes) {
    std::vector<uint8_t> staged(STORAGE_HEADER_BYTES + bytes, 0);
    const uint32_t size = uint32_t(bytes);
    std::memcpy(staged.data(), &size, sizeof(size));
    if (bytes && data) std::memcpy(staged.data() + STORAGE_HEADER_BYTES, data, bytes);
    return staged;
}


StorageBufferID VulkanRenderer::createStorageBuffer(const void* data, const size_t bytes) {
    const std::vector<uint8_t> staged = withStorageHeader(data, bytes);

    GPUBuffer buffer;
    buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (!uploadBuffer(staged.data(), staged.size(), buffer.usage, buffer))
        return EMPTY_BUFFER;

    buffer.live = true;
    return m_owner->allocSlot(m_storageBuffers, m_owner->m_freeStorageBuffers, std::move(buffer));
}


void VulkanRenderer::updateStorageBuffer(const StorageBufferID buffer, const void* data, const size_t bytes) {
    if (buffer >= m_storageBuffers.size()) {
        std::fprintf(stderr, "[vulkan] updateStorageBuffer(): ID out of range\n");
        return;
    }

    GPUBuffer& sb = m_storageBuffers[buffer];
    if (!sb.live) return;

    uint8_t header[STORAGE_HEADER_BYTES] = {};
    const uint32_t size = uint32_t(bytes);
    std::memcpy(header, &size, sizeof(size));

    updateDynamic(sb, header, STORAGE_HEADER_BYTES, data, bytes);
}


void VulkanRenderer::releaseStorageBuffer(const StorageBufferID id) {
    if (id < m_storageBuffers.size()) retireBuffer(m_storageBuffers[id]);
}



/* ========================================================================= //
    Textures
// ========================================================================= */

TextureID VulkanRenderer::createTexture(const uint32_t w, const uint32_t h, const uint32_t* data, const bool srgb, const bool mips) {
    if (!w || !h || !data) return EMPTY_TEXTURE;
    if (m_textures.size() >= m_maxTextures) {
        std::fprintf(stderr, "[vulkan] texture limit (%u) reached\n", m_maxTextures);
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
    if (texture >= m_textures.size() || !data) return;

    GPUTexture& tex = m_textures[texture];
    if (!tex.image) return;

    const VkCommandBuffer cmd = uploadCommands();
    if (!cmd) return;

    VkBuffer     src    = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    if (!stageUpload(data, VkDeviceSize(tex.width) * tex.height * 4, src, offset)) return;

    recordImageUpload(cmd, tex, src, offset, 0, 0, tex.width, tex.height, VK_IMAGE_LAYOUT_UNDEFINED);
}


void VulkanRenderer::updateTextureRect(
    const TextureID texture,
    const uint32_t x, const uint32_t y,
    const uint32_t w, const uint32_t h,
    const uint32_t* data
) {
    if (texture >= m_textures.size() || !data) return;

    GPUTexture& tex = m_textures[texture];
    if (!tex.image || !w || !h) return;

    if (x + w > tex.width || y + h > tex.height) {
        std::fprintf(
            stderr, "[vulkan] updateTextureRect: %ux%u at (%u,%u) does not fit a %ux%u texture\n",
            w, h, x, y, tex.width, tex.height
        );
        return;
    }

    const VkCommandBuffer cmd = uploadCommands();
    if (!cmd) return;

    VkBuffer     src    = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    if (!stageUpload(data, VkDeviceSize(w) * h * 4, src, offset)) return;

    recordImageUpload(cmd, tex, src, offset, x, y, w, h, tex.layout);
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

void VulkanRenderer::releaseTarget(const RenderTargetID id) { 
    if (id >= m_textures.size()) return;
    clearTextureDescriptor(id);
    destroyTextureNow(m_textures[id]);
}


void VulkanRenderer::releaseVertexBuffer(const VertexBufferID id) {
    if (id < m_vertexBuffers.size()) destroyBufferNow(m_vertexBuffers[id]);
}


void VulkanRenderer::releaseIndexBuffer(const IndexBufferID id) {
    if (id < m_indexBuffers.size()) destroyBufferNow(m_indexBuffers[id]);
}


void VulkanRenderer::releaseUniformBuffer(const UniformBufferID id) { 
    if (id < m_uniformBuffers.size()) retireBuffer(m_uniformBuffers[id]); 
}

void VulkanRenderer::releaseProgram(const ProgramID id) {
    if (id >= m_programs.size()) return;

    // past framesInFlight already (Renderer's deferred destroy), so no frame still uses them
    size_t keep = 0;
    for (size_t i = 0; i < m_pipelines.size(); ++i) {
        if (m_pipelines[i].first.program == id) vkDestroyPipeline(m_ctx.device(), m_pipelines[i].second, nullptr);
        else m_pipelines[keep++] = m_pipelines[i];
    }
    m_pipelines.resize(keep);

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
    return scaledExtent().width;
}


uint32_t VulkanRenderer::height() const {
    return scaledExtent().height;
}


} // namespace valo

#endif // VALO_HAS_VULKAN