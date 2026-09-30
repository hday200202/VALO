#pragma once

#ifdef VALO_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <vector>

#include "VulkanContext.hpp"
#include "../../math/Vec.hpp"

namespace valo {

class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain() { shutdown(); }

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    bool init(VulkanContext& ctx, const Vec2u& size, const bool vsync = true);
    void shutdown();

    VkSwapchainKHR handle() const { return m_swapchain; }
    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }
    uint32_t imageCount() const { return uint32_t(m_images.size()); }
    const std::vector<VkImageView>& views() const { return m_views; }
    const std::vector<VkImage>& images() const { return m_images; }

private:
    VulkanContext*  m_ctx = nullptr;
    VkSwapchainKHR  m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_images;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent = {0, 0};
    std::vector<VkImageView> m_views;
};

} // namespace valo

#endif // VALO_HAS_VULKAN