#pragma once

#ifdef VALO_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <vector>
#include "VulkanContext.hpp"

namespace valo {

class VulkanFrames {
public:
    VulkanFrames() = default;
    ~VulkanFrames() { shutdown(); }

    VulkanFrames(const VulkanFrames&) = delete;
    VulkanFrames& operator=(const VulkanFrames&) = delete;

    bool init(VulkanContext& ctx, const uint32_t framesInFlight, const uint32_t imageCount);
    void shutdown();

    VkCommandPool pool() const { return m_pool; }
    uint32_t framesInFlight() const { return uint32_t(m_commandBuffers.size()); }
    VkCommandBuffer commandBuffer(const uint32_t i) const { return m_commandBuffers[i]; }
    VkSemaphore imageAvailable(const uint32_t frame) const { return m_imageAvailable[frame]; }
    VkFence inFlight(const uint32_t frame) const { return m_inFlight[frame]; }
    VkSemaphore renderFinished(const uint32_t image) const { return m_renderFinished[image]; }

private:
    VulkanContext* m_ctx = nullptr;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore> m_imageAvailable;
    std::vector<VkSemaphore> m_renderFinished;
    std::vector<VkFence> m_inFlight;
};

} // namespace valo

#endif // VALO_HAS_VULKAN