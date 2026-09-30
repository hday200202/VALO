#include "VulkanRenderer.hpp"

#ifdef VALO_HAS_VULKAN

#include <cstdio>
#include <cstdint>

namespace valo {

bool VulkanRenderer::init(Window& window, const uint32_t framesInFlight, const bool validation) {
    m_window = &window;

    if (!m_ctx.init(window, validation)) return false;
    if (!m_swapchain.init(m_ctx, window.getSize(), true)) return false;
    if (!m_frames.init(m_ctx, framesInFlight, m_swapchain.imageCount())) return false;

    return true;
}

bool VulkanRenderer::drawFrame(const Color& clearColor) {
    const VkDevice device = m_ctx.device();
    const uint32_t frame = m_frameIndex;

    if (m_needsRecreate) {
        if (!recreateSwapchain()) return false;
        if (m_needsRecreate) return true;
    }

    const VkFence fence = m_frames.inFlight(frame);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    uint32_t image = 0;
    VkResult r = vkAcquireNextImageKHR(
        device, m_swapchain.handle(), UINT64_MAX,
        m_frames.imageAvailable(frame), VK_NULL_HANDLE, &image
    );

    if (r == VK_ERROR_OUT_OF_DATE_KHR) return recreateSwapchain();
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
        .image                  = m_swapchain.images()[image],
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
        .pImageMemoryBarriers       = &toAttachment
    };
    vkCmdPipelineBarrier2(cmd, &depIn);

    const VkRenderingAttachmentInfo color{
        .sType          = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView      = m_swapchain.views()[image],
        .imageLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue     = { .color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}} }
    };

    const VkRenderingInfo ri{
        .sType                  = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea             = { {0, 0}, m_swapchain.extent() },
        .layerCount             = 1,
        .colorAttachmentCount   = 1,
        .pColorAttachments      = &color
    };

    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);

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
        .image                  = m_swapchain.images()[image],
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
        .semaphore              = m_frames.renderFinished(image),
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

    if (vkQueueSubmit2(m_ctx.graphicsQueue(), 1, &submit, fence) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkQueueSubmit2 failed\n");
        return false;
    }

    const VkSemaphore presentWait = m_frames.renderFinished(image);
    const VkSwapchainKHR chain = m_swapchain.handle();

    const VkPresentInfoKHR pi{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &presentWait,
        .swapchainCount     = 1,
        .pSwapchains        = &chain,
        .pImageIndices      = &image
    };

    r = vkQueuePresentKHR(m_ctx.presentQueue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) m_needsRecreate = true;
    else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkQueuePresentKHR failed (%d)\n", r);
        return false;
    }

    m_frameIndex = (frame + 1) % m_frames.framesInFlight();
    return true;
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
    if (!m_swapchain.init(m_ctx, m_window->getSize(), true)) {
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

    m_needsRecreate = false;
    return true;
}


void VulkanRenderer::shutdown() {
    if (m_ctx.device()) vkDeviceWaitIdle(m_ctx.device());

    m_frames.shutdown();
    m_swapchain.shutdown();
    m_ctx.shutdown();
}

} // namespace valo

#endif // VALO_HAS_VULKAN