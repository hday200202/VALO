#include "VulkanFrames.hpp"

#ifdef VALO_HAS_VULKAN

#include <cstdio>

namespace valo {

bool VulkanFrames::init(VulkanContext& ctx, const uint32_t framesInFlight, const uint32_t imageCount) {
    m_ctx = &ctx;

    const VkCommandPoolCreateInfo pi{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags              = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex   = ctx.graphicsFamily()
    };

    if (vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_pool) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateCommandPool failed \n");
        return false;
    }

    m_commandBuffers.resize(framesInFlight);

    const VkCommandBufferAllocateInfo ai{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = m_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = framesInFlight
    };

    if (vkAllocateCommandBuffers(ctx.device(), &ai, m_commandBuffers.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkAllocateCommandBuffers failed\n");
        return false;
    }

    m_imageAvailable.resize(framesInFlight);
    m_inFlight.resize(framesInFlight);
    m_renderFinished.resize(imageCount);

    const VkSemaphoreCreateInfo si{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    const VkFenceCreateInfo fi{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (uint32_t f = 0; f < framesInFlight; ++f) {
        if (
            vkCreateSemaphore(ctx.device(), &si, nullptr, &m_imageAvailable[f]) != VK_SUCCESS ||
            vkCreateFence(ctx.device(), &fi, nullptr, &m_inFlight[f]) != VK_SUCCESS
        ) {
            std::fprintf(stderr, "[vulkan] per-frame sync object creation failed (frame %u)\n", f);
            return false;
        }
    }

    for (uint32_t i = 0; i < imageCount; ++i) {
        if (vkCreateSemaphore(ctx.device(), &si, nullptr, &m_renderFinished[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[vulkan] renderFinished semaphore failed (image %u)\n", i);
            return false;
        }
    }

    std::fprintf(
        stderr, "[vulkan] %u frames in flight, %u image semaphores\n", 
        framesInFlight, imageCount
    );
    return true;
}

void VulkanFrames::shutdown() {
    if (m_ctx) {
        vkDeviceWaitIdle(m_ctx->device());

        for (auto s : m_imageAvailable) if (s) vkDestroySemaphore(m_ctx->device(), s, nullptr);
        for (auto s : m_renderFinished) if (s) vkDestroySemaphore(m_ctx->device(), s, nullptr);
        for (auto f : m_inFlight)       if (f) vkDestroyFence(m_ctx->device(), f, nullptr);

        if (m_pool && m_ctx) {
            vkDestroyCommandPool(m_ctx->device(), m_pool, nullptr);
            m_pool = VK_NULL_HANDLE;
        }

        m_ctx = nullptr;
    }

    m_imageAvailable.clear();
    m_renderFinished.clear();
    m_inFlight.clear();
    m_commandBuffers.clear();
}

} // namespace valo

#endif // VALO_HAS_VULKAN